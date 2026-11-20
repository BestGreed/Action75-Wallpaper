"""Collect notices from the locked Windows dependency graph used by the manager."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def collect(output):
    manifest = Path(__file__).resolve().parents[1] / "apps/manager/Cargo.toml"
    cargo_home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo"))
    cargo = shutil.which("cargo") or cargo_home / "bin/cargo.exe"
    graph = subprocess.check_output([
        cargo, "metadata", "--locked", "--offline", "--format-version", "1",
        "--filter-platform", "x86_64-pc-windows-msvc", "--manifest-path", manifest,
    ])
    packages = [p for p in json.loads(graph)["packages"]
                if (p["source"] or "").startswith("registry+")
                or Path(p["manifest_path"]).is_relative_to(manifest.parent / "vendor")]
    if not packages:
        raise ValueError("No locked Windows registry dependencies were found")
    declarations = []
    count = 0
    for package in packages:
        name = f"{package['name']}-{package['version']}"
        package_manifest = Path(package["manifest_path"])
        if not package_manifest.is_file():
            raise FileNotFoundError(f"Missing locked crate sources for {name}")
        source = package_manifest.parent
        notices = {p for p in source.rglob("*") if p.is_file() and p.name.upper().startswith(
            ("LICENSE", "LICENCE", "COPYING", "NOTICE", "OFL", "UNLICENSE"))}
        if package["license_file"]:
            notices.add(source / package["license_file"])
        if not package["license"] and not notices:
            raise ValueError(f"No license declaration or notice found for {name}")
        declarations.append({"package": name, "license": package["license"],
                             "authors": package["authors"], "repository": package["repository"]})
        for notice in notices:
            target = output / name / notice.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(notice, target)
            count += 1
    if not count:
        raise ValueError("No Rust dependency notice files were collected")
    (output / "packages.json").write_text(
        json.dumps(declarations, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Rust license notices: {count} files, {len(packages)} package declarations")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: license-notices.py <output-directory>")
    collect(Path(sys.argv[1]))
