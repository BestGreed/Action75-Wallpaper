"""Exercise two isolated, real QA installers through the installed manager's update CTA.

Build embeds a private file replay of GitHub's release JSON in both QA binaries.
Run never switches desktops. Every targeted process and registry key belongs to the QA ID.
"""
import argparse
import ctypes
from functools import partial
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
from tempfile import TemporaryDirectory
import time
from threading import Event,Thread
import uuid
import winreg
from zipfile import ZipFile

import psutil
import win32ts
from release_build import build_release, file_version as version, validate_receipt
from release_payload import ROOT, MANAGER, PLAYER, WORKER, DEFAULT_COMPILER, build_installer, release_asset, release_files, version as project_version
from release_qa_gui import (
    W, checked, client_rect, installer_on_desktop, manager_session, per_monitor_coordinates, post,
    wait_for, wait_for_process, window_dpi,
)

spec=importlib.util.spec_from_file_location("release_checks",ROOT/"tools/verify-release.py")
qa=importlib.util.module_from_spec(spec)
spec.loader.exec_module(qa)
def asset_name(version):
    return f"Action75-Wallpaper-Setup-{version}-x64.exe"


def upgrade_versions(previous, current):
    def numeric(version):
        if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
            raise ValueError(f"Invalid version: {version}")
        return tuple(map(int, version.split(".")))

    major, minor, patch = numeric(current)
    if previous is None:
        if patch == 0:
            raise ValueError("Specify --from-version at a major or minor version boundary")
        previous = f"{major}.{minor}.{patch - 1}"
    if numeric(previous) >= numeric(current):
        raise ValueError("The installed version must precede the candidate version")
    return previous, current
def read(path):return json.loads(path.read_bytes())
def write(path,obj):path.write_text(json.dumps(obj,indent=2,ensure_ascii=False),encoding="utf-8")
def require(condition,message):
    if not condition:raise RuntimeError(str(message))
def prepare(work, cargo, previous, current, wallpapers=None):
    work.mkdir(parents=True)
    token = uuid.uuid4().hex
    replay = work / "feed"
    replay.mkdir()
    archive_tools = runpy.run_path(str(ROOT / "tools/package-source.py"))
    source_zip = archive_tools["write_archive"](ROOT, work / "source", wallpapers=wallpapers)
    builds = {}
    # The pinned C++ runtime has long nested source names on Windows.
    with TemporaryDirectory(prefix="a75-src-") as scratch:
        with ZipFile(source_zip) as archive:
            archive.extractall(scratch)
        source = Path(scratch) / f"ACTION75-{project_version()}"
        external = set(release_files(wallpapers=wallpapers)) - set(release_files(include_external=False, wallpapers=wallpapers))
        pinned_runtime = Path("vendor-source/windows-native/EffekseerForCpp1.80.8.zip")
        if (ROOT / pinned_runtime).is_file():
            external.add(pinned_runtime)
        for relative in external:
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(release_asset(ROOT, relative, wallpapers), target)
        for ver in (previous, current):
            for name in ("Cargo.toml", "Cargo.lock"):
                path = Path("apps/manager") / name
                text = (ROOT / path).read_text("utf-8")
                text, count = re.subn(r'(name = "action75-manager"\s+version = ")[^"]+',
                                      lambda match: match[1] + ver, text, count=1)
                require(count == 1, f"Unable to set manager package version in {name}")
                (source / path).write_text(text, encoding="utf-8")
            receipt = build_release(work / f"build-{ver}", root=source, qa_id=token,
                                    update_feed=replay, cargo=cargo)
            _, binaries = validate_receipt(source, receipt, token)
            package = build_installer(DEFAULT_COMPILER, receipt, work, root=source, qa_id=token)
            require(version(package) == ver, "Installer PE version mismatch")
            builds[ver] = dict(installer=package.name, installerSha256=qa.digest(package),
                               managerSha256=qa.digest(binaries[MANAGER]), playerSha256=qa.digest(binaries[PLAYER]),
                               workerSha256=qa.digest(binaries[WORKER]))
    filename=asset_name(current)
    shutil.copy2(work/builds[current]["installer"],replay/filename)
    release=dict(tag_name="v"+current,draft=False,prerelease=False,assets=[dict(
        name=filename,state="uploaded",size=(replay/filename).stat().st_size,
        digest="sha256:"+qa.digest(replay/filename),browser_download_url=
        f"https://github.com/blackwhite-poi/Action75-Wallpaper/releases/download/v{current}/{filename}")])
    write(replay/"release.json",release)
    write(work / "build.json", dict(qaId=token, packages=builds))

def click(window,xy):
    with per_monitor_coordinates():
        dpi=window_dpi(window)/96;box=W.RECT();checked(client_rect(window,ctypes.byref(box)))
        x,y=(round(v*dpi) for v in xy);x=x if x>=0 else box.right+x;y=y if y>=0 else box.bottom+y;require(0<=x<box.right and 0<=y<box.bottom,"CTA outside client")
        point=(y<<16)|x
        for msg,w,l in [(0x86,1,0),(6,1,0),(7,0,0),(0x200,0,point-1),(0x200,0,point),(0x201,1,point),(0x202,0,point)]:
            checked(post(window,msg,w,l))

def own_processes(app,rel):
    expected=(app/rel).resolve()
    result=[]
    # Take one system snapshot per poll. Reading every process image path is slow and
    # races with other monitors using psutil's shared process_iter cache.
    for _,pid,name,_ in win32ts.WTSEnumerateProcesses():
        if name.casefold()!=expected.name.casefold():
            continue
        try:
            proc=psutil.Process(pid)
            if Path(proc.exe()).resolve()==expected:
                result.append(proc)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return result

def portable_snapshot(portable):
    if not portable:return {}
    players=[]
    for proc in own_processes(portable,PLAYER):
        try:
            args=proc.cmdline()
        except psutil.NoSuchProcess:
            continue
        # Standalone benchmarks may reuse the installed executable with a separate
        # --root. Protect the user's session, whose data is actually under portable.
        if "--root" not in args or Path(args[args.index("--root")+1]).resolve()==portable.resolve():
            players.append(proc.pid)
    return dict(files={str(rel):qa.digest(portable/rel) for rel in (MANAGER,PLAYER,WORKER,Path("data/app-settings.json"),
                Path("data/fm-calibration.json")) if (portable/rel).is_file()},
                players=sorted(players))

def exercise(work,position,portable,keep,importer,simulate_lock,playing):
    config=read(work/"build.json");token=config["qaId"];packs=config["packages"]
    previous,current=packs
    upgrade_versions(previous,current)
    run=work/("trial-"+uuid.uuid4().hex[:12]);run.mkdir();app=run/"installed";data=app/"data"
    reg="Action75WallpaperQA-"+token
    require(qa.registration(reg) is None,"QA installation identity already in use")
    require(qa.digest(work/packs[previous]["installer"])==packs[previous]["installerSha256"],"Baseline digest mismatch")
    protected=qa.production_snapshot(); portable_before=portable_snapshot(portable)
    report=dict(qaId=token,root=str(app),packageDigests=packs,sourceVersion=previous,targetVersion=current)
    path=run/"result.json";write(path,report)
    print("QA evidence",run,flush=True)
    monitor_stop=Event();monitor=None;samples=[];lock=None
    pipe="Action75UpdateQA-"+uuid.uuid4().hex
    call=partial(qa.pipe_request,pipe)
    try:
        subprocess.run([work/packs[previous]["installer"],*qa.SILENT,"/NOICONS",f"/DIR={app}",f"/LOG={run/'baseline-install.log'}"],
                       check=True,timeout=90,creationflags=qa.FLAGS)
        require(version(app/MANAGER)==previous and qa.registration(reg)["DisplayVersion"][0]==previous,"Wrong installed baseline")
        require(qa.digest(app/PLAYER)==packs[previous]["playerSha256"] and qa.digest(app/WORKER)==packs[previous]["workerSha256"],"Wrong baseline native binaries")
        data.mkdir(exist_ok=True)
        prefs={"setupComplete":True,"panelOpen":False,"theme":"graphite","gpuPreference":"integrated",
               "wallpaperDir":"","logDir":""}
        write(data/"app-settings.json",prefs)
        calibration=read(app/"assets/defaults/fm-calibration.json")
        calibration["points"]["KeyA"]["x"]+=.001
        write(data/"fm-calibration.json",calibration)
        (data/"player-pipe.txt").write_text(pipe,encoding="ascii")
        # Use a real versioned imported directory, not an asset that the new installer ships.
        src=qa.LEGACY_ABI1_PACK
        pack_id=read(src/"manifest.json")["id"];imported=app/"wallpapers-native"/pack_id
        state=run/"player-data";state.mkdir()
        shutil.copy2(data/"fm-calibration.json",state/"fm-calibration.json")
        flags=[app/PLAYER,"--root",app,"--headless-test","--state-dir",state,"--pipe",pipe]
        worker=subprocess.Popen(flags,creationflags=qa.FLAGS)
        status=call("status");require(status["playerPid"]==worker.pid and status["headless"],status)
        if importer:
            subprocess.run([importer,app,src],check=True,creationflags=qa.FLAGS)
        else:
            qa.import_wallpaper(app/MANAGER,app,src,imported/"manifest.json",run/"import.png")
        call("listPacks")
        call("selectPack",id=pack_id)
        call("setPackSettings",id=pack_id,values={"amber_lights":True,"runnerBrightness":.65})
        values=call("getPackSettings",id=pack_id)["values"]
        require(values.get("amber_lights") is True and abs(values.get("runnerBrightness",0)-.65)<1e-5, "Test parameter overrides were not applied")
        if not playing:call("pause")
        preserved=[data/"fm-calibration.json",state/"fm-calibration.json",state/"wallpaper-settings"/(pack_id+".json"),*sorted(imported.rglob("*"))]
        before={str(p.relative_to(run)):qa.digest(p) for p in preserved if p.is_file()}
        capture=run/"old-client.png"
        with manager_session(app/MANAGER,app,"--screenshot-size","1380","880","--screenshot",str(capture)) as (old,windows):
            window=wait_for(lambda:windows("Action75 Wallpaper"),"private desktop manager",30)[0]
            wait_for(capture.is_file,"client screenshot",40)
            log=app/"logs/manager.log"
            def logged(message):
                return log.exists() and message in log.read_text(encoding="utf8",errors="replace")
            wait_for(lambda:logged("发现新版本 "+current),"automatic update check",20)
            # Corrupt a private test asset without changing length. Restore the byte in
            # finally so another full attempt can reuse the authentic candidate.
            asset=work/"feed"/asset_name(current)
            with asset.open("r+b") as f:
                first=f.read(1)
                f.seek(0);f.write(bytes([first[0]^1]));f.flush();os.fsync(f.fileno())
            try:
                click(window,position)
                wait_for(lambda: logged("SHA-256 校验失败"),"rejected corrupted installer",18)
                require(call("status")["playerPid"]==worker.pid and qa.registration(reg)["DisplayVersion"][0]==previous,
                        "Rejected download changed the running installation")
                report["rejectedBadDigest"]=True
            finally:
                with asset.open("r+b") as f:f.write(first)
            require(qa.digest(asset)==packs[current]["installerSha256"],"Replay asset not restored")
            report["oldManagerPid"]=old.pid;report["oldPlayerPid"]=worker.pid;write(path,report)
            if simulate_lock:
                # Deny replacement of the QA manager to exercise actual Inno rollback.
                # Other processes may still read and execute the old binary for recovery.
                import win32file
                lock=win32file.CreateFile(str(app/MANAGER),win32file.GENERIC_READ,win32file.FILE_SHARE_READ,
                    None,win32file.OPEN_EXISTING,win32file.FILE_ATTRIBUTE_NORMAL,None)
            def sample():
                while not monitor_stop.is_set():
                    samples.append([p.pid for p in own_processes(app,PLAYER)])
                    monitor_stop.wait(.04)
            monitor=Thread(target=sample,daemon=True);monitor.start()
            click(window,position)
            def upgraded():
                # Inno replaces the uninstall key during handoff. Read one value;
                # treat a removed key as an intermediate state on the next poll.
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, f"{qa.UNINSTALL}\\{reg}_is1") as key:
                        registered=winreg.QueryValueEx(key,"DisplayVersion")[0]
                except OSError as exc:
                    if exc.winerror in (2,1018):
                        return False
                    raise
                return registered==current and (app/MANAGER).is_file() and version(app/MANAGER)==current
            if not simulate_lock:
                wait_for(upgraded,f"installed {current} after the one click",120)
            wait_for(lambda:len(own_processes(app,MANAGER))==1 and own_processes(app,MANAGER)[0].pid!=old.pid,
                     "single restarted client",90)
            def restored():
                procs=own_processes(app,PLAYER)
                return len(procs)==1 and procs[0].pid!=worker.pid
            wait_for(restored,"single restarted player",40)
            status=call("status")
            require(status["headless"] and status["paused"]==(not playing) and status["packId"]==pack_id and status["playerPid"]!=worker.pid,status)
            require(call("getPackSettings",id=pack_id)["values"]==values,"Parameter values changed")
            require(read(data/"app-settings.json").items()>=prefs.items(),"App preferences changed")
            require(all(qa.digest(run/k)==v for k,v in before.items()),"Calibration, package or user profile changed")
            expected=previous if simulate_lock else current
            require(qa.digest(app/MANAGER)==packs[expected]['managerSha256'],"Unexpected installed manager digest")
            require(qa.digest(app/PLAYER)==packs[expected]["playerSha256"] and qa.digest(app/WORKER)==packs[expected]["workerSha256"],"Wrong restored native binaries")
            require(qa.registration(reg)['DisplayVersion'][0]==expected,"Unexpected registry version")
            report["rollbackAfterFileLock"]=simulate_lock
            if playing:
                frames=status["framesRendered"]
                call("testPluginEvent",type=1,code="ArrowRight",x=.8,y=.7)
                wait_for(lambda:call("status")["framesRendered"]>frames+2,"motion after update",5)
                call("testPluginEvent",type=2,code="ArrowRight",x=.8,y=.7)
                report["framesAfterPress"]=call("status")["framesRendered"]-frames
            report.update(newManagerPid=own_processes(app,MANAGER)[0].pid,newPlayerPid=status['playerPid'],
                newPlayerCount=len(own_processes(app,PLAYER)),paused=status["paused"],headless=status["headless"],selected=pack_id,preserved=before,values=values,
                version=version(app/MANAGER),installedLocation=qa.registration(reg)["InstallLocation"][0])
        report["productionUntouched"]=qa.production_snapshot()==protected
        report["protectedInstallBefore"]=portable_before
        report["protectedInstallAfter"]=portable_snapshot(portable)
        require(report["productionUntouched"] and report["protectedInstallAfter"]==portable_before,"Portable or production changed")
        report["completed"]=True
    finally:
        if lock:lock.Close()
        if monitor:
            monitor_stop.set();monitor.join(5)
            report["maxPlayersSampled"]=max(map(len,samples),default=0)
            report["playerPolls"]=len(samples)
            if report["maxPlayersSampled"]!=1:report["completed"]=False
        for rel in (MANAGER,PLAYER,WORKER):
            for proc in own_processes(app,rel):
                try:proc.terminate();proc.wait(5)
                except psutil.NoSuchProcess:pass
        if not keep and (app/"unins000.exe").exists():
            subprocess.run([app/"unins000.exe",*qa.SILENT,f"/LOG={run/'uninstall.log'}"],timeout=60,creationflags=qa.FLAGS)
        report["portableUntouched"]=portable_snapshot(portable)==portable_before
        write(path,report)
    require(report.get("maxPlayersSampled")==1,"Overlapping QA players in handoff")
    print(path,flush=True)


RELEASE_REPO = "blackwhite-poi/Action75-Wallpaper"


def download_published_baseline(work, candidate):
    # The Windows curl build trusts the same certificate store as Setup.
    curl = [Path(os.environ["SystemRoot"]) / "System32/curl.exe", "-q", "--fail", "--silent",
            "--show-error", "--proto", "=https", "--proto-redir", "=https",
            "--user-agent", "ACTION75 upgrade verification"]
    config = ""
    if token := os.environ.get("GH_TOKEN"):
        config = f'header = "Authorization: Bearer {token}"\n'
    api = f"https://api.github.com/repos/{RELEASE_REPO}/releases?per_page=50"
    releases = json.loads(subprocess.check_output(
        [*curl, "--max-time", "25", "--header", "Accept: application/vnd.github+json",
         "--config", "-", api], input=config.encode("ascii"), creationflags=qa.FLAGS))
    options = []
    for release in releases:
        tag = release["tag_name"]
        if release["draft"] or release["prerelease"] or not tag.startswith("v"):
            continue
        previous = tag[1:]
        try:
            upgrade_versions(previous, candidate)
        except ValueError:
            continue
        name = asset_name(previous)
        for asset in release["assets"]:
            url = f"https://github.com/{RELEASE_REPO}/releases/download/{tag}/{name}"
            if (asset["name"] == name and asset["state"] == "uploaded"
                    and asset["browser_download_url"] == url
                    and 0 < asset["size"] < 256 * 1024 * 1024
                    and re.fullmatch(r"sha256:[a-f0-9]{64}", asset.get("digest") or "")):
                options.append((tuple(map(int, previous.split("."))), tag, asset))
    if not options:
        raise RuntimeError(f"No published installer with a checksum precedes {candidate}")
    _, tag, asset = max(options, key=lambda option: option[0])
    path = work / asset["name"]
    subprocess.run([*curl, "--location", "--max-time", "60", "--max-filesize", str(asset["size"]),
                    "--output", path, asset["browser_download_url"]], check=True, creationflags=qa.FLAGS)
    require(path.stat().st_size == asset["size"], "Published installer size mismatch")
    require(qa.digest(path) == asset["digest"].removeprefix("sha256:"), "Published installer SHA-256 mismatch")
    require(version(path) == tag[1:], "Published installer PE version differs from the release tag")
    metadata = {"version": tag[1:], "url": asset["browser_download_url"],
                "bytes": asset["size"], "sha256": qa.digest(path)}
    return path, metadata


def verify_installer_handoff(run, baseline, metadata, candidate, receipt, rollback, install_id):
    import win32event
    import win32file

    run.mkdir()
    app = run / "installed"
    data = app / "data"
    uninstaller = app / "unins000.exe"
    registered = partial(qa.registration, install_id)
    require(registered() is None, "The tested installation identity is already in use")
    report = {"baseline": metadata, "candidate": receipt, "rollback": rollback, "passed": False}
    try:
        subprocess.run([baseline, *qa.SILENT, "/SP-", "/NOICONS", f"/DIR={app}",
                        f"/LOG={run / 'baseline-install.log'}"], check=True, timeout=120, creationflags=qa.FLAGS)
        previous = metadata["version"]
        registration = registered()
        require(registration["DisplayVersion"][0] == previous
                and Path(registration["InstallLocation"][0]).resolve() == app.resolve()
                and version(app / MANAGER) == previous, "Wrong real baseline installation")
        originals = {relative.as_posix(): qa.digest(app / relative) for relative in (MANAGER, PLAYER, WORKER)}
        data.mkdir(exist_ok=True)
        prefs = {"setupComplete": True, "panelOpen": False, "wallpaperDir": "", "logDir": ""}
        write(data / "app-settings.json", prefs)
        calibration = data / "fm-calibration.json"
        shutil.copy2(app / "assets/defaults/fm-calibration.json", calibration)
        preserved = qa.digest(calibration)
        with qa.headless_playback(run, app, "afterimage", paused=rollback) as (player, status):
            pipe = status["playerExplicitPipe"]
            state = status["playerStateDirectory"]
            token = uuid.uuid4().hex
            event = win32event.CreateEvent(None, True, False, f"Local\\Action75Update-{token}")
            resume = data / f"update-resume-{token}.json"
            write(resume, dict(pid=player.pid, state=state, pipe=pipe, headless=True, hardware=False))

            lock = None
            try:
                with manager_session(app / MANAGER, app) as (panel, windows):
                    window = wait_for(lambda: windows("Action75 Wallpaper"), "released controller", 45)[0]
                    if rollback:
                        lock = win32file.CreateFile(str(app / MANAGER), win32file.GENERIC_READ,
                                                    win32file.FILE_SHARE_READ, None, win32file.OPEN_EXISTING, 0, None)
                    command = [candidate, *qa.SILENT, "/SP-", "/NORESTARTAPPLICATIONS", "/A75UPDATE=1",
                               f"/DIR={app}", f"/A75READY={token}", f"/A75PARENT={panel.pid}",
                               f"/LOG={run / 'upgrade.log'}"]
                    with installer_on_desktop(command, app, panel) as installer:
                        require(win32event.WaitForSingleObject(event, 45000) == 0,
                                "Candidate installer did not acknowledge the released controller")
                        checked(post(window, 0x0010, 0, 0))  # WM_CLOSE of the isolated test panel
                        upgrade_code = wait_for_process(installer, 180)
                    report["installerExitCode"] = upgrade_code
                    require((upgrade_code != 0) == rollback, "Unexpected success or failure from the candidate installer")
                    require(player.wait(timeout=10) == 0, "Original player did not exit normally")
                    qa.assert_process_exited(status["nativeWorkerPid"])
                    panel_processes = wait_for(lambda: own_processes(app, MANAGER), "restored controller", 45)
                    restored = qa.pipe_request(pipe, "status")
                    player_processes = own_processes(app, PLAYER)
                    require(len(panel_processes) == len(player_processes) == 1
                            and panel_processes[0].pid != panel.pid
                            and restored["playerPid"] == player_processes[0].pid != player.pid,
                            "Expected exactly one restored player and controller")
                    require(restored["headless"] and restored["paused"] == rollback
                            and restored["packId"] == status["packId"] and restored["nativeWorkerPid"] > 0,
                            "Playback session was not restored")
                    expected = previous if rollback else receipt["version"]
                    require(registered()["DisplayVersion"][0] == expected
                            and version(app / MANAGER) == expected, "Unexpected installed version after handoff")
                    digests = {relative.as_posix(): qa.digest(app / relative) for relative in (MANAGER, PLAYER, WORKER)}
                    require(digests == (originals if rollback else receipt["sha256"]), "Wrong executable bytes")
                    require(qa.digest(calibration) == preserved and read(data / "app-settings.json").items() >= prefs.items(),
                            "Preferences or calibration changed during upgrade")
                    require(not resume.exists(), "Upgrade session record was not consumed")
                    report.update(passed=True, version=expected, binaries=digests,
                                  oldPlayerPid=player.pid, newPlayerPid=restored["playerPid"],
                                  oldPanelPid=panel.pid, newPanelPid=panel_processes[0].pid)
            finally:
                if lock:
                    lock.Close()
                event.Close()
    finally:
        try:
            # Also works for the unmodified older uninstaller.
            for relative in (MANAGER, PLAYER):
                for process in own_processes(app, relative):
                    try:
                        process.terminate()
                        process.wait(10)
                    except psutil.NoSuchProcess:
                        pass
            if uninstaller.is_file() and registered() is not None:
                subprocess.run([uninstaller, *qa.SILENT, f"/LOG={run / 'uninstall.log'}"],
                               check=True, timeout=60, creationflags=qa.FLAGS)
            wait_for(lambda: registered() is None, "isolated installation registration to clear")
        finally:
            write(run / "result.json", report)


def exercise_published(work, candidate, receipt_path):
    require(os.environ.get("GITHUB_ACTIONS") == "true"
            and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted",
            "The public baseline may only be installed on a disposable GitHub-hosted runner")
    installed, autorun, links = qa.production_snapshot()
    require(installed is None and autorun is None and not any(links),
            "Existing ACTION75 installation signals found; refusing the published-upgrade test")
    receipt, _ = validate_receipt(ROOT, receipt_path)
    require(version(candidate) == receipt["version"], "Candidate does not match the current receipt")
    work.mkdir(parents=True)
    baseline, metadata = download_published_baseline(work, receipt["version"])
    write(work / "baseline.json", metadata)
    for rollback in (False, True):
        verify_installer_handoff(work / ("rollback" if rollback else "upgrade"),
                                 baseline, metadata, candidate, receipt, rollback, qa.PRODUCTION_ID)


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode",choices=["build","run","published"])
    p.add_argument("--work",type=Path,required=True)
    p.add_argument("--cargo")
    p.add_argument("--wallpapers",type=Path,help="Verified staged library for the two QA installer builds")
    p.add_argument("--from-version",help="Installed QA version; defaults to the previous patch release")
    p.add_argument("--to-version",default=project_version(),help="Candidate QA version; defaults to the current manager version")
    p.add_argument("--candidate",type=Path,help="Production candidate installer for disposable CI")
    p.add_argument("--receipt",type=Path,help="Build receipt for the production candidate")
    p.add_argument("--cta",type=float,nargs=2,default=(284,-18),help="Logical pixels; negative coordinates count from the far edge")
    p.add_argument("--portable",type=Path)
    p.add_argument("--import-tool",type=Path,help="Optional compiled production importer for seeding the test fixture")
    p.add_argument("--keep",action="store_true")
    p.add_argument("--playing",action="store_true",help="Upgrade during playback and check input after restore")
    p.add_argument("--simulate-lock",action="store_true",help="Deny replacement of the QA manager to exercise installer rollback")
    args=p.parse_args()
    if args.mode=="build":
        previous,current=upgrade_versions(args.from_version,args.to_version)
        prepare(args.work.resolve(),args.cargo,previous,current,args.wallpapers)
    elif args.mode == "published":
        if not args.candidate or not args.receipt:
            p.error("published mode requires --candidate and --receipt")
        exercise_published(args.work.resolve(), args.candidate.resolve(strict=True), args.receipt)
    else:
        exercise(args.work.resolve(),args.cta,args.portable,args.keep,args.import_tool,args.simulate_lock,args.playing)
