"""Offscreen author preview using an existing Action75 player and a scratch library."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("player", "pack", "frame"):
        parser.add_argument("--" + key, type=Path, required=True)
    args = parser.parse_args()
    pack = args.pack.resolve(strict=True)
    manifest = json.loads((pack / "manifest.json").read_text(encoding="utf-8-sig"))
    with tempfile.TemporaryDirectory(prefix="Action75-author-") as scratch:
        root = Path(scratch)
        shutil.copytree(pack, root / "wallpapers-native" / "candidate")
        proc = subprocess.run([str(args.player.resolve(strict=True)), "--root", str(root), "--import-trial"],
                              timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        result = root / "trial-result.json"
        report = json.loads(result.read_text(encoding="utf-8")) if result.exists() else {}
        if proc.returncode or not report.get("ok") or not report.get("triggered") or report.get("framesRendered", 0) < 1:
            raise SystemExit(f"Trial failed ({proc.returncode}): {report}")
        args.frame.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / "trial-frame.png", args.frame)
        print(json.dumps({"pack": manifest["id"], **report}, ensure_ascii=True))

if __name__ == "__main__":
    main()
