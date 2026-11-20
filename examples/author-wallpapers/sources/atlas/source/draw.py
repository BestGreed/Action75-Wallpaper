"""Draw the bathymetric chart and 24 frames of an acoustic locator echo."""
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from background_controls import with_background

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

PACK = Path(__file__).resolve().parents[1] / "soundings-echo"
PACK.mkdir(exist_ok=True)


def save_json(name, data):
    if name == "settings.json":
        with_background(data)
    (PACK / name).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def sea_chart():
    y, x = np.mgrid[0:360, 0:960]
    level = (0.8 * np.cos(x / 126 + y / 106) + np.sin(x / 82 - y / 67)
             + 0.44 * np.sin(x / 110 + np.cos(y / 42)) + y / 330)
    contour = np.clip(1 - np.abs((level * 7 + 0.5) % 1 - 0.5) * 13, 0, 1)
    grid = ((x % 80 == 0) | (y % 80 == 0)) * 0.35
    shade = 0.5 + 0.5 * np.cos(x / 290 - y / 140)
    channels = [base + scale * shade + ink * (contour + grid)
                for base, scale, ink in [(5, 4, 7), (18, 9, 18), (26, 12, 20)]]
    return Image.fromarray(np.stack(channels, axis=-1).astype("uint8"))


def echo_frame(index):
    t = index / 23
    fade = min(1, (1 - t) * 3.7)
    side = 576
    c = side // 2
    plane = Image.new("RGBA", (side, side))
    pen = ImageDraw.Draw(plane)

    def rgba(rgb):
        return (*rgb, round(255 * fade))

    aqua, amber, ice = rgba((65, 246, 223)), rgba((255, 178, 73)), rgba((219, 255, 242))
    radius = 42 + 221 * (1 - (1 - t) ** 2)

    def box(r):
        return (c - r, c - r, c + r, c + r)

    for sector in range(3):
        start = 120 * sector + 63 * t
        pen.arc(box(radius), start, start + 88, fill=aqua, width=7)
        pen.arc(box(radius - 10), start + 62, start + 89, fill=ice, width=3)
    for tooth in range(12):
        a = math.radians(30 * tooth + 63 * t)
        r = radius - 18
        points = [(c + s * math.cos(a), c + s * math.sin(a)) for s in (r, r - (15 if tooth % 3 == 0 else 7))]
        pen.line(points, fill=amber, width=6)
    d = 6 + 12 * (1 - t)
    pen.polygon([(c, c-d), (c+d/2, c), (c, c+d), (c-d/2, c)], fill=ice)
    glow = plane.copy()
    glow.putalpha(plane.getchannel("A").filter(ImageFilter.GaussianBlur(6)))
    halo = Image.new("RGBA", plane.size, (29, 209, 190, 0))
    halo.putalpha(glow.getchannel("A"))
    return Image.alpha_composite(halo, plane).resize((192, 192), Image.Resampling.LANCZOS)


def main():
    sea = sea_chart()
    sea.save(PACK / "depths.png")
    atlas = Image.new("RGBA", (6 * 192, 4 * 192))
    for i in range(24):
        atlas.paste(echo_frame(i), ((i % 6) * 192, (i // 6) * 192))
    atlas.save(PACK / "echo.png")
    cover = sea.convert("RGBA")
    frame = echo_frame(8).resize((345, 345), Image.Resampling.LANCZOS)
    cover.alpha_composite(frame, (307, 7))
    cover.convert("RGB").resize((640, 240), Image.Resampling.LANCZOS).save(PACK / "cover.png")
    save_json("manifest.json", {
        "version": 1, "id": PACK.name, "name": "海图回声", "kind": "spriteBurst",
        "settings": "settings.json", "preview": "cover.png", "background": "depths.png",
        "effect": "echo.png", "columns": 6, "rows": 4, "duration": 0.85, "diameter": 300
    })
    save_json("settings.json", {
        "version": 1, "presentation": "layered",
        "groups": [{"id": "sonar", "label": "回声", "controls": [
            {"id": "reach", "label": "波环直径", "type": "slider", "default": 1,
             "min": 0.5, "max": 1.4, "step": 0.05, "unit": "×", "binding": {"target": "effectScale"}}
        ]}]
    })


if __name__ == "__main__":
    main()
