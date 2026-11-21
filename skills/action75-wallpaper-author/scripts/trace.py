"""Exercise a pack with isolated keyboard events and read back real frames at chosen times."""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


@contextmanager
def session(player, host_root, pack, hardware=False):
    with tempfile.TemporaryDirectory(prefix="Action75-frames-") as scratch:
        root = Path(scratch)
        state = root / "test-state"
        state.mkdir()
        pack_id = json.loads((pack / "manifest.json").read_text(encoding="utf-8-sig"))["id"]
        shutil.copytree(pack, root / "wallpapers-native" / "candidate")
        for rel in ("assets/defaults/fm-calibration.json", "existing-resources/finalmouse/action75-ue-keymap.json"):
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(host_root / rel, target)
        (root / "data").mkdir()
        shutil.copy2(root / "assets/defaults/fm-calibration.json",root / "data/fm-calibration.json")
        write_json(state / "native-player.json", {"version": 1, "packId": pack_id})
        cli = [str(player.resolve(strict=True)), "--root", str(root), "--pipe", "Author-" + uuid.uuid4().hex]
        process = subprocess.Popen(cli + ["--headless-test", "--state-dir", str(state)] +
                                   (["--headless-hardware"] if hardware else []),
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        def call(action, **fields):
            reply = subprocess.run(cli + ["--command", json.dumps(dict(action=action, **fields))],
                                   capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
            message = json.loads(reply.stdout.decode("utf-8-sig")) if reply.stdout else {}
            if reply.returncode or not message.get("ok"):
                raise RuntimeError(f"{action}: {message.get('error') or reply.stderr or reply.returncode}")
            return message["result"]
        try:
            deadline = time.monotonic() + 12
            while True:
                try:
                    status = call("status")
                    break
                except RuntimeError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(.05)
            if status.get("playerPid") != process.pid or not status.get("headless"):
                raise RuntimeError("Wrong offscreen player identity")
            call("selectPack", id=pack_id)
            yield call, state, pack_id
        finally:
            if process.poll() is None:
                try:
                    call("stop")
                    process.wait(timeout=3)
                except (RuntimeError, OSError, subprocess.SubprocessError):
                    process.kill()
                    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("player", "host-root", "pack", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--values", default="{}", help="JSON patch keyed by the author's control IDs")
    parser.add_argument("--code", default="KeyG")
    parser.add_argument("--keys", type=int, default=1)
    parser.add_argument("--frames-ms", type=int, nargs="+", default=[100, 450, 1000])
    parser.add_argument("--hardware", action="store_true")
    args = parser.parse_args()
    if args.keys < 1 or min(args.frames_ms) < 0:
        parser.error("Supply positive key count and nonnegative capture times")
    args.output.mkdir(parents=True, exist_ok=True)
    with session(args.player, args.host_root, args.pack, args.hardware) as (call, state, pack_id):
        values = json.loads(args.values)
        if values:
            call("setPackSettings", id=pack_id, values=values)
        keys = [call("testTrigger", code=args.code) for _ in range(args.keys)]
        start = time.monotonic()
        frames = []
        for t in sorted(set(args.frames_ms)):
            time.sleep(max(0, t / 1000 - (time.monotonic() - start)))
            elapsed = time.monotonic() - start
            ack = call("testFrame")
            name = f"frame-{t:05}.png"
            shutil.copy2(state / "test-frame.png", args.output / name)
            frames.append(dict(file=name, elapsedSeconds=elapsed, **ack))
        write_json(args.output / "trace.json", dict(pack=pack_id, keys=keys, frames=frames))


if __name__ == "__main__":
    main()
