//! Coordinate an update with the registered, per-user Inno installation.
mod restore;
mod uninstall;

use crate::{command, game_compat::win, player, storage};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    ffi::OsString,
    fs,
    io::Write,
    os::windows::ffi::OsStringExt,
    path::{Path, PathBuf},
    process::Command,
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};
use windows::{
    Win32::{
        Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
        System::{
            Registry::{HKEY_CURRENT_USER, RRF_RT_REG_SZ, RRF_SUBKEY_WOW6464KEY, RegGetValueW},
            Threading::{CreateEventW, WaitForSingleObject},
        },
    },
    core::{HSTRING, w},
};

fn manager(root: &Path) -> PathBuf {
    root.join("dist/Action75Manager/Action75Manager.exe")
}

fn registered(root: &Path) -> Result<PathBuf, String> {
    #[cfg(feature = "update-qa")]
    let id = {
        let key = option_env!("ACTION75_QA_INSTALL_ID").unwrap_or_default();
        if key.len() != 32 || !key.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err("此测试版没有专用安装标识".into());
        }
        format!("Action75WallpaperQA-{key}")
    };
    #[cfg(not(feature = "update-qa"))]
    let id = "{81CCDD80-9A70-4694-94EB-AC6D96651B70}";
    registered_id(root, &id)
}

fn registered_id(root: &Path, id: &str) -> Result<PathBuf, String> {
    let key = HSTRING::from(format!(
        r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{id}_is1"
    ));
    let mut data = vec![0u16; 32768];
    let mut bytes = (data.len() * 2) as u32;
    unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            &key,
            w!("InstallLocation"),
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
            None,
            Some(data.as_mut_ptr().cast()),
            Some(&mut bytes),
        )
    }
    .ok()
    .map_err(|_| "仅通过安装程序安装的副本支持应用内更新".to_string())?;
    let end = data.iter().position(|v| *v == 0).unwrap_or(data.len());
    let path = PathBuf::from(OsString::from_wide(&data[..end]));
    if !path.is_absolute()
        || crate::runtime_identity::installation_endpoint(&path)?
            != crate::runtime_identity::installation_endpoint(root)?
    {
        return Err("当前目录与 Windows 登记的安装位置不同".into());
    }
    Ok(path)
}

pub fn installed(root: &Path) -> bool {
    registered(root).is_ok()
}

pub fn check_download_permission() -> Result<(), String> {
    if win::is_elevated()? {
        return Err("安装更新需要以普通权限重新打开控制台。".into());
    }
    Ok(())
}

struct Event(HANDLE);
impl Drop for Event {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

#[derive(Serialize, Deserialize)]
pub(crate) struct Resume {
    pid: u32,
    state: Option<PathBuf>,
    pipe: Option<String>,
    headless: bool,
    hardware: bool,
}

pub(crate) fn status(root: &Path) -> Result<Option<Value>, String> {
    match command(root, json!({"action":"status"})) {
        Ok(v) => Ok(Some(v)),
        Err(e) if e == "播放器未启动" => Ok(None),
        Err(e) => Err(e),
    }
}

impl Resume {
    pub(crate) fn capture(root: &Path, s: &Value) -> Result<Self, String> {
        let pid = s["playerPid"]
            .as_u64()
            .and_then(|id| u32::try_from(id).ok())
            .ok_or("播放器进程身份无效")?;
        win::validate_process(pid, &player(root))?;
        let headless = s["displayGuardMode"] == "headless";
        let state = s["playerStateDirectory"].as_str().map(PathBuf::from);
        let pipe = s["playerExplicitPipe"]
            .as_str()
            .filter(|p| !p.is_empty())
            .map(str::to_owned);
        if headless && (pipe.is_none() || state.is_none()) {
            return Err("原离屏播放器未提供可恢复的启动配置".into());
        }
        Ok(Self {
            pid,
            state,
            pipe,
            headless,
            hardware: s["headlessHardware"] == true,
        })
    }

    pub(crate) fn stop(&self, root: &Path) -> Result<(), String> {
        let Some(current) = status(root)? else {
            return Ok(());
        };
        if current["playerPid"].as_u64() != Some(self.pid.into()) {
            return Err("原播放器已经更换，请重试".into());
        }
        let process = win::validate_process(self.pid, &player(root))?;
        let stop = command(root, json!({"action":"stop","expectedPlayerPid":self.pid}));
        if process.wait(Duration::from_secs(10))? {
            Ok(())
        } else {
            Err(stop.err().unwrap_or_else(|| "播放器未及时退出".into()))
        }
    }

    fn launch(&self, root: &Path) -> Command {
        let mut cmd = Command::new(player(root));
        cmd.current_dir(root).arg("--root").arg(root);
        if let Some(state) = &self.state {
            cmd.arg("--state-dir").arg(state);
        }
        if let Some(pipe) = &self.pipe {
            cmd.arg("--pipe").arg(pipe);
        }
        if self.headless {
            cmd.arg("--headless-test");
        }
        if self.hardware {
            cmd.arg("--headless-hardware");
        }
        cmd
    }

    pub(crate) fn restore(&self, root: &Path) -> Result<(), String> {
        self.restore_checked(root, |_| Ok(()))
    }

    pub(crate) fn restore_checked(
        &self,
        root: &Path,
        check: impl Fn(&Value) -> Result<(), String>,
    ) -> Result<(), String> {
        if let Some(existing) = status(root)? {
            return check(&existing);
        }
        let mut child = self.launch(root).spawn().map_err(|e| e.to_string())?;
        restore::confirm(&mut child, Duration::from_secs(12), |remaining| {
            let status = match crate::player_control::command_timeout(
                root,
                json!({"action":"status"}),
                remaining,
            ) {
                Ok(status) => Some(status),
                Err(error) if error == "播放器未启动" => None,
                Err(error) => return Err(error),
            };
            status
                .map(|s| {
                    check(&s)?;
                    s["playerPid"]
                        .as_u64()
                        .and_then(|pid| u32::try_from(pid).ok())
                        .ok_or_else(|| "播放器进程身份无效".into())
                })
                .transpose()
        })
    }
}

fn record(root: &Path, token: &str) -> Result<PathBuf, String> {
    if token.len() != 32 || !token.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("更新交接标识无效".into());
    }
    Ok(root
        .join("data")
        .join(format!("update-resume-{token}.json")))
}

static PENDING: AtomicBool = AtomicBool::new(false);

/// Returns after Setup has verified its AppId, installation directory and caller.
/// The caller must then close the panel normally so pending settings are flushed.
pub fn start(root: &Path, installer: &Path) -> Result<(), String> {
    if PENDING.swap(true, Ordering::AcqRel) {
        return Err("更新安装已开始".into());
    }
    let result = start_once(root, installer);
    if result.is_err() {
        PENDING.store(false, Ordering::Release);
    }
    result
}

fn start_once(root: &Path, installer: &Path) -> Result<(), String> {
    let root = registered(root)?;
    win::validate_process(std::process::id(), &manager(&root))?;
    check_download_permission()?;
    let resume = status(&root)?
        .map(|s| Resume::capture(&root, &s))
        .transpose()?;
    let token = win::random_token()?;
    let event = Event(
        unsafe {
            CreateEventW(
                None,
                true,
                false,
                &HSTRING::from(format!(r"Local\Action75Update-{token}")),
            )
        }
        .map_err(|e| e.to_string())?,
    );
    let path = record(&root, &token)?;
    let mut saved = fs::File::create_new(&path).map_err(|e| e.to_string())?;
    let result = (|| {
        saved
            .write_all(&serde_json::to_vec(&resume).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        saved.sync_all().map_err(|e| e.to_string())?;
        drop(saved);
        let mut child = Command::new(installer)
            .args([
                "/SP-",
                "/VERYSILENT",
                "/SUPPRESSMSGBOXES",
                "/NORESTART",
                "/NOCLOSEAPPLICATIONS",
                "/NORESTARTAPPLICATIONS",
                "/A75UPDATE=1",
            ])
            .arg(format!("/DIR={}", root.display()))
            .arg(format!("/A75PARENT={}", std::process::id()))
            .arg(format!("/A75READY={token}"))
            .spawn()
            .map_err(|e| format!("无法启动安装程序，{e}"))?;
        let until = Instant::now() + Duration::from_secs(45);
        loop {
            match unsafe { WaitForSingleObject(event.0, 100) } {
                WAIT_OBJECT_0 => return Ok(()),
                WAIT_TIMEOUT => (),
                _ => return Err("等待安装程序确认时出错".into()),
            }
            if let Some(result) = child.try_wait().map_err(|e| e.to_string())? {
                return Err(format!("安装程序未接受当前目录的更新（{result}）"));
            }
            if Instant::now() >= until {
                return Err("安装程序未及时确认更新".into());
            }
        }
    })();
    if result.is_err() {
        let _ = fs::remove_file(path);
    }
    result
}

/// The installer invokes the installed manager before overwriting files and after
/// commit or rollback. No normal GUI/bootstrap side effects run in either helper.
pub fn helper_mode() -> Option<i32> {
    let mut args = std::env::args_os().skip(1);
    let mode = args.next()?.into_string().ok()?;
    if mode == "--uninstall-session" {
        let root = crate::default_root();
        let qa_id = args.next();
        return Some(match uninstall::run(&root, qa_id.as_deref()) {
            Ok(()) => 0,
            Err(error) => {
                storage::log(&root, "ERROR", &format!("无法准备卸载，{error}"));
                1
            }
        });
    }
    if mode != "--update-stop" && mode != "--update-restore" {
        return None;
    }
    let token = args
        .next()
        .and_then(|arg| arg.into_string().ok())
        .unwrap_or_default();
    let failed = args.next().is_some_and(|arg| arg == "--update-failed");
    let root = crate::default_root();
    let result = (|| {
        let root = registered(&root)?;
        win::validate_process(std::process::id(), &manager(&root))?;
        let path = record(&root, &token)?;
        let resume: Option<Resume> =
            serde_json::from_slice(&fs::read(&path).map_err(|e| e.to_string())?)
                .map_err(|e| e.to_string())?;
        if mode == "--update-stop" {
            if let Some(r) = resume {
                r.stop(&root)?;
            } else if status(&root)?.is_some() {
                return Err("更新准备期间播放器已启动".to_string());
            }
            return Ok(());
        }
        let restored = resume.map(|r| r.restore(&root)).transpose();
        if let Err(error) = &restored {
            storage::log(&root, "ERROR", error);
        }
        let mut panel = Command::new(manager(&root));
        if failed {
            storage::log(&root, "ERROR", "更新安装失败，重新打开原会话");
        }
        if failed || restored.is_err() {
            panel.arg("--update-failed");
        }
        panel
            .args(["--updated-panel", "--settings", "--root"])
            .arg(&root)
            .current_dir(&root)
            .spawn()
            .map_err(|e| e.to_string())?;
        fs::remove_file(path).map_err(|e| e.to_string())?;
        restored.map(|_| ())
    })();
    if let Err(e) = result {
        storage::log(&root, "ERROR", &format!("安装交接失败，{e}"));
        Some(1)
    } else {
        Some(0)
    }
}
