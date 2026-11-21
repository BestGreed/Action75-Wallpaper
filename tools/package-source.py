"""Archive source, the author SDK and declared distributable assets."""
import argparse
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile
from release_payload import ROOT, digest, release_asset, release_files, version


SOURCE_ENTRIES = [
    "README.md", "PRODUCT.md", "DESIGN.md", "GLOSSARY.md", ".gitignore",
    ".clang-format", ".impeccable/design.json", "docs",
    "apps/manager", "apps/manager/vendor/egui-wgpu", "native/windows-player", "tools", "skills/action75-wallpaper-author",
    "examples/author-wallpapers", "examples/native-wallpapers",
    "examples/author-wallpapers/sources/dx12/night-orbit-trio/vendor",
    "examples/author-wallpapers/sources/dx12/soundings-echo/vendor",
    "vendor-source/windows-native/nlohmann/json.hpp",
]
JSON_HEADER = Path("vendor-source/windows-native/nlohmann/json.hpp")
JSON_SHA256 = "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"


def write_archive(root, output_dir, *, wallpapers=None):
    root = root.resolve(strict=True)
    if wallpapers is not None:
        wallpapers = wallpapers.resolve(strict=True)
    app_version = version(root)
    output = output_dir / f"ACTION75-source-{app_version}.zip"
    excluded = {"target", "build", "vendor", "__pycache__", "logs", "data", "artifacts"}
    files = set(release_files(root, include_external=False, wallpapers=wallpapers))
    if digest(root / JSON_HEADER) != JSON_SHA256:
        raise ValueError("SDK requires the unmodified nlohmann/json 3.12.0 header")
    for optional in ("BUILDING.md", ".github/workflows/windows.yml"):
        if (root / optional).is_file():
            files.add(Path(optional))
    for entry in SOURCE_ENTRIES:
        path = root / entry
        if not path.exists():
            raise FileNotFoundError(path)
        candidates = [path] if path.is_file() else path.rglob("*")
        for candidate in candidates:
            relative = candidate.relative_to(root)
            if (candidate.is_file() and not excluded.intersection(candidate.relative_to(path).parts)
                    and not any(part.startswith("build-") for part in relative.parent.parts)
                    and candidate.suffix not in {".log", ".pyc"}):
                files.add(relative)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".action75-source-", dir=output_dir) as scratch:
        temporary = Path(scratch) / output.name
        with ZipFile(temporary, "w", ZIP_DEFLATED, compresslevel=9) as archive:
            for relative in sorted(files):
                source = release_asset(root, relative, wallpapers)
                if source.resolve(strict=True) != source:
                    raise ValueError(f"Source archive input redirects to another path: {relative}")
                archive.write(source, f"ACTION75-{app_version}/{relative.as_posix()}")
        temporary.replace(output)
    print(f"{output}: {len(files)} files, {output.stat().st_size} bytes")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release")
    parser.add_argument("--wallpapers", type=Path, help="Use the built-in packs from a verified staging directory.")
    args = parser.parse_args()
    write_archive(ROOT, args.output_dir, wallpapers=args.wallpapers)


if __name__ == "__main__":
    main()
