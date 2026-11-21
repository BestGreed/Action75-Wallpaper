"""Compare active glass frames using temporary builds with the same RNG seed."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
PACK = "glass-shatter"
SEED = "0xa751u"


def replace_once(path, before, after):
    source = path.read_text(encoding="utf-8")
    if source.count(before) != 1:
        raise RuntimeError(f"Unexpected seed expression in {path.name}")
    path.write_text(source.replace(before, after), encoding="utf-8")


def build(source, build_dir, target, *options):
    subprocess.run(["cmake", "-S", source, "-B", build_dir, "-G", "Visual Studio 17 2022", "-A", "x64",
                    *options], check=True)
    subprocess.run(["cmake", "--build", build_dir, "--config", "Release", "--target", target,
                    "--parallel", "3"], check=True)


def baseline_build(reference, scratch, fxc, sdk, headers):
    source = scratch / "reference"
    source.mkdir()
    files = ("GlassShatter.cpp", "GlassShatter.h", "GlassShatter.hlsl", "PackSettings.cpp",
             "PackSettings.h", "LayerColor.h", "VariantPlayback.h", "Wallpaper.cpp")
    for name in files:
        shutil.copy2(reference / name, source / name)
    palette = ROOT / "native" / "windows-player" / "include" / "EffectPalette.h"
    shutil.copy2(palette, source / palette.name)
    replace_once(source / "Wallpaper.cpp", "  Scene scene;", f"  Scene scene{{{SEED}}};")
    shader_entries = ("backdropVS", "backdropPS", "crackVS", "crackPS", "paneVS", "panePS")
    headers_list = []
    for entry in shader_entries:
        symbol = "Glass" + entry[0].upper() + entry[1:]
        name = symbol + ".h"
        profile = "vs_5_0" if entry.endswith("VS") else "ps_5_0"
        subprocess.run([fxc, "/nologo", "/O3", "/T", profile, "/E", entry, "/Vn", symbol,
                        "/Fh", source / name, source / "GlassShatter.hlsl"], check=True)
        headers_list.append(f'#include "{name}"\n')
    (source / "GlassShatterShaders.h").write_text("".join(headers_list), encoding="utf-8")
    (source / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.24)
project(GlassReference LANGUAGES CXX)
add_library(wallpaper MODULE Wallpaper.cpp PackSettings.cpp GlassShatter.cpp)
target_compile_features(wallpaper PRIVATE cxx_std_17)
target_compile_options(wallpaper PRIVATE /utf-8)
target_compile_definitions(wallpaper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN A75_GLASS ACTION75_WALLPAPER_EXPORTS)
target_include_directories(wallpaper PRIVATE "{sdk.as_posix()}" "{headers.as_posix()}")
target_link_libraries(wallpaper PRIVATE d3d11)
set_property(TARGET wallpaper PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
""", encoding="utf-8")
    build_dir = scratch / "build-reference"
    build(source, build_dir, "wallpaper")
    return build_dir / "Release" / "wallpaper.dll"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("replay", "baseline", "packs", "output", "fxc"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    sdk = ROOT / "skills" / "action75-wallpaper-author" / "sdk"
    headers = ROOT / "vendor-source" / "windows-native"
    with tempfile.TemporaryDirectory(prefix="a75-glass-seed-") as temporary:
        scratch = Path(temporary)
        old_dll = baseline_build(args.baseline / "reference", scratch, args.fxc, sdk, headers)
        new_source = scratch / "source"
        shutil.copytree(HERE.parent, new_source)
        replace_once(new_source / "GlassShatter.cpp",
                     "random_(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()))",
                     f"random_({SEED})")
        build_dir = scratch / "build"
        build(new_source, build_dir, f"wallpaper_{PACK}",
              f"-DACTION75_SURFACE_SDK={sdk}", f"-DACTION75_SURFACE_JSON={headers}")
        new_dll = build_dir / "native-wallpapers" / PACK / "wallpaper.dll"
        old_pack = scratch / "old" / PACK
        new_pack = scratch / "new" / PACK
        for library, package, dll in ((args.baseline, old_pack, old_dll), (args.packs, new_pack, new_dll)):
            shutil.copytree(library / PACK, package)
            shutil.copy2(dll, package / "wallpaper.dll")
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "fixtures.json").write_text(json.dumps(
            {"rngSeed": SEED, "d3d11DllSha256": digest(old_dll), "d3d12DllSha256": digest(new_dll),
             "d3d11SourceSnapshot": str(args.baseline.resolve() / "reference")}, indent=2) + "\n",
            encoding="utf-8")
        subprocess.run([sys.executable, HERE / "compare.py", "--replay", args.replay, "--baseline",
                        old_pack.parent, "--packs", new_pack.parent, "--output", args.output,
                        "--pack", PACK, "--fixed-glass-seed"], check=True)


if __name__ == "__main__":
    main()
