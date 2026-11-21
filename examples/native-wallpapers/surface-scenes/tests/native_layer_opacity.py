"""Read back isolated opacity layers and compare the installed build with its replacement."""

import argparse
import json
from pathlib import Path
import subprocess

from PIL import Image, ImageChops, ImageStat

from compare import KEY, SETTINGS, Timeline, dump


LAYERS = {
    "black-hole": ("starOpacity", "holeOpacity"),
    "event-horizon-native": ("starOpacity", "holeOpacity"),
    "glass-shatter": ("fractureOpacity",),
    "night-fireworks": ("foregroundOpacity",),
    "stillwater": ("rippleOpacity",),
    "supernova-horizon": ("novaOpacity", "diskOpacity"),
}
STATIC_OBJECTS = {"holeOpacity", "diskOpacity"}
RANDOM_SCENES = {"stillwater", "glass-shatter"}


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def peak(image):
    return max(channel.getextrema()[1] for channel in image.split())


def capture(replay, pack, folder, size, vendor, settings, transition=False, compare_at_half=None):
    manifest = load(pack / "manifest.json")
    identifier = manifest["id"]
    initial = settings if not compare_at_half else settings | {compare_at_half: 100}
    timeline = Timeline(initial, *size)
    timeline.frame(0, capture="idle")
    now = 0.01
    key = {"type": KEY, "code": "KeyG", "at": now, "x": 0.36, "y": 0.49}
    timeline.frame(now, key)
    # The water simulation uses fixed substeps and caps the number per callback.
    for index in range(1, 23):
        frame = "early" if index == 5 else "active" if index == 20 else None
        timeline.frame(now + index / 60, capture=frame)
    if compare_at_half:
        timeline.frame(0.39, capture="full-before-half")
        timeline.frame(0.39, {"type": SETTINGS, "values": settings}, capture="half")
    if transition:
        off = {layer: 0 for layer in ("backgroundOpacity", *LAYERS[identifier])}
        timeline.frame(0.42, {"type": SETTINGS, "values": off}, capture="hidden", status=-1)
        timeline.frame(0.43, key | {"at": 0.43}, capture="hidden-input", status=-1)
        restored = dict.fromkeys(off, 100) | settings
        timeline.frame(0.44, {"type": SETTINGS, "values": restored}, capture="restored")

    folder.mkdir(parents=True, exist_ok=True)
    fixture = folder / "timeline.json"
    dump(fixture, timeline.data() | {"rgba": True, "entry": manifest["entry"], "vendorId": vendor})
    subprocess.run([str(replay), "12", str(pack), str(fixture), str(folder)], check=True)
    report = load(folder / "replay.json")
    if vendor and report["vendorId"] != vendor:
        raise AssertionError(("Wrong adapter", report))
    return {file.stem: Image.frombytes("RGBA", size, file.read_bytes())
            for file in folder.glob("*.rgba")}, report


def coverage(image):
    alpha = image.getchannel("A")
    rgb_max = ImageChops.lighter(ImageChops.lighter(image.getchannel("R"), image.getchannel("G")),
                                 image.getchannel("B"))
    invalid = peak(ImageChops.subtract(rgb_max, alpha))
    if invalid:
        raise AssertionError(("Invalid premultiplied RGB", invalid))
    hist = alpha.histogram()
    return {"alpha": alpha.getextrema(), "covered": alpha.width * alpha.height - hist[0],
            "totalAlpha": sum(value * count for value, count in enumerate(hist))}


def verify(replay, baseline, pack, folder, size, vendor):
    manifest = load(pack / "manifest.json")
    identifier = manifest["id"]
    layers = ("backgroundOpacity", *LAYERS[identifier])
    schema = load(pack / manifest["settings"])
    schema_controls = {c["id"]: c for g in schema["groups"] for c in g["controls"]}
    assert manifest["desktopOpacityControl"] == layers[0]
    for layer in layers:
        control = schema_controls[layer]
        assert (control["min"], control["max"], control["default"], control["appearance"]) == (0, 100, 100, "opacity")

    original, _ = capture(replay, baseline, folder / "baseline", size, vendor, {})
    current, info = capture(replay, pack, folder / "default", size, vendor, {}, transition=True)
    default_delta = {}
    for name, image in original.items():
        difference = ImageChops.difference(image, current[name])
        default_delta[name] = {"peak": peak(difference),
                               "meanRgb": max(ImageStat.Stat(difference).mean[:3])}
        if vendor and identifier == "night-fireworks" and name != "idle":
            # The INTEL float blend can reorder additive fragments between identical runs.
            assert default_delta[name]["peak"] <= 3 and default_delta[name]["meanRgb"] < .01, (
                identifier, name, default_delta)
        elif name == "idle" or identifier not in RANDOM_SCENES:
            assert default_delta[name]["peak"] == 0, (identifier, name, default_delta)
        else:
            assert default_delta[name]["meanRgb"] < 5, (identifier, name, default_delta)

    assert peak(current["hidden"]) == peak(current["hidden-input"]) == 0
    assert current["restored"].getchannel("A").getextrema() == (255, 255)
    off = dict.fromkeys(layers, 0)
    results = {}
    for layer in layers:
        quantities = {}
        for amount in (0, 50, 100):
            if amount == 0 and results:
                quantities[amount] = next(iter(results.values()))[0]
                continue
            settings = off | {layer: amount}
            compare_half = (identifier in RANDOM_SCENES and amount == 50 and layer != layers[0])
            scenario, _ = capture(replay, pack, folder / f"{layer}-{amount}", size, vendor, settings,
                                  compare_at_half=layer if compare_half else None)
            for image in scenario.values():
                coverage(image)
            frame = "idle" if layer == layers[0] or layer in STATIC_OBJECTS else "active"
            if compare_half:
                reference = coverage(scenario["full-before-half"])
                frame = "half"
                assert 0 < coverage(scenario[frame])["totalAlpha"] < reference["totalAlpha"]
            quantities[amount] = coverage(scenario[frame])
            if compare_half:
                quantities[amount]["fullOpacityAlphaAtSameFrame"] = reference["totalAlpha"]
            if layer == layers[0]:
                expected = round(amount * 255 / 100)
                assert quantities[amount]["alpha"] == (expected, expected), (identifier, layer, amount, quantities)
            elif amount:
                assert quantities[amount]["alpha"][0] == 0 and quantities[amount]["covered"] > 30, (
                    identifier, layer, amount, quantities)
                if layer in STATIC_OBJECTS or layer == "starOpacity":
                    assert quantities[amount]["alpha"][1] <= round(amount * 255 / 100)
        assert quantities[0]["covered"] == 0
        if identifier not in RANDOM_SCENES or layer == layers[0]:
            assert quantities[100]["totalAlpha"] > quantities[50]["totalAlpha"] > 0, (identifier, layer, quantities)
        results[layer] = quantities

    mixed, _ = capture(replay, pack, folder / "mixed-half", size, vendor, dict.fromkeys(layers, 50))
    mixed_coverage = coverage(mixed["active"])
    assert mixed_coverage["alpha"][0] == 128 and mixed_coverage["alpha"][1] > 128
    report = {"pack": identifier, "resolution": size, "vendorId": info["vendorId"],
              "debugLayer": info["debug"], "defaultFrames": default_delta, "isolatedLayers": results,
              "mixedLayersAt50": mixed_coverage, "allHiddenSchedulesIdle": True}
    dump(folder / "report.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("replay", "baseline", "packs", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--vendor-id", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--size", type=int, nargs=2, default=(960, 360))
    parser.add_argument("--include", nargs="+", choices=list(LAYERS), default=list(LAYERS))
    args = parser.parse_args()
    summary = {}
    for identifier in args.include:
        summary[identifier] = verify(args.replay, args.baseline / identifier, args.packs / identifier,
                                     args.output / identifier, tuple(args.size), args.vendor_id)
        print(f"{identifier}: passed", flush=True)
    dump(args.output / "report.json", summary)


if __name__ == "__main__":
    main()
