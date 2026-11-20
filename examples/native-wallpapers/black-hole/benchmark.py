"""Matched headless iGPU trials of built-in and DLL Event Horizon (Windows/pywin32/psutil)."""
import argparse
import hashlib
import json
from pathlib import Path
from statistics import median
import re
import shutil
import subprocess
import tempfile
import time
import uuid

import psutil
import win32pdh as pdh
import win32file
import pywintypes

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
KEYS = ("KeyQ","KeyW","Space","KeyA","KeyS","ArrowRight","KeyJ","KeyL","Enter")
COUNTERS = {"engines":r"\GPU Engine(*)\Utilization Percentage",
            "shared":r"\GPU Process Memory(*)\Shared Usage","dedicated":r"\GPU Process Memory(*)\Dedicated Usage"}


def save(path, data):
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding="utf-8")


def call(pipe, action, **fields):
    payload = json.dumps(dict(action=action,**fields)).encode() + b"\n"
    deadline = time.monotonic()+10
    while True:
        try:
            h = win32file.CreateFile(pipe,win32file.GENERIC_READ|win32file.GENERIC_WRITE,0,None,
                                      win32file.OPEN_EXISTING,0,None)
            try:
                win32file.WriteFile(h,payload)
                reply = bytearray()
                while not reply.endswith(b"\n"):
                    reply.extend(win32file.ReadFile(h,65536)[1])
            finally:
                h.Close()
            break
        except pywintypes.error:
            if time.monotonic() >= deadline: raise
            time.sleep(.02)
    msg = json.loads(reply)
    if not msg["ok"]: raise RuntimeError(msg)
    return msg["result"]


class Counters:
    def __init__(self):
        self.query = pdh.OpenQuery()
        self.counters = {k:pdh.AddEnglishCounter(self.query,p) for k,p in COUNTERS.items()}

    def read(self, processes, adapter):
        pdh.CollectQueryData(self.query)
        data = {}
        for process in processes:
            mem = process.memory_info()
            cpu = process.cpu_times()
            data[process.pid] = dict(workingSetMiB=mem.rss/2**20,cpuSeconds=cpu.user+cpu.system,
                                     engines={},sharedMiB=0.,dedicatedMiB=0.)
        for kind,counter in self.counters.items():
            try:
                values = pdh.GetFormattedCounterArray(counter,pdh.PDH_FMT_DOUBLE)
            except pywintypes.error:
                continue  # first rate sample needs a prior snapshot
            for key,value in values.items():
                match = re.match(r"pid_(\d+)_luid_(0x[0-9a-f]+)_(0x[0-9a-f]+)_phys_(\d+)(.*)",key,re.I)
                if not match: continue
                pid = int(match[1])
                luid = (int(match[2],16)<<32) | int(match[3],16)
                if pid not in data or luid != adapter: continue
                if kind=="engines":
                    data[pid]["engines"][match[4]+match[5]] = value
                else:
                    data[pid][kind+"MiB"] += value/2**20
        return data


def run(args, native, output):
    pack = HERE/"pack" if native else args.baseline_pack
    manifest = json.loads((pack/"manifest.json").read_bytes())
    if manifest["kind"] != ("native" if native else "blackHole"):
        raise ValueError(f"Wrong pack for this group: {pack}")
    pack_id = manifest["id"]
    with tempfile.TemporaryDirectory(prefix="event-horizon-bench-") as temp:
        root = Path(temp); state = root/"state"; state.mkdir()
        shutil.copytree(pack,root/"wallpapers-native"/pack_id)
        for name in ("assets/defaults/fm-calibration.json","existing-resources/finalmouse/action75-ue-keymap.json"):
            dest = root/name; dest.parent.mkdir(parents=True); shutil.copy2(args.host_root/name,dest)
        (root/"data").mkdir()
        shutil.copy2(root/"assets/defaults/fm-calibration.json",root/"data/fm-calibration.json")
        save(root/"data/app-settings.json",dict(gpuPreference="integrated"))
        save(state/"native-player.json",dict(version=1,fps=60,packId=pack_id))
        pipe = "EventHorizonBench-"+uuid.uuid4().hex
        executable = (args.player if native else args.baseline_player) or args.player
        proc = subprocess.Popen([executable,"--root",root,"--state-dir",state,"--pipe",pipe,
                                "--headless-test","--headless-hardware","--perf-stats"],creationflags=subprocess.CREATE_NO_WINDOW)
        pipe = r"\\.\pipe" + "\\" + pipe
        counters = Counters()
        try:
            status = call(pipe,"status")
            assert status["playerPid"] == proc.pid and status["headless"]
            status = call(pipe,"selectPack",id=pack_id)
            assert status["packId"] == pack_id and "Intel" in status["gpuAdapter"] and status["error"] is None, status
            assert (status["outputWidth"],status["outputHeight"]) == (1920,720)
            luid_raw = status["gpuAdapterLuid"]
            if isinstance(luid_raw,str):
                luid = int(luid_raw.replace(":",""),16)
            else: luid = int(luid_raw)
            parent = psutil.Process(proc.pid)
            processes = [parent, *parent.children(recursive=True)]
            if native and len(processes)<2: raise RuntimeError("Missing native rendering worker")
            report = dict(pack=pack_id,adapter=status["gpuAdapter"],adapterLuid=luid_raw,
                          executable=str(executable),sha256=hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
                          packFileSha256={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in pack.iterdir() if f.is_file()},
                          resolution=[1920,720],processes={p.pid:p.name() for p in processes}, phases={})
            if native:
                report["workerSha256"] = hashlib.sha256((executable.parent/"Action75WallpaperWorker.exe").read_bytes()).hexdigest()
            counters.read(processes,luid)
            time.sleep(2)
            for label, hz in (("idle",0),("typing",40)):
                begin = call(pipe,"perfReset")
                wall = time.perf_counter()
                prior = counters.read(processes,luid)
                snapshots = []
                sent = 0
                for step in range(1,args.seconds*2+1):
                    until = wall + step*.5
                    while hz and sent < step * hz//2:
                        time.sleep(max(0,wall+sent/hz-time.perf_counter()))
                        call(pipe,"testTrigger",code=KEYS[sent%len(KEYS)])
                        sent += 1
                    time.sleep(max(0,until-time.perf_counter()))
                    snapshots.append(dict(elapsed=time.perf_counter()-wall,processes=counters.read(processes,luid)))
                end = call(pipe,"perfRead")
                elapsed = snapshots[-1]["elapsed"]
                frames = end["status"]["framesRendered"]-begin["status"]["framesRendered"]
                if frames <= 0: raise RuntimeError("Animation did not advance")
                if not any(any(p["engines"].values()) for p in snapshots[-1]["processes"].values()):
                    raise RuntimeError(f"No matched GPU engine data for LUID {luid_raw}")
                metrics = {}
                for p in processes:
                    dt0, engine_ms = 0.,0.
                    for sample in snapshots:
                        engine_ms += sum(sample["processes"][p.pid]["engines"].values())*(sample["elapsed"]-dt0)*10
                        dt0=sample["elapsed"]
                    last=snapshots[-1]["processes"][p.pid]
                    metrics[p.pid]=dict(engineMsPerPresentedFrame=engine_ms/frames,
                        cpuOneCorePercent=(last["cpuSeconds"]-prior[p.pid]["cpuSeconds"])*100/elapsed)
                report["phases"][label]=dict(seconds=elapsed,frames=frames,fps=frames/elapsed,keys=sent,
                    byProcess=metrics,sumEngineMsPerFrame=sum(x["engineMsPerPresentedFrame"] for x in metrics.values()),
                    peakWorkingSetMiB=max(sum(p["workingSetMiB"] for p in s["processes"].values()) for s in snapshots),
                    peakSharedMiB=max(sum(p["sharedMiB"] for p in s["processes"].values()) for s in snapshots),
                    peakDedicatedMiB=max(sum(p["dedicatedMiB"] for p in s["processes"].values()) for s in snapshots),
                    hostGpuTimestamps=end["performance"]["gpuMs"],samples=snapshots)
                print(label,round(frames/elapsed,2),flush=True)
            save(output,report)
        finally:
            pdh.CloseQuery(counters.query)
            if proc.poll() is None:
                try:
                    call(pipe,"stop",expectedPlayerPid=proc.pid,expectedRuntimeRoot=str(root))
                    proc.wait(timeout=4)
                finally:
                    if proc.poll() is None: proc.kill(); proc.wait()


def summarize(folder):
    result = {}
    for label in ("baseline","native"):
        cases = [json.loads(f.read_bytes()) for f in sorted(folder.glob(label+"-*.json"))]
        if not cases: continue
        case = cases[0]
        assert all(r["packFileSha256"] == case["packFileSha256"] and r["sha256"] == case["sha256"]
                   and r["adapterLuid"] == case["adapterLuid"] for r in cases)
        phases = {}
        for phase in ("idle","typing"):
            group = [r["phases"][phase] for r in cases]
            phases[phase] = {k:median(r[k] for r in group) for k in
                ("fps","sumEngineMsPerFrame","peakWorkingSetMiB","peakSharedMiB","peakDedicatedMiB")}
            phases[phase]["sumEngineBusyPct"] = median(r["sumEngineMsPerFrame"]*r["fps"]/10 for r in group)
            phases[phase]["cpuOneCorePct"] = median(sum(p["cpuOneCorePercent"] for p in r["byProcess"].values()) for r in group)
        result[label] = dict(trials=len(cases),adapter=case["adapter"],luid=case["adapterLuid"],
            playerSha256=case["sha256"],packFiles=case["packFileSha256"],phases=phases)
        if "workerSha256" in case: result[label]["workerSha256"]=case["workerSha256"]
    save(folder/"summary.json",result)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--player",type=Path,required=True)
    p.add_argument("--baseline-player",type=Path)
    p.add_argument("--baseline-pack",type=Path,default=ROOT/"wallpapers-native/black-hole")
    p.add_argument("--host-root",type=Path,default=ROOT)
    p.add_argument("--seconds",type=int,default=6)
    p.add_argument("--trials",type=int,default=3)
    p.add_argument("--only",choices=("baseline","native","both"),default="both")
    p.add_argument("--output",type=Path,required=True)
    options=p.parse_args()
    options.output.mkdir(parents=True,exist_ok=True)
    for i in range(options.trials):
        for label in (("baseline","native") if i%2==0 else ("native","baseline")):
            if options.only not in ("both",label): continue
            run(options,label=="native",options.output/f"{label}-{i+1}.json")

    summarize(options.output)
