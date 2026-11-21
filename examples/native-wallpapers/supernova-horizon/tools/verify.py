"""Verify the packaged scene through the Action75 author's offscreen interface."""

import argparse
import importlib.util
import json
from pathlib import Path
import time

from PIL import Image, ImageChops


def frame(state, call, output, name):
    call("testFrame")
    with Image.open(state / "test-frame.png") as source:
        image = source.convert("RGB")
    image.save(output / (name + ".png"))
    return image


def difference(before, after):
    return ImageChops.difference(before, after)


def peak(image):
    return max(high for _, high in image.getextrema())


def threshold_bounds(image, minimum):
    channels = image.split()
    brightest = ImageChops.lighter(channels[0], ImageChops.lighter(channels[1], channels[2]))
    return brightest.point(lambda value: 255 if value >= minimum else 0).getbbox()


def check_burst(call, state, output, baseline, code, label):
    key = call("testTrigger", code=code)
    assert key["acceptedTriggers"] == 1, key
    flash = frame(state, call, output, label + "-flash")
    x, y = round(key["x"]), round(key["y"])
    delta = difference(baseline, flash)
    changed = threshold_bounds(delta, 12)
    center_peak = peak(delta.crop((x - 9, y - 9, x + 10, y + 10)))
    assert changed and center_peak >= 60, (code, changed, center_peak)
    left, top, right, bottom = changed
    assert max(x - left, y - top, right - x, bottom - y) < baseline.height * 0.27, (code, changed)
    return key, changed, center_peak


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("skill", "player", "host-root", "pack", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--hardware", action="store_true")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("action75_trace", args.skill / "scripts" / "trace.py")
    trace = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(trace)
    args.output.mkdir(parents=True, exist_ok=True)
    with trace.session(args.player, args.host_root, args.pack, args.hardware) as (call, state, pack_id):
        status = call("status")
        assert status["renderer"] == "D3D12/NativePlugin", status["renderer"]
        assert status["nativeWorkerPid"] and status["nativeWorkerPid"] != status["playerPid"], status
        baseline = frame(state, call, args.output, "nebula")
        key, bounds, peak_t = check_burst(call, state, args.output, baseline, "KeyT", "nebula-t")
        time.sleep(0.13)
        frame(state, call, args.output, "nebula-t-shock")
        time.sleep(1.05)
        settled = frame(state, call, args.output, "settled")
        assert difference(baseline, settled).getbbox() is None
        rest_before = call("status")["framesRendered"]
        time.sleep(0.45)
        rest_after = call("status")["framesRendered"]
        assert rest_before == rest_after, (rest_before, rest_after)

        def change(values, label):
            reply = call("setPackSettings", id=pack_id, values=values)
            assert all(reply["values"][key] == value for key, value in values.items()), reply["values"]
            time.sleep(0.12)
            return frame(state, call, args.output, label)

        void = change({"skyMode": 1}, "void")
        assert peak(difference(baseline, void)) >= 26
        disk_probe = (round(baseline.width * 0.530), round(baseline.height * 0.443))
        assert baseline.getpixel(disk_probe) == void.getpixel(disk_probe)
        second, second_bounds, peak_m = check_burst(call, state, args.output, void, "KeyM", "void-m")
        assert second["x"] > key["x"] + baseline.width * 0.10
        time.sleep(1.12)

        dark_disk = change({"diskBrightness": 0}, "void-no-disk")
        disk_difference = difference(void, dark_disk)
        region = (int(baseline.width * 0.42), 0, int(baseline.width * 0.68), baseline.height)
        assert peak(disk_difference.crop(region)) >= 90
        assert peak(disk_difference.crop((0, 0, baseline.width // 4, baseline.height))) == 0
        restored = change({"diskBrightness": 1, "skyMode": 0}, "nebula-restored")
        assert difference(baseline, restored).getbbox() is None

        sky_deltas = {}
        for name, value, default in (
            ("skyBrightness", 0.5, 1.0),
            ("skyHue", 120, 0),
            ("skySaturation", 0, 1.0),
            ("skyColor", [110, 255, 145], [255, 255, 255]),
        ):
            adjusted = change({name: value}, name)
            sky_deltas[name] = peak(difference(baseline, adjusted))
            assert sky_deltas[name] >= 10, (name, sky_deltas[name])
            reply = call("setPackSettings", id=pack_id, values={name: default})
            assert reply["values"][name] == default

        call("setPackSettings", id=pack_id, values={"novaBrightness": 0})
        time.sleep(0.12)
        call("testTrigger", code="KeyT")
        muted = frame(state, call, args.output, "muted-trigger")
        assert difference(baseline, muted).getbbox() is None

        def measure_nova(values, label, delay):
            call("setPackSettings", id=pack_id, values=values)
            reply = call("testTrigger", code="KeyT")
            assert reply["acceptedTriggers"] == 1, reply
            time.sleep(delay)
            captured = frame(state, call, args.output, label)
            time.sleep(1.5)
            return difference(baseline, captured)

        tinted = measure_nova({"novaBrightness": 1, "novaColor": [255, 0, 0]}, "nova-tint", 0)
        tint_peaks = [high for _, high in tinted.getextrema()]
        assert tint_peaks[0] > 60 and tint_peaks[1:] == [0, 0], tint_peaks
        narrow = measure_nova({"novaColor": [255, 255, 255], "novaRadius": 0.55}, "nova-narrow", 0.15)
        wide = measure_nova({"novaRadius": 1.45}, "nova-wide", 0.15)
        widths = []
        for delta in (narrow, wide):
            extent = threshold_bounds(delta, 16)
            assert extent, "Expected to see the shock wave"
            widths.append(extent[2] - extent[0])
        assert widths[1] > widths[0] * 1.6, widths
        short = measure_nova({"novaRadius": 1, "novaDuration": 0.45}, "nova-short", 0.62)
        long = measure_nova({"novaDuration": 1.35}, "nova-long", 0.62)
        assert short.getbbox() is None and peak(long) >= 20
        summary = {
            "pack": pack_id,
            "renderer": status["renderer"],
            "adapter": status["gpuAdapter"],
            "resolution": list(baseline.size),
            "keyT": {"receipt": key, "bounds": bounds, "centerPeak": peak_t},
            "keyM": {"receipt": second, "bounds": second_bounds, "centerPeak": peak_m},
            "peakDifferenceNebulaAndVoid": peak(difference(baseline, void)),
            "peakDifferenceDiskDisabled": peak(disk_difference),
            "skySettingsPeakDifferences": sky_deltas,
            "novaTintPeakDifferences": tint_peaks,
            "novaShockWidthsPx": widths,
            "novaDurationPeakDifferences": [peak(short), peak(long)],
            "idleFramesDelta": rest_after - rest_before,
        }
    report = json.dumps(summary, ensure_ascii=True, indent=2)
    (args.output / "report.json").write_text(report + "\n", encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
