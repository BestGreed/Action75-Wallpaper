"""Replay output loss in the real player with a private pipe and offscreen D3D11."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def check(condition, detail):
    if not condition:
        raise AssertionError(detail)


def exercise(binary, source, root):
    state = root / "state"
    state.mkdir()
    (state / "native-player.json").write_text(json.dumps({
        "version": 1, "monitorDevice": "keyboard-port-A",
        "monitorTargetPath": "keyboard-port-A", "monitorTargetSerial": "test-keyboard",
    }), encoding="utf-8")
    keymap = Path("existing-resources/finalmouse/action75-ue-keymap.json")
    (root / keymap).parent.mkdir(parents=True)
    shutil.copyfile(source / keymap, root / keymap)
    (root / "data").mkdir()
    shutil.copyfile(source / "assets/defaults/fm-calibration.json", root / "data/fm-calibration.json")
    shutil.copytree(source / "wallpapers-native/afterimage", root / "wallpapers-native/afterimage")
    name = "Action75ReconnectTest-" + uuid.uuid4().hex
    pipe = Path(r"\\.\pipe") / name
    args = [binary, "--headless-test", "--pipe", name, "--root", root, "--state-dir", state]
    with subprocess.Popen(args, creationflags=subprocess.CREATE_NO_WINDOW) as player:
        def call(action="status", **fields):
            payload = dict(action=action, expectedPlayerPid=player.pid, expectedRuntimeRoot=str(root), **fields)
            deadline = time.monotonic() + 12
            while True:
                try:
                    channel = pipe.open("r+b", buffering=0)
                    break
                except OSError as error:
                    if player.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f"Could not connect to test player: {player.poll()}") from error
                    time.sleep(.02)
            with channel:
                channel.write(json.dumps(payload).encode() + b"\n")
                reply = json.loads(channel.readline())
            check(reply["ok"], reply)
            return reply["result"]

        def until(predicate):
            deadline = time.monotonic() + 8
            while True:
                status = call()
                if predicate(status):
                    return status
                check(time.monotonic() < deadline, status)
                time.sleep(.025)

        def topology(device, key=False):
            return call("testMonitor", device=device, firstKey=key)

        def image_digest():
            call("testFrame")
            return hashlib.sha256((state / "test-frame.png").read_bytes()).digest()

        try:
            armed = call()
            check(armed["headless"] and not armed["rawInputRegistered"] and not armed["exitShortcutRegistered"], armed)
            topology("keyboard-port-A")
            selected = call("selectPack", id="afterimage")
            worker = selected["nativeWorkerPid"]
            check(selected["monitorConnected"] and worker, selected)

            # A momentary unplug pauses output and input, then resumes the same worker.
            missing = topology("", key=True)
            check(not missing["monitorConnected"] and missing["keyHandle"] < 0, missing)
            time.sleep(.12)
            short = topology("keyboard-port-A")
            check(short["nativeWorkerPid"] == worker and not short["graphicsSuspended"], short)
            time.sleep(1.65)
            settled = call()
            check(settled["nativeWorkerPid"] == worker and settled["monitorConnected"], settled)
            idle = image_digest()

            # Let the production scheduler expire its grace period and release the device.
            missing = topology("")
            dormant = until(lambda s: s["graphicsSuspended"])
            check(not dormant["nativeWorkerPid"] and not dormant["gpuAdapterLuid"], dormant)
            check(dormant["framesRendered"] == missing["framesRendered"], dormant)
            profile = call("getPackSettings", id="afterimage")
            updated = call("setPackSettings", id="afterimage", expectedRevision=profile["revision"],
                           values={"brightness": 73})
            check(updated["revision"] > profile["revision"], updated)

            # The first physical-input handler runs synchronously with reattachment,
            # before an idle pass could rescue an incorrectly deferred device rebuild.
            connected = topology("keyboard-port-B", key=True)
            check(connected["monitorDevice"] == "keyboard-port-B" and connected["packId"] == "afterimage", connected)
            check(connected["nativeWorkerPid"] and connected["nativeWorkerPid"] != worker, connected)
            check(connected["keyHandle"] > 0 and not connected["graphicsSuspended"], connected)
            check(connected["packSettingsRevision"] == updated["revision"], connected)
            until(lambda s: s["framesRendered"] > dormant["framesRendered"])
            check(image_digest() != idle, "First key did not change the rendered image")
            # Both idle captures must match with the saved settings applied.
            # This rules out a background or decoder change masquerading as a key effect.
            deadline = time.monotonic() + 5
            while image_digest() != idle:
                check(time.monotonic() < deadline, "Wallpaper did not return to its original idle image")
                time.sleep(.1)
            confirmed = call("getPackSettings", id="afterimage")
            check(confirmed["values"] == updated["values"] and confirmed["revision"] == updated["revision"], confirmed)
            persisted = json.loads((state / "wallpaper-settings/afterimage.json").read_text(encoding="utf-8"))
            check(persisted["values"]["brightness"] == 73 and persisted["revision"] == updated["revision"], persisted)
            call("stop")
            player.wait(5)
        finally:
            if player.poll() is None:
                player.kill()
                player.wait()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="action75-reconnect-") as temporary:
        exercise(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(temporary))
