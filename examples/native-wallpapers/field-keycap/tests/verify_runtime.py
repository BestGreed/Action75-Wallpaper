"""Import the ABI2 packages into an inactive test desktop and check playback."""

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import sys
import tempfile
import time
import uuid

import psutil

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools"))
import release_qa_gui as qa

pipe_request = runpy.run_path(str(REPO / "tools/verify-release.py"))["pipe_request"]
PACKS = ("silk-nebula", "gilded-synapse", "harmonic-textile", "keycap-glow")
KEYS = ("KeyG", "KeyQ", "Space", "KeyP", "KeyA", "KeyJ", "KeyL", "Enter")


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def prepare_root(root, runtime, initial, packages):
    for path in ("data", "state", "dist/Action75Native", "existing-resources/finalmouse", "wallpapers-native"):
        (root / path).mkdir(parents=True, exist_ok=True)
    for name in ("Action75Native.exe", "Action75WallpaperWorker.exe"):
        shutil.copy2(runtime / name, root / "dist/Action75Native" / name)
    calibration = Path("assets/defaults/fm-calibration.json")
    (root / calibration).parent.mkdir(parents=True)
    shutil.copy2(REPO / calibration, root / calibration)
    shutil.copy2(REPO / calibration, root / "data/fm-calibration.json")
    keymap = Path("existing-resources/finalmouse/action75-ue-keymap.json")
    shutil.copy2(REPO / keymap, root / keymap)
    write_json(root / "data/app-settings.json", {"setupComplete": True, "gpuPreference": "integrated"})
    write_json(root / "state/native-player.json", {"version": 1, "packId": initial})
    for name, source in packages.items():
        shutil.copytree(source, root / "wallpapers-native" / name)


@contextmanager
def player(root):
    name = "Action75FieldMigration-" + uuid.uuid4().hex
    (root / "data/player-pipe.txt").write_text(name, encoding="ascii")
    desktop = qa.checked(qa.create_desktop(name, None, None, 0, 0x01FF, None))
    process = None
    try:
        process = qa.spawn_on_desktop([
            root / "dist/Action75Native/Action75Native.exe", "--root", root, "--state-dir", root / "state",
            "--pipe", name, "--headless-test", "--headless-hardware",
        ], root, "WinSta0\\" + name)

        def call(action="status", **data):
            return pipe_request(name, action, expectedPlayerPid=process.pid, expectedRuntimeRoot=str(root), **data)

        status = call()
        assert status["headless"] and status["headlessHardware"] and not status["rawInputRegistered"], status
        assert status["gpuPreference"] == "integrated" and "Intel" in status["gpuAdapter"], status
        yield call
        call("stop")
        assert qa.wait(process.handle, 5000) == 0, "Could not stop test player"
    finally:
        if process:
            if qa.wait(process.handle, 0) != 0:
                qa.terminate(process.handle, 1)
                qa.wait(process.handle, 5000)
            qa.close(process.thread)
            qa.close(process.handle)
        qa.close_desktop(desktop)


def commit(call, pack_id, values):
    profile = call("getPackSettings", id=pack_id)
    return call("setPackSettings", id=pack_id, expectedRevision=profile["revision"], values=values)


def throughput(call, status):
    def processes(snapshot):
        return [psutil.Process(snapshot[name]) for name in ("playerPid", "nativeWorkerPid")]

    def cpu_seconds(process):
        times = process.cpu_times()
        return times.user + times.system

    time.sleep(.25)
    start = call()
    workers = processes(start)
    cpu = [cpu_seconds(process) for process in workers]
    begin = time.perf_counter()
    accepted = 0
    for i in range(100):
        time.sleep(max(0, begin + i * .025 - time.perf_counter()))
        accepted += call("testTrigger", code=KEYS[i % len(KEYS)])["acceptedTriggers"]
    time.sleep(max(0, begin + 2.5 - time.perf_counter()))
    end = call()
    elapsed = time.perf_counter() - begin
    assert end["nativeWorkerPid"] == status["nativeWorkerPid"] and not end["error"], end
    return dict(renderer=end["renderer"], adapter=end["gpuAdapter"], seconds=round(elapsed, 3), inputs=accepted,
                fps=round((end["framesRendered"] - start["framesRendered"]) / elapsed, 2),
                playerCpuSeconds=round(cpu_seconds(workers[0]) - cpu[0], 3),
                workerCpuSeconds=round(cpu_seconds(workers[1]) - cpu[1], 3),
                playerWorkingMiB=round(workers[0].memory_info().rss / 1048576, 2),
                workerWorkingMiB=round(workers[1].memory_info().rss / 1048576, 2))


def test_upgraded_package(call, root, pack_id, profile, output):
    status = call("selectPack", id=pack_id)
    assert status["renderer"] == "D3D12/NativePlugin" and status["nativeWorkerPid"], status
    restored = call("getPackSettings", id=pack_id)
    assert restored["values"] == profile["values"] and restored["revision"] == profile["revision"], restored
    usage = throughput(call, status)
    before = call()
    resized = call("testResize", width=1600, height=600)
    assert (resized["outputWidth"], resized["outputHeight"]) == (1600, 600), resized
    assert resized["nativeWorkerPid"] and resized["nativeWorkerPid"] != before["nativeWorkerPid"], resized
    assert resized["renderer"] == "D3D12/NativePlugin" and resized["packSettingsRevision"] == profile["revision"], resized
    hit = call("testTrigger", code="Space")
    assert hit["acceptedTriggers"] == 1, hit
    qa.wait_for(lambda: call()["framesRendered"] > before["framesRendered"], "a resized D3D12 frame")
    call("testFrame")
    shutil.copy2(root / "state/test-frame.png", output / f"{pack_id}-resized.png")
    keycap = "flowSpeed" in restored["values"]
    idle = {"flowSpeed": 0} if keycap else {"brightness": 0}
    commit(call, pack_id, idle)
    time.sleep(.95 if keycap else .20)
    idle_start = call()["framesRendered"]
    time.sleep(.35)
    assert call()["framesRendered"] == idle_start, "A static scene did not sleep"
    if keycap:
        assert call("testTrigger", code="KeyG")["acceptedTriggers"] == 1
        qa.wait_for(lambda: call()["framesRendered"] > idle_start, "a dormant keycap to wake")
    call("testResize", width=1920, height=720)
    return dict(measurement=usage, migratedRevision=profile["revision"],
                testedStaticSleep=True, resize=[1600, 600], firstKey=hit["acceptedTriggers"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "baseline", "packs", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--click", type=int, nargs=2)
    args = parser.parse_args()
    runtime, baseline, packages, output = [p.resolve() for p in (args.runtime, args.baseline, args.packs, args.output)]
    output.mkdir(parents=True, exist_ok=True)
    report = {"runtimeSha256": {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in runtime.glob("*.exe")},
              "old": {}, "d3d12": {}, "imports": {}}
    with tempfile.TemporaryDirectory(prefix="Action75FieldMigration-") as scratch:
        scratch = Path(scratch)
        old = scratch / "old"
        prepare_root(old, runtime, PACKS[0], {name: baseline / name for name in PACKS})
        profiles = {}
        with player(old) as call:
            for name in PACKS:
                status = call("selectPack", id=name)
                assert status["renderer"] == "D3D11/NativePlugin"
                values = call("getPackSettings", id=name)["values"]
                patch = {"brightness": 1.25, "design": 1} if "flowSpeed" in values else {"brightness": 1.25, "size": 1.2}
                profiles[name] = commit(call, name, patch)
                report["old"][name] = throughput(call, status)
                print(name, report["old"][name], flush=True)

        root = scratch / "candidate"
        prepare_root(root, runtime, "tidal-signal", {"tidal-signal": REPO / "examples/native-wallpapers/tidal-signal/pack"})
        shutil.copytree(old / "state/wallpaper-settings", root / "state/wallpaper-settings")
        with player(root) as call:
            for name in PACKS:
                manifest = root / "wallpapers-native" / name / "manifest.json"
                qa.import_wallpaper(runtime / "Action75Manager.exe", root, packages / name, manifest,
                                    output / f"{name}-gallery.png", click=args.click)
                assert call()["packId"] == "tidal-signal", "Import changed playback"
                call("listPacks")
                assert json.loads(manifest.read_bytes())["id"] == name
                report["imports"][name] = dict(published=True, priorPlayback="tidal-signal")
                print(name, "imported in the isolated gallery", flush=True)
            for name in PACKS:
                report["d3d12"][name] = test_upgraded_package(call, root, name, profiles[name], output)
                print(name, report["d3d12"][name], flush=True)
    write_json(output / "runtime.json", report)


if __name__ == "__main__":
    main()
