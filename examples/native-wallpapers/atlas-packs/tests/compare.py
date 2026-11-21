"""Compare frozen ABI 1 and assembled ABI 2 package frames on the same WARP driver."""

import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


PACKS = ("afterimage", "carrier", "folio", "tidal-glass", "meridian", "paintball-native")
FRAMES = ("idle", "artwork", "hit", "overlap", "progress", "paused", "resumed", "reset",
          "styled-idle", "styled-hit", "styled-decay", "expired")
DISPLAY_FRAMES = ("artwork", "hit", "overlap", "styled-hit", "styled-decay")


def fixed_effect(pack, out, index):
    shutil.copytree(pack, out)
    path = out / "scene.json"
    scene = json.loads(path.read_text(encoding="utf-8-sig"))
    scene["effects"] = [scene["effects"][index]]
    path.write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")


def load_frame(path, width, height):
    return np.fromfile(path, np.uint8).reshape(height, width, 4)


def delta(old, new):
    return np.abs(old.astype(np.int16) - new)


def active_pixels(frame, background):
    return np.count_nonzero(np.max(delta(frame, background)[:, :, :3], axis=2) > 3)


def make_contact_sheet(directory, width, height, title):
    thumb_width = min(width, 680)
    thumb_height = round(thumb_width * height / width)
    margin, gap = 16, 12
    row_height = thumb_height + 28
    sheet = Image.new("RGB", (2 * thumb_width + 2 * margin + gap, len(DISPLAY_FRAMES) * row_height + 68),
                      "#191a1b")
    pen = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    pen.text((margin, 13), title, font=font, fill="#f4f0e4")
    for i, (tag, label) in enumerate((("old", "ABI 1 / D3D11"), ("new", "ABI 2 / D3D12"))):
        x = margin + i * (thumb_width + gap)
        pen.text((x, 37), label, font=font, fill="#b7c0c8")
        for row, frame in enumerate(DISPLAY_FRAMES):
            y = 55 + row * row_height
            pen.text((x, y), frame, font=font, fill="#e4e2df")
            with Image.open(directory / f"{frame}-{tag}.png") as capture:
                panel = capture.convert("RGB").resize((thumb_width, thumb_height), Image.Resampling.LANCZOS)
                sheet.paste(panel, (x, y + 17))
    sheet.save(directory / "comparison.png")


def compare(probe, old, new, output, width, height, paint, fixed=False, screenshot=False):
    output.mkdir(parents=True)
    stats = {}
    with tempfile.TemporaryDirectory(prefix="action75-atlas-frames-") as temp:
        folder = Path(temp)
        run = subprocess.run([probe, old, new, folder, str(width), str(height)],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        (output / "frames.log").write_text(run.stdout + run.stderr, encoding="utf-8")
        if run.returncode:
            raise RuntimeError(f"{output.name}: ABI probe failed (see frames.log)")

        open_frame = lambda name, tag: load_frame(folder / f"{name}-{tag}.rgba", width, height)
        default = {tag: open_frame("artwork", tag) for tag in ("old", "new")}
        styled = {tag: open_frame("styled-idle", tag) for tag in ("old", "new")}
        for frame in FRAMES:
            a, b = (open_frame(frame, tag) for tag in ("old", "new"))
            rgba = delta(a, b)
            rgb = rgba[:, :, :3]
            peaks = np.max(rgb, axis=2)
            stats[frame] = {
                "rgb_changed_pixels": int(np.count_nonzero(peaks)),
                "rgb_gt_4": int(np.count_nonzero(peaks > 4)),
                "rgb_gt_12": int(np.count_nonzero(peaks > 12)),
                "rgb_max_abs": int(peaks.max()),
                "rgb_mean_abs": float(rgb.mean()),
                "different_alpha_pixels": int(np.count_nonzero(rgba[:, :, 3])),
            }
            if not paint and rgba.max() != 0:
                raise AssertionError(f"{output.name} {frame}: expected exact RGBA parity")
            if paint and frame == "artwork" and (rgb.mean() > 1 or peaks.max() > 6):
                raise AssertionError("Paintball canvas difference exceeds quantization tolerance")
            references = styled if frame.startswith("styled-") or frame == "expired" else default
            for tag, image in (("old", a), ("new", b)):
                if frame in ("reset", "expired") and np.any(image != references[tag]):
                    raise AssertionError(f"{output.name} {frame} {tag}: effect did not clear")
                stats[frame][f"{tag}_active_pixels"] = int(active_pixels(image, references[tag]))
                if frame in ("hit", "overlap", "styled-hit") and stats[frame][f"{tag}_active_pixels"] < 60:
                    raise AssertionError(f"{output.name} {frame} {tag}: no substantial localized effect")
                if frame in DISPLAY_FRAMES and screenshot:
                    Image.fromarray(image).save(output / f"{frame}-{tag}.png")
            if fixed and frame in ("hit", "overlap", "progress", "styled-hit", "styled-decay"):
                old_count = stats[frame]["old_active_pixels"]
                new_count = stats[frame]["new_active_pixels"]
                if abs(old_count - new_count) > 240 + .12 * max(old_count, new_count):
                    raise AssertionError(f"{output.name} {frame}: active footprint changed unexpectedly")
                if rgb.mean() > 1 or stats[frame]["rgb_gt_12"] > width * height * .01:
                    raise AssertionError(f"{output.name} {frame}: visual difference exceeds tolerance")
    if screenshot:
        make_contact_sheet(output, width, height, output.name)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--new", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--packs", nargs="+", choices=PACKS, default=PACKS)
    parser.add_argument("--paintball-variants", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = {"width": args.width, "height": args.height, "packs": {}, "fixed_pigments": {}}
    for pack in args.packs:
        results["packs"][pack] = compare(args.probe, args.old / pack, args.new / pack,
                                         args.output / pack, args.width, args.height,
                                         pack == "paintball-native", screenshot=True)
        print(f"PASS {pack}", flush=True)

    if args.paintball_variants:
        for index in range(9):
            tag = f"pigment-{index + 1:02}"
            with tempfile.TemporaryDirectory(prefix="action75-pigment-") as temp:
                fixture = Path(temp)
                fixed_effect(args.old / "paintball-native", fixture / "old", index)
                fixed_effect(args.new / "paintball-native", fixture / "new", index)
                results["fixed_pigments"][tag] = compare(
                    args.probe, fixture / "old", fixture / "new", args.output / tag,
                    args.width, args.height, True, fixed=True, screenshot=True)
            print(f"PASS Paintball {tag}", flush=True)

    (args.output / "comparison.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
