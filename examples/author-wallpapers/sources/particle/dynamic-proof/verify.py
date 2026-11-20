"""Read back two identical-time captures and measure the float's offset from the gauge."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from PIL import Image

HERE = Path(__file__).resolve().parent
PACK = HERE/"tidemark-bearing"


def measure(path):
    image = Image.open(path).convert("RGB")
    r,g,b = image.split()
    from PIL import ImageChops
    masks = [ImageChops.subtract(g,r), ImageChops.subtract(r,b)]
    centers = []
    for mask in masks:
        box = mask.point(lambda x: 255 if x > 35 else 0).getbbox()
        if not box:
            raise ValueError(f"Missing marker in {path}")
        centers.append((box[0]+box[2]-1)/2)
    return dict(gaugeX=centers[0],floatX=centers[1],deltaX=centers[1]-centers[0])


def main():
    parser = argparse.ArgumentParser()
    for name in ("player","host-root","trace"):
        parser.add_argument("--"+name,type=Path,required=True)
    args = parser.parse_args()
    result = {}
    for value in (-1,1):
        dest = HERE / "evidence" / f"bearing-{value}"
        subprocess.run([sys.executable,args.trace,"--player",args.player,"--host-root",args.host_root,
                        "--pack",PACK,"--output",dest,"--values",json.dumps({"bearing":value}),
                        "--frames-ms","430"],env=dict(os.environ,TEMP=str(HERE),TMP=str(HERE)),check=True)
        result[str(value)] = measure(dest/"frame-00430.png")
    assert result["-1"]["deltaX"] < -30 and result["1"]["deltaX"] > 30
    result["effectSHA256"] = hashlib.sha256((PACK/"tidemark.efk").read_bytes()).hexdigest()
    result["playerSHA256"] = hashlib.sha256(args.player.read_bytes()).hexdigest()
    (HERE/"evidence"/"measurements.json").write_text(json.dumps(result,indent=2),encoding="utf-8")


if __name__ == "__main__":
    main()
