"""Check a native package in a private Action75 runtime and settings directory."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import tempfile
import time
import uuid

from PIL import Image, ImageChops


HERE = Path(__file__).resolve().parent.parent
io = runpy.run_path(str(HERE / "benchmark.py"))
call, player_session, save = (io[name] for name in ("call", "player_session", "save"))


def capture(request, state, output, name):
    request("testFrame")
    path = output / f"{name}.png"
    shutil.copy2(state / "test-frame.png", path)
    with Image.open(path) as frame:
        return frame.convert("RGB")


def expect_renderer(request, pid, pack_id):
    status = request("status")
    assert status["headless"] and status["playerPid"] == pid and not status["error"], status
    assert status["packId"] == pack_id and status["renderer"] == "D3D12/NativePlugin", status
    assert status["nativeWorkerPid"] > 0, status
    return status


def verify_probe(request, state, output):
    time.sleep(.12)
    idle = capture(request, state, output, "idle")
    bg = idle.getpixel((0, 0))
    assert idle.getbbox() and all(lo == hi for lo, hi in idle.getextrema()), idle.getextrema()
    key = request("testTrigger", code="KeyG")
    assert key["acceptedTriggers"] == 1, key
    time.sleep(.08)
    flash = capture(request, state, output, "flash")
    center = (int(key["x"]), int(key["y"]))
    flash_color = flash.getpixel(center)
    assert flash_color != bg
    assert ImageChops.difference(idle, flash).getbbox() == (
        max(0, center[0] - 44), max(0, center[1] - 44),
        min(idle.width, center[0] + 44), min(idle.height, center[1] + 44))
    time.sleep(.50)
    settled = capture(request, state, output, "settled")
    assert not ImageChops.difference(idle, settled).getbbox(), "Flash did not expire"
    before = request("status")["framesRendered"]
    time.sleep(.20)
    assert request("status")["framesRendered"] == before, "Probe did not go to sleep"
    return {"trigger": key, "flashRgb": flash_color, "idleRgb": bg, "settledAndSleeping": True}


def verify_black_hole(request, state, output, pack_id):
    fields = request("getPackSettings", id=pack_id)
    controls = [c["id"] for group in fields["schema"]["groups"] for c in group["controls"]]
    assert len(controls) == 26 and len(set(controls)) == 26, controls
    request("setPackSettings", id=pack_id, values={
        "skyMode": 3, "backgroundBrightness": 0.0, "diskBrightness": 0.25,
        "starBrightness": 2.0, "starSize": 1.2, "centerX": 58.0, "centerY": 46.0})
    time.sleep(.10)
    before = capture(request, state, output, "before-key")
    key = request("testTrigger", code="KeyQ")
    assert key["acceptedTriggers"] == 1, key
    time.sleep(.17)
    after = capture(request, state, output, "key-star")
    x, y = int(key["x"]), int(key["y"])
    box = ImageChops.difference(before, after).crop((x - 24, y - 24, x + 25, y + 25)).getbbox()
    assert box and box[0] <= 24 < box[2] and box[1] <= 24 < box[3], (box, key)

    request("setPackSettings", id=pack_id, values={
        "skyMode": 0, "backgroundBrightness": 1.0, "diskBrightness": 1.0})
    request("testPluginEvent", type=3, x=.58, y=.46)
    request("testPluginEvent", type=4, x=.42, y=.66)
    request("testPluginEvent", type=5, x=.42, y=.66)
    expected_center = {"centerX": 42.0, "centerY": 66.0}
    values = request("getPackSettings", id=pack_id)["values"]
    assert expected_center.items() <= values.items(), values
    saved = json.loads((state / "wallpaper-settings" / f"{pack_id}.json").read_bytes())
    assert expected_center.items() <= saved["values"].items(), saved
    request("pause")
    request("resume")
    time.sleep(.10)
    capture(request, state, output, "dragged-nebula")
    return {"trigger": key, "keyCropChangedBounds": box, "controlIds": controls, "dragCommit": expected_center}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("player", "host-root", "pack", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--scene", choices=("black-hole", "probe"), required=True)
    args = parser.parse_args()
    pack_id = json.loads((args.pack / "manifest.json").read_bytes())["id"]
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="Action75-Native-Test-") as scratch:
        root = Path(scratch)
        state = root / "state"
        state.mkdir()
        shutil.copytree(args.pack, root / "wallpapers-native" / "candidate")
        for file in ("assets/defaults/fm-calibration.json",
                     "existing-resources/finalmouse/action75-ue-keymap.json"):
            destination = root / file
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.host_root / file, destination)
        (root / "data").mkdir()
        shutil.copy2(root / "assets/defaults/fm-calibration.json", root / "data/fm-calibration.json")
        save(state / "native-player.json", {"version": 1, "fps": 60, "packId": pack_id})
        name = "Action75NativeQa-" + uuid.uuid4().hex
        with player_session(args.player.resolve(), root, state, name, perf_stats=False) as (process, pipe):
            def request(action, **data):
                return call(pipe, action, **data)

            request("selectPack", id=pack_id)
            status = expect_renderer(request, process.pid, pack_id)
            if args.scene == "probe":
                evidence = verify_probe(request, state, args.output)
            else:
                evidence = verify_black_hole(request, state, args.output, pack_id)
        if args.scene == "black-hole":
            with player_session(args.player.resolve(), root, state, name, perf_stats=False) as (process, pipe):
                expect_renderer(request, process.pid, pack_id)
                loaded = request("getPackSettings", id=pack_id)["values"]
                assert evidence["dragCommit"].items() <= loaded.items(), loaded
                evidence["persistedAfterRestart"] = True
    for file, role in ((args.player, "player"), (args.player.with_name("Action75WallpaperWorker.exe"), "worker"),
                       (args.pack / "wallpaper.dll", "dll")):
        evidence[role + "Sha256"] = hashlib.sha256(file.read_bytes()).hexdigest()
    evidence.update(pack=pack_id, adapter=status["gpuAdapter"], renderer=status["renderer"], passed=True)
    save(args.output / "player.json", evidence)
    print(json.dumps(evidence, ensure_ascii=False))


if __name__ == "__main__":
    main()
