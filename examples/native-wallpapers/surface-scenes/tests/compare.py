"""Replay the ABI 1 and ABI 2 DLLs offscreen, compare readbacks, and save PNG evidence."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageChops

KEY, POINTER, SETTINGS, RESIZE, PAUSE, RESUME, RESET = 1, 3, 6, 7, 8, 9, 10
PACKS = ("stillwater", "night-fireworks", "glass-shatter")


def dump(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def key(**fields):
    return {"type": KEY, "code": "KeyG", **fields}


class Timeline:
    def __init__(self, defaults, width=960, height=360):
        self.defaults = defaults
        self.frames = []
        self.width, self.height = width, height

    def config(self, **changes):
        return {"type": SETTINGS, "values": self.defaults | changes}

    def frame(self, at, *events, capture=None, status=None, **dimensions):
        frame = {"at": at, "events": events, **dimensions}
        if capture is not None:
            frame["capture"] = capture
        if status is not None:
            frame["status"] = status
        self.frames.append(frame)

    def data(self):
        return {
            "width": self.width, "height": self.height, "settings": self.defaults,
            "frames": self.frames,
        }


def water(t):
    # Equal bounds keep random amplitude from masking solver and optics differences.
    for style in range(5):
        start = 4 * style
        t.frame(start, {"type": RESET}, t.config(backdrop=style, impactMin=1.2, impactMax=1.2),
                capture=f"background-{style}", status=0)
        for frame in range(1, 39):
            events = [key(x=.48, y=.47)] if frame == 1 else []
            if frame == 9:
                events = [{"type": POINTER, "x": .60, "y": .53}]
            name = f"background-{style}-wave-{frame}" if frame in (6, 18, 38) else None
            t.frame(start + frame / 60, *events, capture=name, status=0)
    stable = dict(ambient=0, texture=0, caustics=0, backdrop=4, waterColor=[36, 91, 125])
    for index, force in enumerate((.24, 1.2, 2.36)):
        start = 21 + index * 3
        t.frame(start, {"type": RESET}, t.config(**stable, impactMin=force, impactMax=force),
                capture=f"force-{index}-idle", status=-1)
        t.frame(start + .001, key(), status=0)
        for frame in range(1, 19):
            t.frame(start + frame / 60, capture=f"force-{index}" if frame == 18 else None, status=0)
    t.frame(30, {"type": PAUSE}, capture="pause", status=-1)
    t.frame(30.1, key(), capture="pause-key", status=-1)
    t.frame(31, {"type": RESUME}, {"type": RESET}, status=-1)
    t.frame(32, {"type": RESIZE}, key(), capture="resized", status=0, width=720, height=480)
    t.frame(33, {"type": RESET}, capture="resized-idle", status=-1)


def fireworks(t, schema):
    group = next(group for group in schema["groups"] if "playback" in group)
    controls = {c["id"]: c for g in schema["groups"] for c in g["controls"]}
    for style in controls["backgroundStyle"]["options"]:
        value = style["value"]
        t.frame(value, t.config(backgroundStyle=value), capture=f"background-{value}", status=-1)
    for shape in range(1, 11):
        start = 10 * shape
        t.frame(start, {"type": RESET}, t.config(shape=shape), capture=f"shape-{shape}-idle", status=-1)
        t.frame(start + .17, key(at=start + .01), capture=f"shape-{shape}-early", status=0)
        t.frame(start + .48, capture=f"shape-{shape}-peak", status=0)
        t.frame(start + .91, capture=f"shape-{shape}-tail", status=0)
        t.frame(start + 8, status=-1)
    # Retain every variant and let real event processing advance the sequence.
    enabled = {c["id"]: True for c in group["controls"] if c["type"] == "toggle" and c["id"] != "shuffle"}
    t.frame(109, {"type": RESET}, t.config(**enabled, keyInterval=3), status=-1)
    for index, expected_shape in enumerate(group["playback"]["order"]):
        start = 110 + index * 9
        t.frame(start, key(), status=-1)
        t.frame(start + .01, key(), status=-1)
        t.frame(start + .49, key(at=start + .02), capture=f"cycle-{index + 1}-{expected_shape}", status=0)
        t.frame(start + 8, status=-1)
    t.frame(202, {"type": POINTER, "x": .32, "y": .45, "at": 201.52}, capture="pointer-bypass", status=0)
    t.frame(209, {"type": RESET}, t.config(**enabled, shuffle=True, randomPalette=True, horizon=69), status=-1)
    for i in range(5):
        start = 210 + i * 9
        t.frame(start + .48, key(at=start), capture=f"shuffle-palette-{i}", status=0)
        t.frame(start + 8, status=-1)
    t.frame(258, t.config(shape=9, bloom=0, smoke=0, quality=2, density=1, horizon=96,
                          colors={"preset": "custom", "colors": [[67, 180, 245], [244, 99, 89]]}),
            capture="no-glow-idle", status=-1)
    t.frame(258.5, key(at=258.02), capture="no-glow-custom", status=0)
    t.frame(258.6, *(key(x=.24 + i % 13 * .04, y=.20 + i // 13 * .06, at=258.41) for i in range(126)),
            capture="particle-budget", status=0)
    t.frame(258.7, {"type": PAUSE}, capture="paused-particles", status=-1)
    t.frame(267, {"type": RESUME}, {"type": RESET}, {"type": RESIZE},
            capture="resized-idle", status=-1, width=720, height=480)
    t.frame(267.48, key(at=267), capture="resized-burst", status=0)


def glass(t, schema):
    control = next(c for group in schema["groups"] for c in group["controls"] if c["id"] == "backdrop")
    for style in control["options"]:
        value = style["value"]
        t.frame(value, t.config(backdrop=value), capture=f"background-{value}", status=-1)
    for index, force in enumerate((.38, 1.05, 2.25)):
        start = 6 + index * 3
        fixed = dict(impactMin=force, impactMax=force, duration=2, branches=5)
        t.frame(start, {"type": RESET}, t.config(**fixed), capture=f"force-{index}-idle", status=-1)
        t.frame(start + .09, key(at=start + .01), capture=f"force-{index}-early", status=0)
        for step, core in enumerate((.5, 1.35, 3)):
            t.frame(start + .24, t.config(**fixed, coreSpread=core),
                    capture=f"force-{index}-core-{step}", status=0)
        t.frame(start + .24, t.config(**(fixed | dict(coreSpread=3, branches=7))),
                capture=f"force-{index}-more-branches", status=0)
        t.frame(start + .24, t.config(**(fixed | dict(coreSpread=3, branches=0))),
                capture=f"force-{index}-no-branches", status=0)
        if index == 2:
            optical = fixed | {"coreSpread": 3, "branches": 7}
            for name, value in (("seamWidth", .55), ("glints", 0), ("refraction", 0), ("chips", 4)):
                t.frame(start + .24, t.config(**(optical | {name: value})), capture=f"glass-{name}", status=0)
        t.frame(start + .28, key(at=start + .25), capture=f"force-{index}-repeat", status=0)
        t.frame(start + 2.4, capture=f"force-{index}-settled", status=-1)

    t.frame(16, t.config(impactMin=0, impactMax=0), key(), capture="zero-force", status=-1)
    t.frame(17, t.config(impactMin=.65, impactMax=2.3), {"type": POINTER}, status=-1)
    # Both packages seed strength and geometry independently in production.
    for index in range(12):
        start = 19 + index * .3
        t.frame(start + .24, {"type": RESET}, key(at=start), capture=f"random-{index}", status=0)
    t.frame(24, {"type": PAUSE}, key(), status=-1)
    t.frame(25, {"type": RESUME}, {"type": RESIZE}, key(at=24.9, x=.99), capture="resized", status=0,
            width=720, height=480)
    t.frame(26, {"type": RESET}, capture="resized-idle", status=-1)


def compare_images(before, after):
    diff = ImageChops.difference(before, after)
    histogram = diff.histogram()
    count = before.width * before.height * 3
    return {
        "meanAbsRgb": sum((i % 256) * n for i, n in enumerate(histogram)) / count,
        "maxAbsRgb": max(i % 256 for i, n in enumerate(histogram) if n),
        "channelsOver3Pct": 100 * sum(n for i, n in enumerate(histogram) if i % 256 > 3) / count,
    }


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def distinct(path, names):
    return len({(path / (name + ".png")).read_bytes() for name in names}) == len(names)


def verify_glass(path):
    # Compare the same impact before and after moving the center slider. Keeping its
    # angle, geometry index and time fixed isolates the change in pane boundaries.
    metrics = {}
    for index in range(3):
        key_name = f"force-{index}"
        small = Image.open(path / f"{key_name}-core-0.png")
        large = Image.open(path / f"{key_name}-core-2.png")
        diff = ImageChops.difference(small, large)
        box = tuple(round(fraction * edge) for fraction, edge in zip(
            (.39, .22, .61, .78), (small.width, small.height, small.width, small.height)))
        r, g, b = diff.split()
        max_channel = ImageChops.lighter(ImageChops.lighter(r, g), b)
        bounds = max_channel.point(lambda v: 255 if v > 5 else 0)
        changed = bounds.histogram()[255]
        inner = bounds.crop(box).histogram()[255]
        require(changed > 0 and distinct(path, [f"{key_name}-core-{size}" for size in range(3)]),
                "All three center sizes must visibly change the same fracture")
        require(distinct(path, [f"{key_name}-more-branches", f"{key_name}-no-branches"]),
                "Disabling the branches must change the active fracture")
        idle, settled = (path / f"{key_name}-{suffix}.png" for suffix in ("idle", "settled"))
        require(idle.read_bytes() == settled.read_bytes(), "Glass should return to its original backdrop")
        metrics[key_name] = {"pixelsChanged": changed, "nearImpactFraction": inner / changed}
    require(distinct(path, ["force-2-core-2", "force-2-more-branches", "force-2-no-branches"]),
            "Strong strikes must show the higher branch limit")
    for name in ("seamWidth", "glints", "refraction", "chips"):
        require(distinct(path, ["force-2-more-branches", f"glass-{name}"]),
                f"Changing {name} must affect the active glass")
    require(distinct(path, [f"random-{i}" for i in range(12)]), "Consecutive impacts should vary")
    return metrics


def replay(args, pack, root):
    schema = json.loads((args.baseline / pack / "settings.json").read_text(encoding="utf-8-sig"))
    defaults = {c["id"]: c["default"] for group in schema["groups"] for c in group["controls"]}
    timeline = Timeline(defaults, 1440, 540) if pack == "glass-shatter" else Timeline(defaults)
    if pack == "stillwater":
        water(timeline)
    elif pack == "night-fireworks":
        fireworks(timeline, schema)
    else:
        glass(timeline, schema)

    evidence = args.output / pack
    evidence.mkdir(parents=True, exist_ok=True)
    spec = evidence / "timeline.json"
    dump(spec, timeline.data())
    summaries = {}
    for name, abi, library in (("d3d11", 11, args.baseline), ("d3d12", 12, args.packs)):
        temp = root / name
        result = subprocess.run([args.replay, str(abi), library / pack, spec, temp],
                                capture_output=True, text=True, timeout=120)
        require(result.returncode == 0, f"{pack} {name}: {result.stderr.strip()}")
        summaries[name] = json.loads((temp / "replay.json").read_text(encoding="utf-8"))
        photos = evidence / name
        photos.mkdir(exist_ok=True)
        for frame in timeline.frames:
            if "capture" in frame:
                capture = frame["capture"]
                Image.open(temp / f"{capture}.ppm").save(photos / f"{capture}.png")

    require(summaries["d3d11"]["scheduling"] == summaries["d3d12"]["scheduling"], "Frame scheduling differs")
    require(summaries["d3d11"]["capturesPointer"] == summaries["d3d12"]["capturesPointer"], "Pointer hit policy differs")
    comparisons = {}
    for frame in timeline.frames:
        capture = frame.get("capture")
        random_glass = pack == "glass-shatter" and not args.fixed_glass_seed
        if capture is None or (random_glass and not capture.startswith(("background-", "zero-force", "resized-idle"))):
            continue
        before, after = [Image.open(root / version / f"{capture}.ppm") for version in ("d3d11", "d3d12")]
        comparisons[capture] = compare_images(before, after)
    summary = {"frames": len(timeline.frames), "captures": comparisons, "replays": summaries}
    if pack == "glass-shatter":
        summary["centerSlider"] = {version: verify_glass(evidence / version) for version in summaries}
    if pack == "night-fireworks":
        for version in summaries:
            require(distinct(evidence / version, [f"shape-{i}-peak" for i in range(1, 11)]),
                    "All ten authored fireworks shapes must have distinct readbacks")
    if pack == "stillwater":
        for version in summaries:
            require(distinct(evidence / version, [f"background-{i}" for i in range(5)]),
                    "The five water backgrounds must remain distinct")
    dump(evidence / "comparison.json", summary)
    worst = max(comparisons, key=lambda name: comparisons[name]["meanAbsRgb"])
    print(f"{pack} {len(timeline.frames)} frames per ABI, {len(comparisons)} pixel pairs, "
          f"worst {worst} {comparisons[worst]}, debug layer {summaries['d3d12']['debug']}", flush=True)
    require(all(v["meanAbsRgb"] <= .40 and v["channelsOver3Pct"] <= .60 for v in comparisons.values()),
            f"{pack} has a visible parity error in {worst}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("replay", "baseline", "packs", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--pack", choices=PACKS)
    parser.add_argument("--fixed-glass-seed", action="store_true",
                        help="Compare active glass pixels when both DLL fixtures were built with the same seed")
    args = parser.parse_args()
    for pack in (args.pack,) if args.pack else PACKS:
        with tempfile.TemporaryDirectory(prefix="Action75-surface-compare-") as temporary:
            replay(args, pack, Path(temporary))


if __name__ == "__main__":
    main()
