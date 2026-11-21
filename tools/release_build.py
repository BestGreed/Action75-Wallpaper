"""Build and verify a receipt for the three programs in a distribution."""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from release_payload import MANAGER, PLAYER, ROOT, WORKER, digest, version


OUTPUTS = {
    MANAGER: Path("cargo/release") / MANAGER.name,
    PLAYER: Path("native") / PLAYER,
    WORKER: Path("native") / WORKER,
}


def cargo_executable():
    return shutil.which("cargo") or Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo")) / "bin/cargo.exe"


def file_version(path):
    class FixedInfo(c.Structure):
        _fields_ = [(name, c.c_uint32) for name in ("signature", "structure", "ms", "ls")]

    api = c.WinDLL("version", use_last_error=True)
    size = api.GetFileVersionInfoSizeW(c.c_wchar_p(str(path)), None)
    data = c.create_string_buffer(size)
    if not size or not api.GetFileVersionInfoW(c.c_wchar_p(str(path)), 0, size, data):
        raise ValueError(f"No Windows file version in {path}")
    info = c.POINTER(FixedInfo)()
    count = c.c_uint()
    if (not api.VerQueryValueW(data, c.c_wchar_p("\\"), c.byref(info), c.byref(count))
            or count.value < c.sizeof(FixedInfo) or info.contents.signature != 0xFEEF04BD):
        raise ValueError(f"No fixed Windows file version in {path}")
    ms, ls = info.contents.ms, info.contents.ls
    return f"{ms >> 16}.{ms & 65535}.{ls >> 16}"


def build_inputs(root, qa_id):
    return {
        "version": version(root),
        "cargoLockSha256": digest(root / "apps/manager/Cargo.lock"),
        "assetInventorySha256": digest(root / "tools/release-assets.json"),
        "updateQaIdentity": qa_id,
    }


def validate_receipt(root, receipt, qa_id=None):
    receipt = receipt.resolve(strict=True)
    build = json.loads(receipt.read_text(encoding="utf-8"))
    expected = build_inputs(root, build["updateQaIdentity"])
    if any(build.get(key) != value for key, value in expected.items()):
        raise ValueError("Build receipt does not match the version, dependency lock or asset inventory")
    if build["updateQaIdentity"] is not None and build["updateQaIdentity"] != qa_id:
        raise ValueError("A test update build can only be packaged for its own QA identity")
    binaries = {relative: receipt.parent / path for relative, path in OUTPUTS.items()}
    if set(build["sha256"]) != {relative.as_posix() for relative in binaries}:
        raise ValueError("Build receipt must cover the controller, player and worker")
    for relative, binary in binaries.items():
        if not binary.is_file() or digest(binary) != build["sha256"][relative.as_posix()]:
            raise ValueError(f"Missing or mismatched build output: {binary}")
        if relative != WORKER and file_version(binary) != build["version"]:
            raise ValueError(f"Wrong executable version: {binary}")
    return build, binaries


def build_release(output, *, root=ROOT, qa_id=None, update_feed=None, cargo=None, test=False):
    if (qa_id is None) != (update_feed is None) or (
        qa_id is not None and not re.fullmatch(r"[a-f0-9]{32}", qa_id)
    ):
        raise ValueError("An update QA build requires a UUID identity and a local feed")
    root = root.resolve()
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / "build.json"
    receipt.unlink(missing_ok=True)
    inputs = build_inputs(root, qa_id)
    env = dict(os.environ)
    command = [cargo or cargo_executable(), "build", "--release", "--locked", "--target-dir", output / "cargo"]
    if qa_id is not None:
        command.extend(("--features", "update-qa"))
        env.update(ACTION75_QA_INSTALL_ID=qa_id, ACTION75_QA_UPDATE_DIR=str(update_feed.resolve()))
    if test:
        subprocess.run([command[0], "test", *command[2:]], cwd=root / "apps/manager", env=env, check=True)
    subprocess.run(command, cwd=root / "apps/manager", env=env, check=True)
    native = ["pwsh", "-NoProfile", "-File", root / "tools/build-player.ps1", "-BuildRoot", output / "native"]
    if test:
        native.append("-Test")
    subprocess.run(native, check=True, cwd=root)
    if inputs != build_inputs(root, qa_id):
        raise ValueError("Release inputs changed during the build")
    for relative, path in OUTPUTS.items():
        binary = output / path
        if relative != WORKER and file_version(binary) != inputs["version"]:
            raise ValueError(f"Wrong executable version: {binary}")
    inputs["sha256"] = {relative.as_posix(): digest(output / path) for relative, path in OUTPUTS.items()}
    with tempfile.TemporaryDirectory(prefix=".action75-receipt-", dir=output) as scratch:
        pending = Path(scratch) / receipt.name
        pending.write_text(json.dumps(inputs, indent=2) + "\n", encoding="utf-8")
        pending.replace(receipt)
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--test", action="store_true")
    args = parser.parse_args()
    print(build_release(args.output_dir, test=args.test))
