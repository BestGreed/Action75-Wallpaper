"""Build a source archive without local state, binaries or downloaded dependencies."""
import argparse
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile
from release_payload import ROOT, external_wallpaper_assets, release_files, version


SOURCE_ENTRIES = [
    "README.md", "PRODUCT.md", "DESIGN.md", "GLOSSARY.md", ".gitignore",
    ".clang-format", ".impeccable/design.json", "docs",
    "apps/manager", "apps/manager/vendor/egui-wgpu", "native/windows-player", "tools", "skills/action75-wallpaper-author",
    "examples/author-wallpapers", "examples/native-wallpapers",
]


def write_archive(root, output_dir):
    app_version = version(root)
    output = output_dir / f"ACTION75-source-{app_version}.zip"
    excluded = {"target", "build", "vendor", "__pycache__", "logs", "data", "artifacts"}
    files = set(release_files(root)) - external_wallpaper_assets(root)
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
                archive.write(root / relative, f"ACTION75-{app_version}/{relative.as_posix()}")
        temporary.replace(output)
    print(f"{output}: {len(files)} files, {output.stat().st_size} bytes")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release")
    args = parser.parse_args()
    write_archive(ROOT, args.output_dir)


if __name__ == "__main__":
    main()
