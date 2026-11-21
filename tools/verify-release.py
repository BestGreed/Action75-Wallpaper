"""Verify a fresh QA installer, with optional offscreen playback and import checks."""

import argparse
from contextlib import contextmanager
import ctypes as C
from ctypes import wintypes as W
from functools import partial
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid
import winreg

from release_payload import (
    ROOT, DEFAULT_COMPILER, MANAGER, PLAYER, WORKER,
    build_installer, digest, inventory, release_asset, release_files,
)
from release_qa_gui import (KERNEL, api, checked, import_wallpaper, installer_on_desktop,
                            manager_session, screenshot, wait_for, wait_for_process)


UNINSTALL = r"Software\Microsoft\Windows\CurrentVersion\Uninstall"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
PRODUCTION_ID = "{81CCDD80-9A70-4694-94EB-AC6D96651B70}"
SILENT = ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/NOCLOSEAPPLICATIONS"]
FLAGS = subprocess.CREATE_NO_WINDOW
LEGACY_ABI1_PACK = ROOT / "tools/fixtures/key-game-abi1/pack"

open_file = api(KERNEL, "CreateFileW", W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD,
                C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
write_file = api(KERNEL, "WriteFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                 C.POINTER(W.DWORD), C.c_void_p)
read_file = api(KERNEL, "ReadFile", W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                C.POINTER(W.DWORD), C.c_void_p)
close_handle = api(KERNEL, "CloseHandle", W.BOOL, W.HANDLE)
get_folder_path = api(C.WinDLL("shell32"), "SHGetFolderPathW", W.LONG,
                      W.HWND, C.c_int, W.HANDLE, W.DWORD, W.LPWSTR)


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


def verify_inventory(app, wallpapers):
    expected = set(release_files(wallpapers=wallpapers))
    actual = {path.relative_to(app)
              for folder in ("assets", "existing-resources", "wallpapers-native", "Licenses")
              for path in (app / folder).rglob("*")
              if path.is_file() and not path.is_relative_to(app / "Licenses/Rust")}
    actual.update(path for path in expected if len(path.parts) == 1 and (app / path).is_file())
    assert actual == expected, {"missing": sorted(expected - actual), "extra": sorted(actual - expected)}
    assert all(digest(app / relative) == digest(release_asset(ROOT, relative, wallpapers))
               for relative in expected)
    return sorted(inventory()["wallpapers"])


class Memory(C.Structure):
    _fields_ = [("cb", W.DWORD), ("page_faults", W.DWORD)] + [
        (name, C.c_size_t) for name in (
            "peak", "working", "paged_peak", "paged", "nonpaged_peak",
            "nonpaged", "pagefile", "pagefile_peak",
        )
    ]


def verify_performance(call, process, renderers, report):
    psapi = C.WinDLL("psapi", use_last_error=True)
    get_memory = api(psapi, "GetProcessMemoryInfo", W.BOOL,
                     W.HANDLE, C.POINTER(Memory), W.DWORD)
    open_process = api(KERNEL, "OpenProcess", W.HANDLE, W.DWORD, W.BOOL, W.DWORD)

    def working_set_mib(handle):
        memory = Memory(cb=C.sizeof(Memory))
        checked(get_memory(handle, C.byref(memory), C.sizeof(memory)))
        return memory.working / 1048576

    for pack, renderer in renderers.items():
        selected = call("selectPack", id=pack)
        assert selected["packId"] == pack and selected["renderer"] == renderer, selected
        assert selected["nativeWorkerPid"] > 0, selected
        call("perfReset")
        before = call("status")["framesRendered"]
        start = time.perf_counter()
        delivered = 0
        while time.perf_counter() - start < 4:
            receipt = call("testTrigger", code="Space", count=4)
            assert receipt["acceptedTriggers"] == 4, receipt
            delivered += receipt["acceptedTriggers"]
            time.sleep(0.1)
        elapsed = time.perf_counter() - start
        status = call("status")
        worker_pid = status["nativeWorkerPid"]
        assert status["renderer"] == renderer and worker_pid == selected["nativeWorkerPid"], status
        perf = call("perfRead")["performance"]
        assert status["fps"] == 60 and status.get("error") in (None, ""), status
        measured = (status["framesRendered"] - before) / elapsed
        host_mib = working_set_mib(W.HANDLE(int(process._handle)))
        worker_handle = checked(open_process(0x1000, False, worker_pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        try:
            worker_mib = working_set_mib(worker_handle)
        finally:
            close_handle(worker_handle)
        row = dict(pack=pack, renderer=renderer, fps=measured, seconds=elapsed, deliveredKeyEvents=delivered,
                   adapter=status["gpuAdapter"], adapterLuid=status["gpuAdapterLuid"],
                   player=dict(pid=process.pid, workingSetMiB=host_mib),
                   worker=dict(pid=worker_pid, workingSetMiB=worker_mib))
        row[f"player{renderer.split('/')[0]}Perf"] = perf
        report["performance"].append(row)
        print(f"{pack}: {measured:.2f} FPS, player {host_mib:.2f} MiB, worker {worker_mib:.2f} MiB", flush=True)
        assert 56 < measured < 63, (pack, measured)
    report["checks"].append(f"All {len(renderers)} wallpapers sustain bounded 60 FPS on the test GPU")


def verify_resize_recovery(call, library):
    prior = call("status")
    previous = prior["packId"]
    with tempfile.TemporaryDirectory(prefix="resize-qa-", dir=library) as folder:
        pack = Path(folder)
        shutil.copytree(LEGACY_ABI1_PACK, pack, dirs_exist_ok=True)
        manifest = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
        manifest["id"] = pack.name
        (pack / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        call("listPacks")
        first = call("selectPack", id=pack.name)
        assert first["renderer"] == "D3D11/NativePlugin", first
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
    renderers = {}
    for pack in ids:
        manifest = json.loads((app / "wallpapers-native" / pack / "manifest.json").read_bytes())
        renderers[pack] = "D3D12/NativePlugin" if manifest["abi"] == 2 else "D3D11/NativePlugin"
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
                for pack, renderer in renderers.items():
                    selected = call("selectPack", id=pack)
                    assert selected["packId"] == pack and selected["renderer"] == renderer, selected
                    assert selected["nativeWorkerPid"] > 0, selected
                report["checks"].append(f"All {len(ids)} bundled wallpapers load")
            else:
                verify_performance(call, process, renderers, report)
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
            shutil.copytree(options.wallpapers / upgraded, installed_packs / upgraded)
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
            frame = state / "test-frame.png"

            def capture():
                call("testFrame")
                return frame.read_bytes()

            idle = capture()
            receipt = call("testTrigger", code="Space")
            assert receipt["handle"] >= 0, receipt
            wait_for(lambda: capture() != idle, "imported wallpaper key feedback", seconds=1)
            report["checks"].append("Imported wallpaper selection and visible key feedback pass")

            legacy = "night-platform-runner"
            legacy_manifest = target / legacy / "manifest.json"
            import_wallpaper(app / MANAGER, app, LEGACY_ABI1_PACK, legacy_manifest,
                             run / "legacy-abi1-import.png", click=options.import_click)
            call("listPacks")
            selected = call("selectPack", id=legacy)
            assert selected["renderer"] == "D3D11/NativePlugin" and selected["nativeWorkerPid"], selected
            call("setPackSettings", id=legacy, values={"amber_lights": True})
            assert call("getPackSettings", id=legacy)["values"]["amber_lights"] is True
            restored = call("selectPack", id="paintball-native")
            assert restored["renderer"] == renderers["paintball-native"], restored
            report["checks"].append("Third-party ABI 1 imports, plays, persists settings and switches back")

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
        report["packs"] = verify_inventory(app, options.wallpapers)
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
        with headless_playback(run, app, "afterimage") as (process, status):
            set_qa_startup(qa_id, app)
            if options.exercise_blocked_uninstall:
                verify_blocked_uninstall(run, app, process, test_id)
                report["checks"].append("Uninstall aborts when the existing player cannot exit")
            with manager_session(app / MANAGER, app) as (panel, windows):
                wait_for(lambda: windows("Action75 Wallpaper"), "isolated controller", seconds=40)
                command = [uninstaller, *SILENT, f"/LOG={run / 'uninstall.log'}"]
                with installer_on_desktop(command, app, panel) as uninstall:
                    assert wait_for_process(uninstall, 60) == 0
                assert wait_for_process(panel, 10) == 0
            wait_for(lambda: registration(test_id) is None and not any(link.exists() for link in test_links),
                     "test registration and shortcuts to be removed")
            assert process.wait(timeout=10) == 0
            assert_process_exited(status["nativeWorkerPid"])
            assert get_qa_startup(qa_id) is None
            report["uninstalledWhilePlaying"] = {"controllerPid": panel.pid, "playerPid": process.pid,
                                                "workerPid": status["nativeWorkerPid"]}
            report["checks"].append("Uninstall stops the controller, player and worker and removes its own startup entry")
    finally:
        try:
            if uninstaller.is_file() and registration(test_id) is not None:
                subprocess.run([str(uninstaller), *SILENT, f"/LOG={run / 'uninstall.log'}"],
                               check=True, timeout=60, creationflags=FLAGS)
            wait_for(lambda: registration(test_id) is None and not any(link.exists() for link in test_links),
                     "test registration and shortcuts to be removed")
            if saved is not None:
                assert settings.read_bytes() == saved and digest(calibration) == calibration_hash
                report["checks"].append("Uninstall removes only the QA registration and shortcuts and keeps user data")
        finally:
            remove_qa_startup(qa_id)
            assert production_snapshot() == before, "Production registration, shortcuts or autorun changed"
            report["checks"].append("Production registration, shortcuts and autorun match the pretest snapshot")


@contextmanager
def headless_playback(run, app, pack_id, paused=False):
    data = app / "data"
    data.mkdir(exist_ok=True)
    pipe = "Action75ReleaseQA-" + uuid.uuid4().hex
    (data / "player-pipe.txt").write_text(pipe, encoding="ascii")
    state = Path(tempfile.mkdtemp(prefix="player-state-", dir=run))
    (state / "native-player.json").write_text(json.dumps({
        "version": 1, "fps": 60, "packId": pack_id, "paused": paused,
        "visible": True, "monitorDevice": "__OFFSCREEN_QA__",
    }), encoding="utf-8")
    with (state / "player.log").open("wb") as log:
        process = subprocess.Popen([app / PLAYER, "--root", app, "--state-dir", state,
                                    "--pipe", pipe, "--headless-test"],
                                   stdout=log, stderr=subprocess.STDOUT, creationflags=FLAGS)
        try:
            status = pipe_request(pipe, "status")
            assert status["playerPid"] == process.pid and status["headless"], status
            assert status["packId"] == pack_id and status["paused"] == paused, status
            assert status["nativeWorkerPid"] > 0, status
            yield process, status
        finally:
            if process.poll() is None:
                try:
                    pipe_request(pipe, "stop", expectedPlayerPid=process.pid, expectedRuntimeRoot=str(app))
                    process.wait(timeout=10)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)


def assert_process_exited(pid):
    open_process = api(KERNEL, "OpenProcess", W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
    wait = api(KERNEL, "WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)
    handle = open_process(0x100000, False, pid)
    if handle:
        try:
            assert wait(handle, 5000) == 0, pid
        finally:
            close_handle(handle)
    else:
        assert C.get_last_error() == 87, C.WinError(C.get_last_error())


def set_qa_startup(qa_id, root):
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, access=winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, f"ACTION75-QA-{qa_id}", 0, winreg.REG_SZ,
                         f'"{root / MANAGER}" --background')


def get_qa_startup(qa_id):
    return (registry_values(RUN_KEY) or {}).get(f"ACTION75-QA-{qa_id}")


def remove_qa_startup(qa_id):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, access=winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, f"ACTION75-QA-{qa_id}")
    except FileNotFoundError:
        pass


def verify_blocked_uninstall(run, app, process, test_id):
    import psutil

    player = psutil.Process(process.pid)
    player.suspend()
    try:
        outcome = subprocess.run([app / "unins000.exe", *SILENT, f"/LOG={run / 'blocked-uninstall.log'}"],
                                 timeout=45, creationflags=FLAGS)
        assert outcome.returncode != 0, "Uninstall continued while player could not exit"
        assert registration(test_id) and (app / MANAGER).is_file() and process.poll() is None
    finally:
        player.resume()


def verify_collision(run, installer, qa_id, report, wallpapers):
    app = run / "collision"
    pack_id = "afterimage"
    pack = app / "wallpapers-native" / pack_id
    revision = "v-" + "1" * 40
    resources = pack / ".action75-versions" / revision
    for name in inventory()["wallpapers"][pack_id]:
        target = resources / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(wallpapers / pack_id / name, target)
    manifest = json.loads((resources / "manifest.json").read_bytes())
    manifest["name"] = "QA import before this wallpaper ID was bundled"
    (resources / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    for name in ("entry", "settings", "preview"):
        manifest[name] = (resources.relative_to(pack) / manifest[name]).as_posix()
    manifest["_action75Revision"] = revision
    (pack / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    def contents():
        return {file.relative_to(pack).as_posix(): digest(file) for file in pack.rglob("*") if file.is_file()}

    expected = contents()
    before = production_snapshot()
    test_id = "Action75WallpaperQA-" + qa_id
    uninstaller = app / "unins000.exe"
    assert registration(test_id) is None and get_qa_startup(qa_id) is None
    try:
        subprocess.run([installer, *SILENT, f"/DIR={app}", f"/LOG={run / 'collision-install.log'}"],
                       check=True, timeout=90, creationflags=FLAGS)
        assert contents() == expected
        assert (app / "data/preserved-wallpapers.txt").read_text(encoding="ascii") == pack_id
        assert (app / "wallpapers-native/night-fireworks/manifest.json").is_file()
        with headless_playback(run, app, pack_id) as (_, status):
            assert status.get("packId") == pack_id
        portable = run / "portable"
        target = portable / MANAGER
        target.parent.mkdir(parents=True)
        shutil.copy2(app / MANAGER, target)
        set_qa_startup(qa_id, portable)
        portable_entry = get_qa_startup(qa_id)
        subprocess.run([uninstaller, *SILENT, f"/LOG={run / 'collision-uninstall.log'}"],
                       check=True, timeout=60, creationflags=FLAGS)
        wait_for(lambda: registration(test_id) is None, "uninstall collision fixture")
        assert get_qa_startup(qa_id) == portable_entry
        assert contents() == expected
        report["preservedCollision"] = {"id": pack_id, "files": expected}
        report["checks"].append("Same-ID imported wallpaper plays and survives install and uninstall")
        report["checks"].append("Uninstall preserves another copy's startup entry")
    finally:
        if uninstaller.is_file() and registration(test_id) is not None:
            subprocess.run([uninstaller, *SILENT, f"/LOG={run / 'collision-cleanup.log'}"],
                           timeout=60, creationflags=FLAGS)
        remove_qa_startup(qa_id)
        assert production_snapshot() == before, "Collision QA affected production"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--wallpapers", type=Path, default=ROOT / "wallpapers-native")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "artifacts")
    parser.add_argument("--install-only", action="store_true", help="Skip wallpaper import and performance checks.")
    parser.add_argument("--exercise-blocked-uninstall", action="store_true",
                        help="Use psutil to simulate an unresponsive QA player during uninstall.")
    parser.add_argument("--skip-performance", action="store_true", help="Load every wallpaper without the 60 FPS stress test.")
    parser.add_argument("--import-click", type=float, nargs=2, metavar=("X", "Y"),
                        help="Override the import button click in logical pixels.")
    options = parser.parse_args()
    options.output_dir.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="release-qa-", dir=options.output_dir.resolve()))
    qa_id = uuid.uuid4().hex
    report = {"qaId": qa_id, "checks": [], "performance": []}
    try:
        installer = build_installer(options.compiler, options.receipt, run, qa_id=qa_id,
                                    wallpapers=options.wallpapers)
        report["installer"] = str(installer)
        verify_install(run, installer, qa_id, report, options)
        verify_collision(run, installer, qa_id, report, options.wallpapers)
        report["passed"] = True
    finally:
        path = run / "verification.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(path, flush=True)


if __name__ == "__main__":
    main()
