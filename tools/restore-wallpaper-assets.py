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
    missing = [path for path in external_wallpaper_assets(ROOT) if not (ROOT / path).exists()]
    if not missing:
        return
    with urlopen(URL, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        sys.exit("Wallpaper archive checksum mismatch")
    with ZipFile(io.BytesIO(data)) as archive:
        for relative in missing:
            target = ROOT / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            content = archive.read(relative.relative_to("wallpapers-native").as_posix())
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as temporary:
                temporary.write(content)
            os.replace(temporary.name, target)
    print(f"Restored {len(missing)} wallpaper assets")


if __name__ == "__main__":
    main()
