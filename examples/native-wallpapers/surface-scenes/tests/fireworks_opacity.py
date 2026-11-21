"""Verify the night-fireworks opacity settings with reproducible D3D12 RGBA captures."""

import argparse
import json
from pathlib import Path
import subprocess

from PIL import Image, ImageChops, ImageDraw

from compare import KEY, POINTER, SETTINGS, Timeline, dump


WIDTH, HEIGHT = 960, 360
ACTIVE = ("early", "burst", "tails")


def replay(executable, pack, destination, test_mute, **settings):
    destination.mkdir(parents=True, exist_ok=True)
    timeline = Timeline(settings, WIDTH, HEIGHT)
    timeline.frame(0, capture="idle", status=-1)
    timeline.frame(0.17, {"type": KEY, "code": "KeyG", "at": 0.02, "x": 0.51, "y": 0.69},
                   capture="early", status=0)
    timeline.frame(0.46, capture="burst", status=0)
    timeline.frame(0.91, capture="tails", status=0)
    timeline.frame(9, capture="settled", status=-1)
    if test_mute:
        muted = settings | {"foregroundOpacity": 0}
        timeline.frame(10.17, {"type": KEY, "code": "KeyG", "at": 10.02}, status=0)
        timeline.frame(10.18, {"type": SETTINGS, "values": muted}, capture="muted", status=-1)
        timeline.frame(10.19, {"type": KEY, "code": "KeyG"}, {"type": POINTER}, capture="muted-input", status=-1)
        timeline.frame(15, capture="muted-settled", status=-1)
        timeline.frame(16, {"type": SETTINGS, "values": settings}, status=-1)
        timeline.frame(16.42, {"type": KEY, "code": "KeyG", "at": 16.02}, capture="restored", status=0)
    dump(destination / "timeline.json", timeline.data() | {"rgba": True})
    subprocess.run([str(executable), "12", str(pack), str(destination / "timeline.json"), str(destination)],
                   check=True)
    report = json.loads((destination / "replay.json").read_text(encoding="utf-8"))
    return report, {path.stem: Image.frombytes("RGBA", (WIDTH, HEIGHT), path.read_bytes())
                    for path in destination.glob("*.rgba")}


def peak_difference(a, b):
    return max(channel.getextrema()[1] for channel in ImageChops.difference(a, b).split())


def multiply_bytes(image, amount):
    return image.point(lambda x: round(x * amount))


def premultiplied(image):
    red, green, blue, alpha = image.split()
    maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    assert ImageChops.subtract(maximum, alpha).getbbox() is None


def save_desktop_examples(path, full, half):
    panel_width, panel_height = 480, 180
    label_height = 28
    chart = Image.new("RGB", (2 * panel_width, 2 * (panel_height + label_height)), (31, 34, 41))
    draw = ImageDraw.Draw(chart)
    for row, (desktop, title) in enumerate((((0, 0, 0), "black"), ((226, 233, 243), "light"))):
        for column, (frame, foreground) in enumerate(((full, 100), (half, 50))):
            alpha = frame.getchannel("A")
            visible_desktop = Image.merge("RGB", [alpha.point(lambda a: round(c * (255 - a) / 255))
                                                  for c in desktop])
            composed = ImageChops.add(frame.convert("RGB"), visible_desktop)
            left = column * panel_width
            top = row * (panel_height + label_height)
            draw.text((left + 10, top + 8), f"{title} desktop | foreground {foreground}% | background 0%",
                      fill=(248, 248, 250))
            chart.paste(composed.resize((panel_width, panel_height), Image.Resampling.LANCZOS),
                        (left, top + label_height))
    chart.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("replay", "baseline", "pack", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()

    def run(label, pack=None, **settings):
        return replay(args.replay, pack or args.pack, args.output / label,
                      test_mute=label in ("opaque", "transparent"), **settings)

    original_report, original = run("original", args.baseline)
    final_report, full = run("opaque")
    _, original_solid = run("original-solid", args.baseline, backgroundStyle=1)
    _, solid = run("opaque-solid", backgroundStyle=1)
    assert all(peak_difference(original[name], full[name]) == 0 for name in original)
    assert all(peak_difference(original_solid[name], solid[name]) == 0 for name in original_solid)

    _, transparent = run("transparent", backgroundOpacity=0)
    _, half = run("half-foreground", foregroundOpacity=50, backgroundOpacity=0)
    _, half_background = run("half-background", backgroundOpacity=50)
    _, both = run("half-both", foregroundOpacity=50, backgroundOpacity=50)
    _, solid_transparent = run("transparent-solid", backgroundOpacity=0, backgroundStyle=1)
    _, no_smoke = run("without-smoke", backgroundOpacity=0, smoke=0)
    _, no_glow = run("without-glow", backgroundOpacity=0, bloom=0, smoke=0)

    opaque_idle = full["idle"]
    assert opaque_idle.getextrema()[3] == (255, 255)
    assert half_background["idle"].getextrema()[3] == (128, 128)
    assert peak_difference(multiply_bytes(opaque_idle, .5), half_background["idle"]) <= 1
    assert peak_difference(opaque_idle, full["settled"]) == 0
    for name in ("muted", "muted-input", "muted-settled"):
        assert peak_difference(opaque_idle, full[name]) == 0
        assert all(channel == (0, 0) for channel in transparent[name].getextrema())
    assert peak_difference(opaque_idle, full["restored"]) > 25
    for case in (transparent, half, half_background, both, solid_transparent):
        for image in case.values():
            premultiplied(image)
    for name in ("idle", "settled"):
        assert all(channel == (0, 0) for channel in transparent[name].getextrema())
        assert all(channel == (0, 0) for channel in solid_transparent[name].getextrema())
    for name in ACTIVE:
        assert peak_difference(transparent[name], solid_transparent[name]) == 0
        assert peak_difference(multiply_bytes(transparent[name], .5), half[name]) <= 1
        expected = Image.blend(half[name], Image.blend(opaque_idle, full[name], .5), .5)
        assert peak_difference(expected, both[name]) <= 2
    alpha = transparent["burst"].getchannel("A")
    smoke = no_smoke["burst"].getchannel("A")
    glow = no_glow["burst"].getchannel("A")
    peak = alpha.getextrema()[1]
    assert peak >= 120 and alpha.getpixel((0, 0)) == 0
    assert peak_difference(alpha, smoke) >= 1
    assert peak_difference(smoke, glow) >= 5
    assert alpha.histogram()[0] < glow.histogram()[0]

    summary = {
        "resolution": [WIDTH, HEIGHT],
        "d3d12DebugLayer": original_report["debug"] and final_report["debug"],
        "opaqueFramesEqualToOriginal": len(original) + len(original_solid),
        "foregroundZeroAfterActive": [full["muted"].getextrema(), transparent["muted"].getextrema()],
        "fullyTransparentIdleRgba": transparent["idle"].getpixel((0, 0)),
        "halfBackgroundIdleAlpha": half_background["idle"].getchannel("A").getextrema(),
        "burstPeakAlpha": peak,
        "transparentBurstCoveredPixels": WIDTH * HEIGHT - alpha.histogram()[0],
        "noGlowBurstCoveredPixels": WIDTH * HEIGHT - glow.histogram()[0],
        "smokePeakAlphaContribution": peak_difference(alpha, smoke),
        "glowPeakAlphaContribution": peak_difference(smoke, glow),
    }
    dump(args.output / "report.json", summary)
    save_desktop_examples(args.output / "desktop-examples.png", transparent["burst"], half["burst"])
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
