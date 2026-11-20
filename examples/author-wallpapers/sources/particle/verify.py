"""Run the published six-frame author trial with single-resource diagnostic manifests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from PIL import Image

BASE = Path(__file__).resolve().parent
PACK = BASE / "lantern-almanac"
EVIDENCE = BASE / "evidence"


def check(player, preview):
    env = dict(os.environ, TEMP=str(BASE), TMP=str(BASE))
    EVIDENCE.mkdir(exist_ok=True)
    manifest = json.loads((PACK / "manifest.json").read_text(encoding="utf-8"))
    results = []
    for asset in [None, *manifest["effects"]]:
        with tempfile.TemporaryDirectory(dir=BASE) as scratch:
            target = PACK
            if asset:
                target = Path(scratch) / PACK.name
                shutil.copytree(PACK, target)
                single = dict(manifest, effect=asset, effects=[asset])
                (target/"manifest.json").write_text(json.dumps(single,ensure_ascii=False),encoding="utf-8")
            frame = EVIDENCE / ((Path(asset).stem if asset else "pack-trial")+".png")
            proc = subprocess.run([sys.executable, preview, "--player", player, "--pack", target, "--frame", frame],
                                  check=True, stdout=subprocess.PIPE, env=env)
            report = json.loads(proc.stdout)
            with Image.open(frame) as image:
                rgb = image.convert("RGB")
                box = rgb.getbbox()
                if box is None:
                    raise ValueError(f"Empty frame for {asset}")
                results.append(dict(asset=asset, frame=frame.name, size=rgb.size, litBounds=box,
                                    distinctRGB=len(rgb.getcolors(rgb.width*rgb.height)), trial=report))
    (EVIDENCE/"trial.json").write_text(json.dumps(dict(player=Path(player).name,
        sha256=hashlib.sha256(Path(player).read_bytes()).hexdigest(),results=results), indent=2), encoding="utf-8")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--player", required=True, type=Path)
    p.add_argument("--preview", required=True, type=Path)
    options = p.parse_args()
    check(options.player.resolve(strict=True), options.preview.resolve(strict=True))
