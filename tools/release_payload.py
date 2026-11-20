"""Stage the same distribution assets for installer builds and source archives."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib


ROOT = Path(__file__).resolve().parents[1]
MANAGER = Path("dist/Action75Manager/Action75Manager.exe")
PLAYER = Path("dist/Action75Native/Action75Native.exe")
WORKER = PLAYER.with_name("Action75WallpaperWorker.exe")
BUILT_MANAGER = ROOT / "apps/manager/target/release/Action75Manager.exe"
# Prefer the project compiler; CI and developer machines can use an installed copy.
_bundled_compiler = ROOT / "tools/vendor/InnoSetup/ISCC.exe"
_compiler_locations = [_bundled_compiler, shutil.which("ISCC.exe")]
for variable in ("ProgramFiles(x86)", "ProgramFiles"):
    if directory := os.getenv(variable):
        _compiler_locations.extend(Path(directory) / f"Inno Setup {major}/ISCC.exe" for major in (7, 6))
DEFAULT_COMPILER = next((Path(path) for path in _compiler_locations if path and Path(path).is_file()), _bundled_compiler)


def version(root=ROOT):
    cargo = (root / "apps/manager/Cargo.toml").read_text(encoding="utf-8")
    return tomllib.loads(cargo)["package"]["version"]


def inventory(root=ROOT):
    return json.loads((root / "tools/release-assets.json").read_text(encoding="utf-8"))


def external_wallpaper_assets(root=ROOT):
    pack = "paintball-native"
    source_files = {"manifest.json", "settings.json", "preview.png", "wallpaper.dll", "PORT-NOTES.txt"}
    return {Path("wallpapers-native", pack, name) for name in inventory(root)["wallpapers"][pack]
            if name not in source_files}


def release_files(root=ROOT):
    root = root.resolve()
    included = inventory(root)
    files = [Path(name) for name in included["files"]]
    for pack_id, names in included["wallpapers"].items():
        directory = Path("wallpapers-native") / pack_id
        if "manifest.json" not in names:
            raise ValueError(f"{pack_id} is missing its manifest in release-assets.json")
        files.extend(directory / name for name in names)
    if len(set(files)) != len(files):
        raise ValueError("Duplicate paths in release-assets.json")
    for relative in files:
        source = root / relative
        if relative.is_absolute() or ".." in relative.parts or source.resolve() != source:
            raise ValueError(f"Release assets must be local files without redirects: {relative}")
        if not source.is_file():
            raise FileNotFoundError(source)
    for pack_id in included["wallpapers"]:
        manifest = root / "wallpapers-native" / pack_id / "manifest.json"
        if json.loads(manifest.read_text(encoding="utf-8-sig"))["id"] != pack_id:
            raise ValueError(f"Wallpaper ID does not match the release inventory: {manifest}")
    return sorted(files)


def stage_assets(root, destination):
    files = release_files(root)
    destination.mkdir()
    for relative in files:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target)


def build_installer(compiler, manager, player, output_dir, *, qa_id=None, qa_version=None):
    compiler = compiler.resolve(strict=True)
    manager = manager.resolve(strict=True)
    player = player.resolve(strict=True)
    output_dir = output_dir.resolve()
    if qa_id is not None and not re.fullmatch(r"[a-f0-9]{32}", qa_id):
        raise ValueError("The QA installer ID must be a UUID in lowercase hex")
    if qa_version is not None and (qa_id is None or not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", qa_version)):
        raise ValueError("QA version requires a QA identity and a major.minor.patch version")
    app_version = qa_version or version()
    edition = "-QA" if qa_id is not None else ""
    filename = f"Action75-Wallpaper{edition}-Setup-{app_version}-x64.exe"
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".action75-build-", dir=output_dir) as scratch:
        workspace = Path(scratch)
        payload = workspace / "payload"
        stage_assets(ROOT, payload)
        for source, relative in ((manager, MANAGER), (player, PLAYER), (player.with_name(WORKER.name), WORKER)):
            target = payload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        subprocess.run(
            [sys.executable, str(ROOT / "tools/license-notices.py"), str(payload / "Licenses/Rust")],
            check=True,
        )
        definitions = [f"/DAppVersion={app_version}", f"/DPayloadDir={payload}"]
        if qa_id is not None:
            definitions.append(f"/DQaId={qa_id}")
        subprocess.run(
            [str(compiler), "/Q", f"/O{workspace}", *definitions, str(ROOT / "tools/setup.iss")],
            check=True,
        )
        output = output_dir / filename
        (workspace / filename).replace(output)
    return output


def main():
    parser = argparse.ArgumentParser(description="Build an installer from a fresh release payload.")
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--manager", type=Path, default=BUILT_MANAGER)
    parser.add_argument("--player", type=Path, default=ROOT / PLAYER)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release")
    parser.add_argument("--qa-id")
    parser.add_argument("--qa-version")
    args = parser.parse_args()
    path = build_installer(args.compiler, args.manager, args.player, args.output_dir, qa_id=args.qa_id, qa_version=args.qa_version)
    print(path)


if __name__ == "__main__":
    main()
