"""Collect notices from the locked Windows dependency graph used by the manager."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from release_build import cargo_executable


def upstream_notices(output, package, supplements):
    entry = supplements.get(package)
    if not entry or not entry["notices"]:
        raise ValueError(f"No license text found for {package}; review its locked upstream release")
    declarations = []
    for notice in entry["notices"]:
        relative = Path(notice["file"])
        source = Path(__file__).with_name("rust-licenses") / relative
        content = source.read_bytes().replace(b"\r\n", b"\n")
        if not content.strip() or hashlib.sha256(content).hexdigest() != notice["sha256"]:
            raise ValueError(f"Invalid upstream license text for {package}: {source}")
        target = Path("Shared") / relative
        (output / target).parent.mkdir(parents=True, exist_ok=True)
        (output / target).write_bytes(content)
        declarations.append({"path": target.as_posix(), "source": notice["source"],
                             "sha256": notice["sha256"]})
    return declarations


def collect(output):
    manifest = Path(__file__).resolve().parents[1] / "apps/manager/Cargo.toml"
    graph = subprocess.check_output([
        cargo_executable(), "metadata", "--locked", "--offline", "--format-version", "1",
        "--filter-platform", "x86_64-pc-windows-msvc", "--manifest-path", manifest,
    ])
    packages = [p for p in json.loads(graph)["packages"]
                if (p["source"] or "").startswith("registry+")
                or Path(p["manifest_path"]).is_relative_to(manifest.parent / "vendor")]
    if not packages:
        raise ValueError("No locked Windows registry dependencies were found")
    supplements = json.loads(
        (Path(__file__).with_name("rust-licenses") / "upstream.json").read_text(encoding="utf-8"))
    declarations = []
    collected = set()
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
        entry = {"package": name, "license": package["license"],
                 "authors": package["authors"], "repository": package["repository"]}
        if notices:
            entry["notices"] = []
            for notice in sorted(notices):
                if not notice.read_bytes().strip():
                    raise ValueError(f"Empty license file for {name}: {notice}")
                target = Path(name) / notice.relative_to(source)
                (output / target).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(notice, output / target)
                entry["notices"].append({"path": target.as_posix()})
        else:
            entry["notices"] = upstream_notices(output, name, supplements)
            if selection := supplements[name].get("selectedLicense"):
                if selection not in package["license"].split(" OR "):
                    raise ValueError(f"License selection does not match {name}")
                entry["selectedLicense"] = selection
        collected.update(notice["path"] for notice in entry["notices"])
        declarations.append(entry)
    (output / "packages.json").write_text(
        json.dumps(declarations, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Rust license notices: {len(collected)} files, {len(packages)} package declarations")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: license-notices.py <output-directory>")
    collect(Path(sys.argv[1]))
