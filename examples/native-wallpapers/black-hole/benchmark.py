"""Matched headless hardware trials of two Event Horizon packages."""
import argparse
from contextlib import contextmanager
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


@contextmanager
def player_session(executable, root, state, name, *, perf_stats=True):
    cli = [executable, "--root", root, "--pipe", name]
    pipe = "\\\\.\\pipe\\" + name
    args = cli + ["--state-dir", state, "--headless-test", "--headless-hardware"]
    if perf_stats:
        args.append("--perf-stats")
    proc = subprocess.Popen(args,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        yield proc, pipe
    finally:
        if proc.poll() is None:
            try:
                stop = dict(action="stop", expectedPlayerPid=proc.pid, expectedRuntimeRoot=str(root))
                subprocess.run(cli + ["--command", json.dumps(stop)], check=True, timeout=4,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               creationflags=subprocess.CREATE_NO_WINDOW)
                proc.wait(timeout=4)
            except (OSError, subprocess.SubprocessError):
                pass
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=4)


class Counters:
    def __init__(self):
        self.query = pdh.OpenQuery()
        try:
            self.counters = {k:pdh.AddEnglishCounter(self.query,p) for k,p in COUNTERS.items()}
        except BaseException:
            pdh.CloseQuery(self.query)
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        pdh.CloseQuery(self.query)

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
    pack = args.candidate_pack if native else args.baseline_pack
    manifest = json.loads((pack/"manifest.json").read_bytes())
    uses_worker = manifest["kind"] == "native"
    if not uses_worker and (native or manifest["kind"] != "blackHole"):
        raise ValueError(f"Wrong pack for this group: {pack}")
    dx12 = manifest.get("abi") == 2
    # Queries on the host's D3D11 context were useful for the legacy built-in
    # comparison. Use the same PID counters for both sides of a native-ABI pair.
    gpu_queries = not dx12 and not uses_worker
    pack_id = manifest["id"]
    with tempfile.TemporaryDirectory(prefix="event-horizon-bench-") as temp:
        root = Path(temp); state = root/"state"; state.mkdir()
        shutil.copytree(pack,root/"wallpapers-native"/pack_id)
        for name in ("assets/defaults/fm-calibration.json","existing-resources/finalmouse/action75-ue-keymap.json"):
            dest = root/name; dest.parent.mkdir(parents=True); shutil.copy2(args.host_root/name,dest)
        (root/"data").mkdir()
        shutil.copy2(root/"assets/defaults/fm-calibration.json",root/"data/fm-calibration.json")
        save(root/"data/app-settings.json",dict(gpuPreference=args.gpu))
        save(state/"native-player.json",dict(version=1,fps=60,packId=pack_id))
        name = "EventHorizonBench-"+uuid.uuid4().hex
        executable = (args.player if native else args.baseline_player) or args.player
        with player_session(executable, root, state, name, perf_stats=gpu_queries) as (proc, pipe), Counters() as counters:
            status = call(pipe,"status")
            assert status["playerPid"] == proc.pid and status["headless"]
            status = call(pipe,"selectPack",id=pack_id)
            assert status["packId"] == pack_id and status["error"] is None, status
            assert status["gpuPreference"] == args.gpu and "Basic Render Driver" not in status["gpuAdapter"], status
            if uses_worker:
                assert status["renderer"] == ("D3D12/NativePlugin" if dx12 else "D3D11/NativePlugin"), status
            assert (status["outputWidth"],status["outputHeight"]) == (1920,720)
            luid_raw = status["gpuAdapterLuid"]
            if isinstance(luid_raw,str):
                luid = int(luid_raw.replace(":",""),16)
            else: luid = int(luid_raw)
            parent = psutil.Process(proc.pid)
            processes = [parent]
            if uses_worker:
                worker_pid = status.get("nativeWorkerPid")
                if not worker_pid:
                    raise RuntimeError("Missing native rendering worker")
                worker = psutil.Process(worker_pid)
                if worker.ppid() != proc.pid:
                    raise RuntimeError("Rendering worker does not belong to the benchmark player")
                processes.append(worker)
            report = dict(pack=pack_id,adapter=status["gpuAdapter"],adapterLuid=luid_raw,
                          executable=str(executable),sha256=hashlib.sha256(Path(executable).read_bytes()).hexdigest(),
                          packFileSha256={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in pack.iterdir() if f.is_file()},
                          resolution=[1920,720],processes={p.pid:p.name() for p in processes},
                          hostPid=proc.pid,workerPid=processes[-1].pid if uses_worker else None,
                          renderer=status["renderer"],
                          gpuEngineScope="Windows GPU Engine counters filtered by PID and adapter LUID",
                          timestampScope="player D3D11 context" if gpu_queries else None,phases={})
            if uses_worker:
                report["workerSha256"] = hashlib.sha256((executable.parent/"Action75WallpaperWorker.exe").read_bytes()).hexdigest()
            counters.read(processes,luid)
            time.sleep(2)
            for label, hz in (("idle",0),("typing",40)):
                begin = call(pipe,"perfReset")["status"] if gpu_queries else call(pipe,"status")
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
                end = call(pipe,"perfRead") if gpu_queries else {"status": call(pipe,"status")}
                elapsed = snapshots[-1]["elapsed"]
                assert end["status"]["packId"] == pack_id and not end["status"]["error"], end["status"]
                frames = end["status"]["framesRendered"]-begin["framesRendered"]
                if frames <= 0: raise RuntimeError("Animation did not advance")
                if not any(p["engines"] for sample in snapshots for p in sample["processes"].values()):
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
                    hostGpuTimestamps=end["performance"]["gpuMs"] if gpu_queries else None,samples=snapshots)
                print(label,round(frames/elapsed,2),flush=True)
            save(output,report)


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
    if "baseline" in result and "native" in result and result["baseline"]["luid"] != result["native"]["luid"]:
        raise RuntimeError("The two groups selected different GPU adapters")
    save(folder/"summary.json",result)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--player",type=Path,required=True)
    p.add_argument("--baseline-player",type=Path)
    p.add_argument("--baseline-pack",type=Path,default=ROOT/"wallpapers-native/black-hole")
    p.add_argument("--candidate-pack",type=Path,default=HERE/"pack")
    p.add_argument("--gpu",choices=("integrated","discrete"),default="integrated")
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
