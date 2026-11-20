"""Compare actual playback to a transparent effect and a reduced scale default."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "soundings-echo"
OUT = ROOT / "verify"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--player", type=Path, required=True)
    p.add_argument("--preview", type=Path, required=True)
    args = p.parse_args()
    OUT.mkdir(exist_ok=True)
    result = {"playerSHA256": hashlib.sha256(args.player.read_bytes()).hexdigest(), "trials": {}}
    with tempfile.TemporaryDirectory() as td:
        for name in ("default", "transparent", "small"):
            pack = Path(td) / name
            shutil.copytree(PACK, pack)
            if name == "transparent":
                with Image.open(pack / "echo.png") as img:
                    size = img.size
                Image.new("RGBA", size).save(pack / "echo.png")
            if name == "small":
                settings = pack / "settings.json"
                data = json.loads(settings.read_text(encoding="utf-8"))
                data["groups"][0]["controls"][0]["default"] = 0.5
                settings.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
            proc = subprocess.run([sys.executable, str(args.preview), "--player", str(args.player),
                                   "--pack", str(pack), "--frame", str(OUT / (name + ".png"))],
                                  capture_output=True, text=True, check=True)
            result["trials"][name] = json.loads(proc.stdout)
    reference = np.asarray(Image.open(OUT / "transparent.png").convert("RGB"), dtype=int)
    diffs = {}
    for name in ("default", "small"):
        rgb = np.asarray(Image.open(OUT / (name + ".png")).convert("RGB"), dtype=int)
        mask = np.max(np.abs(rgb - reference), axis=2) >= 16
        y, x = np.where(mask)
        diffs[name] = {"pixels": int(x.size), "bboxInclusive": [int(f(v)) for f, v in
                       [(min, x), (min, y), (max, x), (max, y)]] if x.size else None}
    result["differenceAtLeast16"] = diffs
    result["readbackSize"] = list(reference.shape[1::-1])
    (OUT / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(diffs))


if __name__ == "__main__":
    main()
