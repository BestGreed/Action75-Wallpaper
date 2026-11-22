use super::win;
use crate::update_install::{Resume, status};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    ffi::OsStr,
    mem::size_of,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::Path,
    process::Command,
    thread,
    time::{Duration, Instant},
};
use windows::{
    Win32::{
        Foundation::{HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
        Security::{
            EqualSid, GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenElevation, TokenUser,
        },
        System::Threading::{
            CREATE_NO_WINDOW, CREATE_SUSPENDED, CreateProcessW, DeleteProcThreadAttributeList,
            EXTENDED_STARTUPINFO_PRESENT, GetCurrentProcess, GetExitCodeProcess,
            InitializeProcThreadAttributeList, LPPROC_THREAD_ATTRIBUTE_LIST, OpenProcess,
            OpenProcessToken, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, PROCESS_CREATE_PROCESS,
            PROCESS_INFORMATION, PROCESS_QUERY_LIMITED_INFORMATION, ResumeThread, STARTUPINFOEXW,
            TerminateProcess, UpdateProcThreadAttribute, WaitForSingleObject,
        },
        UI::WindowsAndMessaging::{GetShellWindow, GetWindowThreadProcessId},
    },
    core::{PCWSTR, PWSTR},
};

fn owned(handle: HANDLE) -> OwnedHandle {
    // Only called with handles returned by successful Win32 calls.
    unsafe { OwnedHandle::from_raw_handle(handle.0) }
}

fn raw(handle: &OwnedHandle) -> HANDLE {
    HANDLE(handle.as_raw_handle())
}

fn token(process: HANDLE) -> windows::core::Result<OwnedHandle> {
    let mut handle = HANDLE::default();
    unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut handle)? };
    Ok(owned(handle))
}

fn verify_ordinary(process: HANDLE) -> Result<(), String> {
    let verify = || -> windows::core::Result<bool> {
        let desktop = token(process)?;
        let current = token(unsafe { GetCurrentProcess() })?;
        let mut elevated = 0u32;
        let mut desktop_user = [0usize; 16];
        let mut current_user = [0usize; 16];
        unsafe {
            GetTokenInformation(
                raw(&desktop),
                TokenElevation,
                Some((&mut elevated as *mut u32).cast()),
                size_of::<u32>() as u32,
                &mut 0,
            )?;
            for (handle, user) in [(&desktop, &mut desktop_user), (&current, &mut current_user)] {
                GetTokenInformation(
                    raw(handle),
                    TokenUser,
                    Some(user.as_mut_ptr().cast()),
                    size_of::<[usize; 16]>() as u32,
                    &mut 0,
                )?;
            }
            let sid = |user: &[usize; 16]| (*user.as_ptr().cast::<TOKEN_USER>()).User.Sid;
            Ok(elevated == 0 && EqualSid(sid(&desktop_user), sid(&current_user)).is_ok())
        }
    };
    if !verify().map_err(|e| e.to_string())? {
        return Err("当前桌面没有此账户的普通权限会话".into());
    }
    Ok(())
}

fn desktop_shell() -> Result<OwnedHandle, String> {
    let mut pid = 0;
    unsafe { GetWindowThreadProcessId(GetShellWindow(), Some(&mut pid)) };
    if pid == 0 {
        return Err("无法定位 Windows 桌面会话".into());
    }
    let shell = owned(
        unsafe {
            OpenProcess(
                PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION,
                false,
                pid,
            )
        }
        .map_err(|e| e.to_string())?,
    );
    verify_ordinary(raw(&shell))?;
    Ok(shell)
}

struct Attributes(LPPROC_THREAD_ATTRIBUTE_LIST);
impl Drop for Attributes {
    fn drop(&mut self) {
        unsafe { DeleteProcThreadAttributeList(self.0) };
    }
}

struct Launch {
    process: OwnedHandle,
    thread: OwnedHandle,
}

impl Launch {
    fn suspended(shell: &OwnedHandle, command: &Command) -> Result<Self, String> {
        let wide = |value: &OsStr| value.encode_wide().chain(Some(0)).collect::<Vec<_>>();
        let exe = wide(command.get_program());
        let args = std::iter::once(command.get_program())
            .chain(command.get_args())
            .map(|argument| win::quote(&argument.to_string_lossy()))
            .collect::<Vec<_>>()
            .join(" ");
        let mut line = wide(OsStr::new(&args));
        let directory = command.get_current_dir().map(|path| wide(path.as_os_str()));
        let folder = directory
            .as_ref()
            .map_or(PCWSTR::null(), |path| PCWSTR(path.as_ptr()));
        let mut bytes = 0;
        let mut info = PROCESS_INFORMATION::default();
        let parent = raw(shell);
        unsafe {
            let _ = InitializeProcThreadAttributeList(
                LPPROC_THREAD_ATTRIBUTE_LIST::default(),
                1,
                0,
                &mut bytes,
            );
            let mut storage = vec![0usize; bytes.div_ceil(size_of::<usize>())];
            let list = LPPROC_THREAD_ATTRIBUTE_LIST(storage.as_mut_ptr().cast());
            InitializeProcThreadAttributeList(list, 1, 0, &mut bytes).map_err(|e| e.to_string())?;
            let attributes = Attributes(list);
            UpdateProcThreadAttribute(
                attributes.0,
                0,
                PROC_THREAD_ATTRIBUTE_PARENT_PROCESS as usize,
                Some((&parent as *const HANDLE).cast()),
                size_of::<HANDLE>(),
                None,
                None,
            )
            .map_err(|e| e.to_string())?;
            let mut startup = STARTUPINFOEXW::default();
            startup.StartupInfo.cb = size_of::<STARTUPINFOEXW>() as u32;
            startup.lpAttributeList = attributes.0;
            // Start from the desktop shell's token and verify the child before running any code.
            CreateProcessW(
                PCWSTR(exe.as_ptr()),
                PWSTR(line.as_mut_ptr()),
                None,
                None,
                false,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                None,
                folder,
                &startup.StartupInfo,
                &mut info,
            )
            .map_err(|e| e.to_string())?;
        }
        let child = Self {
            process: owned(info.hProcess),
            thread: owned(info.hThread),
        };
        verify_ordinary(raw(&child.process))?;
        Ok(child)
    }

    fn resume(&self) -> Result<(), String> {
        if unsafe { ResumeThread(raw(&self.thread)) } == u32::MAX {
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(())
    }

    fn wait_ready(&self) -> Result<(), String> {
        let handle = raw(&self.process);
        match unsafe { WaitForSingleObject(handle, 25_000) } {
            WAIT_OBJECT_0 => {
                let mut code = 0;
                unsafe { GetExitCodeProcess(handle, &mut code) }.map_err(|e| e.to_string())?;
                if code == 0 {
                    Ok(())
                } else {
                    Err("普通权限控制台在就绪前退出".into())
                }
            }
            WAIT_TIMEOUT => Err("等待普通权限控制台就绪超时".into()),
            _ => Err(std::io::Error::last_os_error().to_string()),
        }
    }
}

impl Drop for Launch {
    fn drop(&mut self) {
        unsafe {
            let handle = raw(&self.process);
            if WaitForSingleObject(handle, 0) == WAIT_TIMEOUT {
                let _ = TerminateProcess(handle, 1);
                let _ = WaitForSingleObject(handle, 3_000);
            }
        }
    }
}

#[derive(Serialize, Deserialize)]
struct Playback {
    session: Resume,
    expected: Value,
}

fn playback_state(status: &Value) -> Value {
    Value::Object(
        [
            "packId",
            "importedRevision",
            "paused",
            "visible",
            "monitorDevice",
            "deviceGroup",
            "calibrationRevision",
            "exclusiveDisplay",
            "hideKeyboardTaskbar",
            "outputReady",
        ]
        .into_iter()
        .map(|key| (key.to_owned(), status[key].clone()))
        .collect(),
    )
}

pub fn restore_playback(root: &Path, encoded: &str) -> Result<(), String> {
    if win::is_elevated()? {
        return Err("恢复普通权限播放需要以普通权限启动".into());
    }
    let playback: Playback = serde_json::from_str(encoded).map_err(|e| e.to_string())?;
    // The ordinary controller creates its own player so the wallpaper AppContainer
    // starts with the same process security as a normal launch from the desktop.
    playback.session.restore_checked(root, |value| {
        if value["inputElevated"] != false || playback_state(value) != playback.expected {
            return Err("普通权限播放器未能恢复原来的播放状态".into());
        }
        Ok(())
    })
}

/// Start the operational panel from an ordinary process so it can open its
/// own token with the rights needed for the restricted wallpaper trial.
pub fn open_panel(
    root: &Path,
    settings: bool,
    playback: Option<&str>,
    panel_ready: impl Fn(u32) -> bool,
) -> Result<(), String> {
    if win::is_elevated()? {
        return Err("需要通过普通权限桌面打开控制台".into());
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let mut command = Command::new(exe);
    command.current_dir(root).arg("--root").arg(root);
    if settings {
        command.arg("--settings");
    }
    if let Some(playback) = playback {
        command.arg("--ordinary-playback").arg(playback);
    }
    let mut panel = command.spawn().map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if panel.try_wait().map_err(|e| e.to_string())?.is_some() {
            return Err("普通权限控制台在就绪前退出".into());
        }
        if panel_ready(panel.id()) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            let _ = panel.kill();
            let _ = panel.wait();
            return Err("等待普通权限控制台就绪超时".into());
        }
        thread::sleep(Duration::from_millis(100));
    }
}

/// Called after the original panel releases its single-instance lock.
pub fn reopen(root: &Path, settings: bool) -> Result<(), String> {
    let root = std::path::absolute(root).map_err(|e| e.to_string())?;
    let shell = desktop_shell()?;
    let current = status(&root)?;
    let original = current
        .as_ref()
        .filter(|status| status["inputElevated"] == true)
        .map(|status| {
            Resume::capture(&root, status).map(|session| Playback {
                session,
                expected: playback_state(status),
            })
        })
        .transpose()?;
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let mut command = Command::new(exe);
    command
        .current_dir(&root)
        .arg("--ordinary-launcher")
        .arg("--root")
        .arg(&root);
    if settings {
        command.arg("--settings");
    }
    if let Some(original) = &original {
        command
            .arg("--ordinary-playback")
            .arg(serde_json::to_string(original).map_err(|e| e.to_string())?);
    }
    let panel = Launch::suspended(&shell, &command)?;
    let switched = (|| {
        if let Some(original) = &original {
            original.session.stop(&root)?;
        }
        panel.resume()?;
        panel.wait_ready()?;
        Ok(())
    })();
    if let Err(error) = switched {
        drop(panel);
        if let Some(original) = original {
            original
                .session
                .restore(&root)
                .map_err(|e| format!("{error}。恢复管理员播放器失败，{e}"))?;
        }
        return Err(error);
    }
    Ok(())
}
