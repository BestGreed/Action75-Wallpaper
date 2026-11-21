from pathlib import Path
from urllib.request import urlopen
from zipfile import ZipFile
import hashlib
import io
import os
import tempfile
import sys

from release_payload import ROOT, external_wallpaper_assets

URL = "https://raw.githubusercontent.com/blackwhite-poi/Action75-Wallpaper-Library/52cdc9b9646eee90969924ae7e657b9ac0b9a023/packs/%E6%B0%B4%E5%BD%A9%E5%96%B7%E6%BA%85.zip"
SHA256 = "cb38f32198f830297bdd433514043bac402427b85730305a5205cb612d174f51"


def main():
    missing = sorted(path for path in external_wallpaper_assets(ROOT) if not (ROOT / path).exists())
    if not missing:
        return
    with urlopen(URL, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        sys.exit("Wallpaper archive checksum mismatch")
    restored = 0
    with ZipFile(io.BytesIO(data)) as archive:
        targets = [(ROOT / relative, relative.relative_to("wallpapers-native").as_posix()) for relative in missing]
        members = {entry.filename for entry in archive.infolist() if not entry.is_dir()}
        for target, member in targets:
            if member not in members:
                raise ValueError(f"Wallpaper archive is missing {member}")
            if target.resolve() != target:
                raise ValueError(f"Wallpaper asset redirects to another path: {target}")
        for target, member in targets:
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=".action75-asset-", dir=target.parent) as scratch:
                staged = Path(scratch) / target.name
                staged.write_bytes(archive.read(member))
                try:
                    if os.name == "nt":
                        staged.rename(target)
                    else:
                        target.hardlink_to(staged)
                except FileExistsError:
                    if not target.is_file():
                        raise
                else:
                    restored += 1
    print(f"Restored {restored} wallpaper assets")


if __name__ == "__main__":
    main()
