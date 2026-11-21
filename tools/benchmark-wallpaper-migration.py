"""Measure matched native/legacy wallpaper folders on the same Windows integrated GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import tempfile
import time
import uuid

import psutil

ROOT = Path(__file__).resolve().parents[1]
# Use the same per-PID and adapter-LUID accounting as the original black-hole comparison.
common = runpy.run_path(str(ROOT / "examples/native-wallpapers/black-hole/benchmark.py"))
Counters, call, keys = common["Counters"], common["call"], common["KEYS"]
player_session = common["player_session"]


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def trial(executable, package, output, native):
    manifest = json.loads((package / "manifest.json").read_bytes())
    with tempfile.TemporaryDirectory(prefix="Action75-Migration-") as temp:
        root = Path(temp)
        state = root / "state"
        state.mkdir()
        shutil.copytree(package, root / "wallpapers-native" / manifest["id"])
        for name in ("assets/defaults/fm-calibration.json", "existing-resources/finalmouse/action75-ue-keymap.json"):
            dst = root / name
            dst.parent.mkdir(parents=True)
            shutil.copy2(ROOT / name, dst)
        (root / "data").mkdir()
        (root / "data/app-settings.json").write_text('{"gpuPreference":"integrated"}')
        name = "Migration-" + uuid.uuid4().hex
        with player_session(executable, root, state, name) as (proc, pipe), Counters() as counters:
            status = call(pipe, "selectPack", id=manifest["id"])
            assert status["headless"] and status["playerPid"] == proc.pid and not status["error"], status
            assert (status["outputWidth"], status["outputHeight"]) == (1920, 720)
            assert "Intel" in status["gpuAdapter"] and bool(status["nativeWorkerPid"]) == native, status
            adapter = status["gpuAdapterLuid"]
            luid = int(adapter.replace(":", ""), 16) if isinstance(adapter, str) else adapter
            processes = [psutil.Process(proc.pid)]
            if native:
                worker = psutil.Process(status["nativeWorkerPid"])
                if worker.ppid() != proc.pid:
                    raise RuntimeError("Rendering worker does not belong to the benchmark player")
                processes.append(worker)
            call(pipe, "show")
            record = dict(pack=manifest["id"], adapter=status["gpuAdapter"], luid=adapter,
                          playerSha256=digest(executable), hostPid=proc.pid,
                          workerPid=status["nativeWorkerPid"] if native else None,
                          gpuEngineScope="Windows GPU Engine counters filtered by PID and adapter LUID", phases={})
            if native:
                record["dllSha256"] = digest(package / manifest["entry"])
            time.sleep(1.5)
            for label, rate, seconds in (("idle", 0, 2), ("typing", 40, 3)):
                start_frame = call(pipe, "status")["framesRendered"]
                counters.read(processes, luid)
                start = time.perf_counter()
                samples, sent = [], 0
                for step in range(1, seconds * 2 + 1):
                    while sent < step * rate // 2:
                        time.sleep(max(0, start + sent / rate - time.perf_counter()))
                        call(pipe, "testTrigger", code=keys[sent % len(keys)])
                        sent += 1
                    time.sleep(max(0, start + step / 2 - time.perf_counter()))
                    readings = counters.read(processes, luid)
                    samples.append(dict(at=time.perf_counter()-start, gpu=sum(sum(p["engines"].values())
                        for p in readings.values()), workingMiB=sum(p["workingSetMiB"] for p in readings.values()),
                        privateMiB=sum(p.memory_info().private for p in processes)/2**20,
                        sharedMiB=sum(p["sharedMiB"] for p in readings.values()), byProcess=readings))
                end = call(pipe, "status")
                frames = end["framesRendered"] - start_frame
                assert end["packId"] == manifest["id"] and not end["error"] and (not rate or frames > 0), end
                previous, engine_ms = 0., 0.
                for s in samples:
                    engine_ms += (s["at"]-previous)*s["gpu"]*10
                    previous = s["at"]
                if rate and not engine_ms:
                    raise RuntimeError("No matched GPU counter samples")
                record["phases"][label] = dict(frames=frames, seconds=previous, keys=sent, fps=frames/previous,
                    gpuEngineMsPerFrame=engine_ms/frames if frames else None, samples=samples)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(record, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path, help="frozen root containing dist and wallpapers-native")
    parser.add_argument("--player", required=True, type=Path, help="candidate player, with its sibling worker")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", default=2, type=int)
    parser.add_argument("packs", nargs="*")
    args = parser.parse_args()
    before = args.baseline.resolve()
    ids = args.packs or sorted(p.name for p in (before / "wallpapers-native").iterdir()
                               if p.name != "black-hole" and (p / "manifest.json").is_file())
    for repeat in range(args.rounds):
        for index, pack_id in enumerate(ids):
            for native in ((False, True) if (repeat + index) % 2 == 0 else (True, False)):
                group = "after" if native else "before"
                print(repeat+1, group, pack_id, flush=True)
                trial(args.player.resolve() if native else before / "dist/Action75Native/Action75Native.exe",
                      (ROOT if native else before) / "wallpapers-native" / pack_id,
                      args.output / str(repeat+1) / group / (pack_id+".json"), native)


if __name__ == "__main__":
    main()
