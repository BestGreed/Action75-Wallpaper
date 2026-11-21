"""Verify two author ABIs, output loss, key pixels and per-process memory, without a visible window."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import time
import uuid

from PIL import Image
import psutil

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
PORT = "keyboard-port-A"
NEW = "tidal-signal"
OLD = "night-platform-runner"


def check(ok, message):
    if not ok:
        raise AssertionError(message)


def save(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False), encoding="utf-8")


def await_value(fn, predicate, timeout=10):
    end = time.monotonic() + timeout
    while True:
        value = fn()
        if predicate(value):
            return value
        if time.monotonic() > end:
            raise RuntimeError(f"Timed out; last value {value}")
        time.sleep(.04)


def resources(status, hardware):
    pair = [psutil.Process(status["playerPid"]), psutil.Process(status["nativeWorkerPid"])]
    check(pair[1].ppid() == pair[0].pid, "Worker does not belong to this test player")
    values = {}
    if hardware:
        benchmark = runpy.run_path(str(HERE.parent/"black-hole/benchmark.py"))
        with benchmark["Counters"]() as counters:
            luid = status["gpuAdapterLuid"]
            if isinstance(luid, str):
                luid = int(luid.replace(":", ""), 16)
            counters.read(pair, luid)
            time.sleep(.15)
            values = counters.read(pair, luid)
    result = {}
    for role, process in zip(("player", "worker"), pair):
        memory = process.memory_info()
        row = {"pid":process.pid, "privateMiB":memory.private/2**20, "workingSetMiB":memory.rss/2**20}
        if hardware:
            gpu = values[process.pid]
            row.update(dedicatedMiB=gpu["dedicatedMiB"], sharedMiB=gpu["sharedMiB"])
            check(row["dedicatedMiB"] + row["sharedMiB"] > 0, f"No GPU allocation for {role}")
        result[role] = row
    return result


def exercise(player, root, output, hardware):
    library = root/"wallpapers-native"
    state = root/"state"
    state.mkdir()
    (root/"data").mkdir()
    save(state/"native-player.json", dict(version=1,monitorDevice=PORT,monitorTargetPath=PORT,monitorTargetSerial="test-keyboard"))
    save(root/"data/app-settings.json", {"gpuPreference":"integrated"})
    for rel in ("assets/defaults/fm-calibration.json","existing-resources/finalmouse/action75-ue-keymap.json"):
        target=root/rel
        target.parent.mkdir(parents=True)
        shutil.copyfile(REPO/rel,target)
    shutil.copyfile(root/"assets/defaults/fm-calibration.json", root/"data/fm-calibration.json")
    shutil.copytree(HERE/"pack",library/NEW)
    shutil.copytree(REPO/"tools/fixtures/key-game-abi1/pack",library/OLD)
    name = "Action75Dx12Author-" + uuid.uuid4().hex
    cli = [str(player),"--root",str(root),"--pipe",name]
    proc = subprocess.Popen(cli + ["--headless-test","--state-dir",str(state)] + (["--headless-hardware"] if hardware else []),
                            creationflags=subprocess.CREATE_NO_WINDOW)
    pipe = Path(r"\\.\pipe")/name
    def call(action="status", **fields):
        args=dict(action=action,expectedPlayerPid=proc.pid,expectedRuntimeRoot=str(root),**fields)
        deadline=time.monotonic()+10
        while True:
            try:
                channel=pipe.open("r+b",buffering=0)
                break
            except OSError:
                if proc.poll() is not None or time.monotonic()>=deadline: raise
                time.sleep(.025)
        with channel:
            channel.write(json.dumps(args).encode()+b"\n")
            reply=json.loads(channel.readline())
        check(reply["ok"],reply)
        return reply["result"]
    def capture(label):
        call("testFrame")
        dest=output/(label+".png")
        shutil.copyfile(state/"test-frame.png",dest)
        with Image.open(dest) as image:
            return image.convert("RGB")
    def pixel(label, coords, expected):
        def read():
            im=capture(label)
            return im.getpixel((int(coords[0]),int(coords[1])))
        return await_value(read, lambda color:max(abs(v-e) for v,e in zip(color,expected))<=2, timeout=1.35)
    report={"hardware":hardware,"binarySha256":hashlib.sha256(player.read_bytes()).hexdigest(),
            "workerSha256":hashlib.sha256(player.with_name("Action75WallpaperWorker.exe").read_bytes()).hexdigest(),
            "packSha256":{p.relative_to(HERE/"pack").as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted((HERE/"pack").rglob("*")) if p.is_file()}, "stages":{}}
    try:
        initial=call()
        check(initial["headless"] and not initial["rawInputRegistered"], initial)
        call("testMonitor",device=PORT)
        selected=call("selectPack",id=NEW)
        report["adapter"]=selected["gpuAdapter"]
        report["renderer12"]=selected["renderer"]
        check(selected["renderer"] == "D3D12/NativePlugin", selected)
        report["stages"]["dx12"]=resources(selected,hardware)
        old_worker=selected["nativeWorkerPid"]
        key=call("testTrigger",code="KeyG")
        coords=(key["x"],key["y"])
        report["defaultKey"]=pixel("mint",coords,(63,190,161))
        report["stages"]["dx12Active"]=resources(call(),hardware)
        call("setPackSettings",id=NEW,values={"signal_rgb":[240,60,20],"radiance":.5})
        key=call("testTrigger",code="Space")
        space=(key["x"],key["y"])
        report["customKey"]=pixel("coral",space,(120,30,10))
        check(max(capture("coral").getpixel(tuple(map(int,coords))))<75,"Previous key remains lit")
        old=call("selectPack",id=OLD)
        check(old["nativeWorkerPid"]!=old_worker and old["renderer"]=="D3D11/NativePlugin", old)
        report["renderer11"]=old["renderer"]
        report["stages"]["dx11"]=resources(old,hardware)
        call("setPackSettings",id=OLD,values={"amber_lights":True})
        pixel("legacy-amber",(100,546),(255,187,89))
        back=call("selectPack",id=NEW)
        check(back["renderer"]==selected["renderer"] and back["nativeWorkerPid"]!=old["nativeWorkerPid"],back)
        old_worker=back["nativeWorkerPid"]
        call("testMonitor",device="")
        suspended=await_value(call,lambda s:s["graphicsSuspended"] and not s["nativeWorkerPid"])
        check(not suspended["gpuAdapterLuid"],suspended)
        await_value(lambda:psutil.pid_exists(old_worker),lambda exists:not exists)
        report["suspended"]={"workerExited":True,"adapterLuid":suspended["gpuAdapterLuid"],
                             "playerPrivateMiB":psutil.Process(proc.pid).memory_info().private/2**20}
        resumed=call("testMonitor",device="keyboard-port-B",firstKey=True)
        check(resumed["keyHandle"]>0 and resumed["nativeWorkerPid"]!=old_worker,resumed)
        report["firstReconnectKey"]=pixel("reconnect-first-space",space,(120,30,10))
        report["stages"]["reconnected"]=resources(resumed,hardware)
        report["keyPixels"]={"KeyG":coords,"Space":space}
        return report
    finally:
        if proc.poll() is None:
            try:
                call("stop")
                proc.wait(timeout=5)
            finally:
                if proc.poll() is None: proc.kill(); proc.wait()


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--hardware",action="store_true",help="Use configured iGPU and record physical GPU allocation by PID/LUID")
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="Action75-dx12-author-") as tmp:
        result=exercise(args.player.resolve(strict=True),Path(tmp),args.output,args.hardware)
    save(args.output/"report.json",result)
    print(json.dumps(result,ensure_ascii=False))
