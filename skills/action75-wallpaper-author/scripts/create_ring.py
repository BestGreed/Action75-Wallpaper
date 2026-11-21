"""Create an editable spriteBurst wallpaper in a new directory (requires Pillow)."""
import argparse
import json
import re
import shutil
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--id", required=True)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", args.id):
        parser.error("Use a lowercase ASCII pack ID of 1 to 64 characters.")
    if args.output.exists():
        parser.error("Choose a new output directory.")
    args.output.mkdir(parents=True)
    atlas = Image.new("RGBA", (4 * 96, 2 * 96))
    for index in range(8):
        frame = Image.new("RGBA", (96, 96))
        radius = 6 + 5 * index
        ImageDraw.Draw(frame).ellipse(
            (48-radius, 48-radius, 48+radius, 48+radius),
            outline=(109, 234, 222, 255 - 24 * index), width=4,
        )
        atlas.paste(frame, (index % 4 * 96, index // 4 * 96))
    atlas.save(args.output / "ring.png")
    background = Image.new("RGB", (320, 120), (15, 30, 46))
    background.save(args.output / "base.png")
    cover = background.convert("RGBA")
    cover.alpha_composite(atlas.crop((96, 96, 192, 192)), (112, 12))
    cover.save(args.output / "cover.png")
    manifest = dict(version=1, id=args.id, name=args.name, kind="spriteBurst",
                    settings="settings.json", preview="cover.png", background="base.png",
                    effect="ring.png", columns=4, rows=2, duration=0.8, diameter=240)
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    shutil.copyfile(Path(__file__).resolve().parents[1] / "assets/ring/settings.json",
                    args.output / "settings.json")


if __name__ == "__main__":
    main()
