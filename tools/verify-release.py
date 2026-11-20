"""Verify a fresh QA installer, with optional offscreen playback and import checks."""

import argparse
import ctypes as C
from ctypes import wintypes as W
from functools import partial
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid
import winreg

from release_payload import (
    ROOT, BUILT_MANAGER, DEFAULT_COMPILER, MANAGER, PLAYER, WORKER,
    build_installer, inventory, release_files,
)
from release_qa_gui import KERNEL, api, checked, import_wallpaper, screenshot, wait_for


UNINSTALL = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
PRODUCTION_ID = "{81CCDD80-9A70-4694-94EB-AC6D96651B70}"
SILENT = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/NOCLOSEAPPLICATIONS"]
FLAGS = subprocess.CREATE_NO_WINDOW

open_file = api(KERNEL, "CreateFileW", W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD,
                C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
write_file = api(KERNEL, "WriteFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                 C.POINTER(W.DWORD), C.c_void_p)
read_file = api(KERNEL, "ReadFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                C.POINTER(W.DWORD), C.c_void_p)
close_handle = api(KERNEL, "CloseHandle", W.BOOL, W.HANDLE)
get_folder_path = api(C.WinDLL("shell32"), "SHGetFolderPathW", W.LONG,
                      W.HWND, C.c_int, W.HANDLE, W.DWORD, W.LPWSTR)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def registry_values(path):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            return {name: (value, kind)
                    for index in range(winreg.QueryInfoKey(key)[1])
                    for name, value, kind in [winreg.EnumValue(key, index)]}
    except FileNotFoundError:
        return None


def registration(app_id):
    return registry_values(f"{UNINSTALL}\\{app_id}_is1")


def shell_folder(identifier):
    buffer = C.create_unicode_buffer(260)
    result = get_folder_path(None, identifier, None, 0, buffer)
    if result != 0:
        raise OSError(f"Could not read Windows shell folder {identifier}: {result:#x}")
    return Path(buffer.value)


def shortcuts(name):
    return (shell_folder(2) / name / f"{name}.lnk", shell_folder(16) / f"{name}.lnk")


def production_snapshot():
    links = (*shortcuts("Action75 Wallpaper"), *shortcuts("ACTION75"))
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            autorun = winreg.QueryValueEx(key, "ACTION75")
    except FileNotFoundError:
        autorun = None
    return (
        registration(PRODUCTION_ID),
        autorun,
        tuple(digest(path) if path.is_file() else None for path in links),
    )


def pipe_request(pipe, action, ok=True, **fields):
    deadline = time.monotonic() + 5
    while True:
        handle = open_file("\\\\.\\pipe\\" + pipe, 0xC0000000, 0, None, 3, 0, None)
        if handle != C.c_void_p(-1).value:
            break
        if time.monotonic() >= deadline:
            raise C.WinError(C.get_last_error())
        time.sleep(0.01)
    try:
        data = json.dumps(dict(action=action, **fields)).encode() + b"\n"
        count = W.DWORD()
        checked(write_file(handle, data, len(data), C.byref(count), None))
        response = bytearray()
        buffer = C.create_string_buffer(65536)
        while b"\n" not in response:
            checked(read_file(handle, buffer, len(buffer), C.byref(count), None))
            assert count.value and len(response) < 1048576, action
            response.extend(buffer.raw[:count.value])
        reply = json.loads(response)
        assert bool(reply.get("ok")) == ok, reply
        return reply.get("result") if ok else reply
    finally:
        close_handle(handle)


def verify_inventory(app):
    expected = set(release_files())
    actual = {path.relative_to(app)
              for folder in ("assets", "existing-resources", "wallpapers-native", "Licenses")
              for path in (app / folder).rglob("*")
              if path.is_file() and not path.is_relative_to(app / "Licenses/Rust")}
    actual.update(path for path in expected if len(path.parts) == 1 and (app / path).is_file())
    assert actual == expected, {"missing": sorted(expected - actual), "extra": sorted(actual - expected)}
    assert all(digest(app / relative) == digest(ROOT / relative) for relative in expected)
    return sorted(inventory()["wallpapers"])


class Memory(C.Structure):
    _fields_ = [("cb", W.DWORD), ("page_faults", W.DWORD)] + [
        (name, C.c_size_t) for name in (
            "peak", "working", "paged_peak", "paged", "nonpaged_peak",
            "nonpaged", "pagefile", "pagefile_peak",
        )
    ]


def verify_performance(call, process, ids, report):
    psapi = C.WinDLL("psapi", use_last_error=True)
    get_memory = api(psapi, "GetProcessMemoryInfo", W.BOOL,
                     W.HANDLE, C.POINTER(Memory), W.DWORD)
    for pack in ids:
        assert call("selectPack", id=pack)["packId"] == pack
        call("perfReset")
        before = call("status")["framesRendered"]
        start = time.perf_counter()
        events = 0
        while time.perf_counter() - start < 4:
            call("testTrigger", code="Space", count=4)
            events += 4
            time.sleep(0.1)
        elapsed = time.perf_counter() - start
        status = call("status")
        assert status["renderer"] == "D3D11/NativePlugin" and status["nativeWorkerPid"] > 0, status
        perf = call("perfRead")["performance"]
        assert status["fps"] == 60 and status.get("error") in (None, ""), status
        measured = (status["framesRendered"] - before) / elapsed
        memory = Memory(cb=C.sizeof(Memory))
        checked(get_memory(W.HANDLE(int(process._handle)), C.byref(memory), C.sizeof(memory)))
        row = dict(pack=pack, fps=measured, seconds=elapsed, events=events,
                   workingSetMB=memory.working / 1048576, **perf)
        report["performance"].append(row)
        print(f"{pack}: {measured:.2f} FPS, {row['workingSetMB']:.2f} MB", flush=True)
        assert 56 < measured < 63, (pack, measured)
    report["checks"].append(f"All {len(ids)} wallpapers sustain bounded 60 FPS on the test GPU")


def verify_resize_recovery(call, library):
    prior = call("status")
    previous = prior["packId"]
    with tempfile.TemporaryDirectory(prefix="resize-qa-", dir=library) as folder:
        pack = Path(folder)
        shutil.copytree(ROOT / "examples/native-wallpapers/key-game/pack", pack, dirs_exist_ok=True)
        manifest = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
        manifest["id"] = pack.name
        (pack / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        call("listPacks")
        first = call("selectPack", id=pack.name)
        resized = call("testResize", width=960, height=360)
        assert resized["packId"] == pack.name and resized["nativeWorkerPid"] > 0
        assert first["nativeWorkerPid"] != resized["nativeWorkerPid"]
        (pack / manifest["entry"]).unlink()
        call("testResize", width=1920, height=720)
        wait_for(lambda: call("status")["packId"] == previous, "resize fallback")
        assert bool(call("status")["nativeWorkerPid"]) == bool(prior["nativeWorkerPid"])
    call("listPacks")


def verify_runtime(run, app, preferences, report, options):
    pipe = "Action75ReleaseQA-" + uuid.uuid4().hex
    (app / "data/player-pipe.txt").write_text(pipe, encoding="ascii")
    state = run / "state"
    state.mkdir()
    (state / "native-player.json").write_text(json.dumps({
        "version": 1, "fps": 120, "packId": "paintball-native", "visible": True,
        "paused": False, "monitorDevice": "__OFFSCREEN_QA__",
    }), encoding="utf-8")
    call = partial(pipe_request, pipe)
    ids = report["packs"]
    with (run / "player-out.log").open("wb") as stdout, (run / "player-err.log").open("wb") as stderr:
        process = subprocess.Popen([
            str(app / PLAYER), "--root", str(app), "--state-dir", str(state), "--pipe", pipe,
            "--headless-test", "--headless-hardware", "--perf-stats",
        ], stdout=stdout, stderr=stderr, creationflags=FLAGS)
        try:
            status = call("status")
            assert status["playerPid"] == process.pid and status["headless"] and not status["rawInputRegistered"]
            assert status["fps"] == 60
            for rate in (30, 120):
                call("setSettings", ok=False, settings={"fps": rate})
            report["checks"].append("Legacy 120 setting normalizes to 60; 30 and 120 are rejected")
            initial = status["packId"]
            call("selectPack", ok=False, id="not-a-wallpaper")
            assert call("status")["packId"] == initial
            report["checks"].append("Failed wallpaper selection preserves the current wallpaper")

            call("getPackSettings", id="paintball-native")
            profile = state / "wallpaper-settings/paintball-native.json"
            saved = profile.read_bytes()
            call("previewPackSettings", id="paintball-native", values={"brightness": 42})
            assert profile.read_bytes() == saved
            call("setPackSettings", id="paintball-native", values={"brightness": 42})
            assert call("getPackSettings", id="paintball-native")["values"]["brightness"] == 42
            call("resetPackSettings", id="paintball-native")
            report["checks"].append("Preview leaves the profile on disk unchanged; commit persists it")
            if options.skip_performance:
                for pack in ids:
                    assert call("selectPack", id=pack)["packId"] == pack
                report["checks"].append(f"All {len(ids)} bundled wallpapers load")
            else:
                verify_performance(call, process, ids, report)
            call("selectPack", id="paintball-native")

            verify_resize_recovery(call, app / "wallpapers-native")
            report["checks"].append("Resize rebuilt the plugin and restored the prior wallpaper after a failed rebuild")
            target = run / "migrated-packs"
            installed_packs = app / "wallpapers-native"
            installed_packs.rename(target)
            preferences["wallpaperDir"] = str(target)
            (app / "data/app-settings.json").write_text(json.dumps(preferences), encoding="utf-8")
            call("reloadDirectories")
            packs = call("listPacks")
            assert {pack["id"] for pack in packs} == set(ids)
            assert all(Path(pack["path"]).is_relative_to(target) for pack in packs)
            assert call("status")["packId"] == "paintball-native"
            call("selectPack", id="black-hole")
            assert call("status")["nativeWorkerPid"] > 0
            call("selectPack", id="paintball-native")
            report["checks"].append("Live directory reload switches every catalog path and preserves playback")

            # An upgrade lays down a new copy of a shipped pack at the install path.
            upgraded = "glass-shatter"
            shutil.copytree(ROOT / "wallpapers-native" / upgraded, installed_packs / upgraded)
            call("reloadDirectories")
            packs = {pack["id"]: Path(pack["path"]) for pack in call("listPacks")}
            assert set(packs) == set(ids)
            assert packs[upgraded].samefile(installed_packs / upgraded)
            assert all(path.is_relative_to(target) for name, path in packs.items() if name != upgraded)
            assert call("status")["packId"] == "paintball-native"
            report["checks"].append("Upgraded bundled wallpaper takes precedence over its migrated copy")

            library_image = run / "eagle-library.png"
            report["screenshots"] = [str(library_image)]
            source = run / "author-glow"
            shutil.copytree(ROOT / "docs/wallpaper-import-evidence/author-glow", source)
            assert "author-glow" not in ids
            imported = target / "author-glow" / "manifest.json"
            import_wallpaper(app / MANAGER, app, source, imported, library_image, click=options.import_click)
            manifest = json.loads(imported.read_text(encoding="utf-8"))
            assert manifest["id"] == "author-glow"
            entries = call("listPacks")
            assert "author-glow" in {pack["id"] for pack in entries}
            assert call("status")["packId"] == "paintball-native"
            assert call("selectPack", id="author-glow")["packId"] == "author-glow"
            assert call("testTrigger", code="Space", count=1)["handle"] >= 0
            report["checks"].append("Installed library's Import Wallpaper button, native picker, trial and selection pass")

            for label, extra in (
                ("compact", ["--screenshot-size", "1080", "720"]),
                ("settings", ["--screenshot-tab", "settings"]),
                ("devices", ["--screenshot-tab", "devices"]),
                ("calibration", ["--screenshot-tab", "calibration"]),
            ):
                image = run / f"eagle-{label}.png"
                screenshot(app / MANAGER, app, image, *extra)
                report["screenshots"].append(str(image))
        finally:
            if process.poll() is None:
                try:
                    call("stop", expectedPlayerPid=process.pid, expectedRuntimeRoot=str(app))
                    process.wait(timeout=10)
                except (OSError, subprocess.TimeoutExpired):
                    process.terminate()
                    process.wait(timeout=5)


def verify_install(run, installer, qa_id, report, options):
    app = run / "installed"
    test_id = "Action75WallpaperQA-" + qa_id
    test_links = shortcuts("Action75 Wallpaper QA " + qa_id)
    before = production_snapshot()
    report["productionRegistrationPresentBefore"] = before[0] is not None
    uninstaller = app / "unins000.exe"
    data = app / "data"
    settings = data / "app-settings.json"
    calibration = data / "fm-calibration.json"
    saved = None
    calibration_hash = None
    args = [str(installer), *SILENT, f"/DIR={app}", "/TASKS=desktopicon"]
    assert registration(test_id) is None and not any(link.exists() for link in test_links)
    try:
        subprocess.run([*args, f"/LOG={run / 'install.log'}"], check=True, timeout=90, creationflags=FLAGS)
        installed = registration(test_id)
        assert installed and Path(installed["InstallLocation"][0]) == app
        assert all(link.is_file() for link in test_links)
        assert production_snapshot() == before, "QA install affected the production registration or shortcuts"
        assert all((app / binary).is_file() for binary in (MANAGER, PLAYER, WORKER))
        report["installedRoot"] = str(app)
        report["packs"] = verify_inventory(app)
        report["checks"].append(f"Test registration and shortcuts are isolated; exactly {len(report['packs'])} bundles")
        data.mkdir()
        shutil.copy2(app / "assets/defaults/fm-calibration.json", calibration)
        calibration_hash = digest(calibration)
        preferences = {"setupComplete": True, "panelOpen": False, "wallpaperDir": "", "logDir": ""}
        settings.write_text(json.dumps(preferences), encoding="utf-8")
        if not options.install_only:
            verify_runtime(run, app, preferences, report, options)
        saved = settings.read_bytes()
        assert digest(calibration) == calibration_hash

        subprocess.run([*args, f"/LOG={run / 'reinstall.log'}"], check=True, timeout=90, creationflags=FLAGS)
        assert settings.read_bytes() == saved and digest(calibration) == calibration_hash
        assert production_snapshot() == before, "QA reinstall affected the production installation"
        report["checks"].append("Reinstall preserves preferences and calibration")
    finally:
        try:
            if uninstaller.is_file():
                subprocess.run([str(uninstaller), *SILENT, f"/LOG={run / 'uninstall.log'}"],
                               check=True, timeout=60, creationflags=FLAGS)
            wait_for(lambda: registration(test_id) is None and not any(link.exists() for link in test_links),
                     "test registration and shortcuts to be removed")
            if saved is not None:
                assert settings.read_bytes() == saved and digest(calibration) == calibration_hash
                report["checks"].append("Uninstall removes only the QA registration and shortcuts and keeps user data")
        finally:
            assert production_snapshot() == before, "Production registration, shortcuts or autorun changed"
            report["checks"].append("Production registration, shortcuts and autorun match the pretest snapshot")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--manager", type=Path, default=BUILT_MANAGER)
    parser.add_argument("--player", type=Path, default=ROOT / PLAYER)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts")
    parser.add_argument("--install-only", action="store_true", help="Verify only install, reinstall and uninstall.")
    parser.add_argument("--skip-performance", action="store_true", help="Load every wallpaper without the 60 FPS stress test.")
    parser.add_argument("--import-click", type=float, nargs=2, metavar=("X", "Y"),
                        help="Override the import button click in logical pixels.")
    options = parser.parse_args()
    options.output_dir.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="release-qa-", dir=options.output_dir.resolve()))
    qa_id = uuid.uuid4().hex
    report = {"qaId": qa_id, "checks": [], "performance": []}
    try:
        installer = build_installer(options.compiler, options.manager, options.player, run, qa_id=qa_id)
        report["installer"] = str(installer)
        verify_install(run, installer, qa_id, report, options)
        report["passed"] = True
    finally:
        path = run / "verification.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path, flush=True)


if __name__ == "__main__":
    main()
