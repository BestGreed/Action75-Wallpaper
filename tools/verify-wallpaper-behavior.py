"""Read back pause, sleep, wake and native key cadence through the real player."""

import argparse
import importlib.util
import json
from pathlib import Path
import time

from PIL import Image, ImageChops


ROOT = Path(__file__).resolve().parents[1]
TRACE = ROOT / "skills/action75-wallpaper-author/scripts/trace.py"


def capture(call, state):
    count = call("testFrame")["framesRendered"]
    with Image.open(state / "test-frame.png") as frame:
        return frame.convert("RGB"), count


def status_frames(call):
    return call("status")["framesRendered"]


def after(call, state, action, **fields):
    previous = status_frames(call)
    call(action, **fields)
    deadline = time.monotonic() + 3
    while status_frames(call) <= previous:
        if time.monotonic() >= deadline:
            raise AssertionError(f"{action} did not wake the renderer")
        time.sleep(0.02)
    return capture(call, state)


def peak_difference(first, next_frame):
    return max(high for _, high in ImageChops.difference(first, next_frame).getextrema())


def idle_frames(call):
    before = status_frames(call)
    time.sleep(0.35)
    return status_frames(call) - before


def verify_field(call, state, pack_id, output):
    after(call, state, "testResize", width=960, height=360)
    muted, _ = after(call, state, "setPackSettings", id=pack_id,
                     values={"backgroundStyle": 1, "backgroundColor": [37, 75, 116], "brightness": 0})
    assert muted.getcolors(maxcolors=2) == [(960 * 360, (37, 75, 116))]
    still = idle_frames(call)
    assert still == 0, (pack_id, still)
    call("testTrigger", code="KeyG")
    unchanged, _ = capture(call, state)
    assert peak_difference(muted, unchanged) == 0

    recolored, _ = after(call, state, "setPackSettings", id=pack_id, values={"backgroundColor": [62, 18, 101]})
    assert recolored.getcolors(maxcolors=2) == [(960 * 360, (62, 18, 101))]
    awake, _ = after(call, state, "setPackSettings", id=pack_id,
                     values={"backgroundStyle": 0, "brightness": 1})
    changed = peak_difference(recolored, awake)
    assert changed > 20 and idle_frames(call) > 0, (pack_id, changed)
    muted.save(output / f"{pack_id}-muted.png")
    awake.save(output / f"{pack_id}-restored.png")
    return {"idleFrames": still, "wakePeakDifference": changed}


def verify_ferrofluid(call, state, pack_id, output):
    rest, _ = after(call, state, "testResize", width=960, height=360)
    call("testPluginEvent", type=1, code="KeyG", x=0.4, y=0.6)
    time.sleep(0.24)
    frozen, _ = after(call, state, "pause")
    assert peak_difference(rest, frozen) > 20, "The magnet did not move the initial surface"
    recolored, _ = after(call, state, "setPackSettings", id=pack_id, values={"tone": 2})
    material_change = peak_difference(frozen, recolored)
    assert material_change > 20, "Paused material did not render"
    reset, _ = after(call, state, "reloadCalibration")
    assert call("status")["paused"] and peak_difference(recolored, reset) == 0
    frozen_again, _ = after(call, state, "setPackSettings", id=pack_id, values={"tone": 0})
    assert peak_difference(frozen, frozen_again) == 0, "The frozen surface changed under the original material"
    still = idle_frames(call)
    assert still == 0, still
    after(call, state, "resume")
    time.sleep(0.1)
    resumed, _ = capture(call, state)
    resume_change = peak_difference(frozen, resumed)
    assert resume_change > 20, "The solver did not advance after resume"
    frozen.save(output / "ferrofluid-frozen.png")
    recolored.save(output / "ferrofluid-recolored.png")
    resumed.save(output / "ferrofluid-resumed.png")
    return {"pausedMaterialPeakDifference": material_change, "inputResetPeakDifference": 0,
            "pausedIdleFrames": still, "resumePeakDifference": resume_change}


def verify_fireworks(call, state, pack_id, output):
    after(call, state, "testResize", width=960, height=360)
    base, _ = after(call, state, "setPackSettings", id=pack_id, values={"keyInterval": 3, "shape": 4})
    base.save(output / "fireworks-idle.png")
    delivered, peaks = [], []
    for index in range(3):
        receipt = call("testTrigger", code="KeyG")
        delivered.append(receipt["acceptedTriggers"])
        time.sleep(0.14)
        frame, _ = capture(call, state)
        peaks.append(peak_difference(base, frame))
        frame.save(output / f"fireworks-key-{index + 1}.png")
    assert delivered == [1, 1, 1], delivered
    assert peaks[:2] == [0, 0] and peaks[2] > 20, peaks
    return {"deliveredKeyEvents": delivered, "peakPixelDifferences": peaks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wallpapers", "--field-packs", type=Path, default=ROOT / "wallpapers-native")
    parser.add_argument("--ferrofluid-pack", type=Path,
                        default=ROOT / "examples/native-wallpapers/ferrofluid/Ferrofluid")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("action75_trace", TRACE)
    trace = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(trace)
    cases = [(args.wallpapers / name, verify_field)
             for name in ("silk-nebula", "gilded-synapse", "harmonic-textile")]
    cases.extend([(args.ferrofluid_pack, verify_ferrofluid),
                  (args.wallpapers / "night-fireworks", verify_fireworks)])
    results = {}
    for pack, verify in cases:
        with trace.session(args.player, ROOT, pack) as (call, state, pack_id):
            results[pack_id] = verify(call, state, pack_id, args.output)
    summary = json.dumps(results, indent=2)
    (args.output / "result.json").write_text(summary, encoding="utf-8")
    print(summary)


if __name__ == "__main__":
    main()
