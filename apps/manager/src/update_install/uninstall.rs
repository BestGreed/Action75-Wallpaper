use super::{Event, manager, registered_id};
use crate::{game_compat, player, runtime_identity::installation_endpoint, startup};
use std::{
    ffi::OsStr,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use windows::{
    Win32::{
        Foundation::{BOOL, ERROR_FILE_NOT_FOUND, HWND, LPARAM, WPARAM},
        System::{
            ProcessStatus::EnumProcesses,
            Threading::{
                EVENT_MODIFY_STATE, OpenEventW, OpenProcess, PROCESS_NAME_WIN32,
                PROCESS_QUERY_LIMITED_INFORMATION, QueryFullProcessImageNameW, SetEvent,
            },
        },
        UI::WindowsAndMessaging::{EnumWindows, GetPropW, PostMessageW, RegisterWindowMessageW},
    },
    core::{HSTRING, PWSTR, w},
};

pub(super) fn run(root: &Path, qa_id: Option<&OsStr>) -> Result<(), String> {
    let qa_id = qa_id
        .map(|id| {
            id.to_str()
                .filter(|id| id.len() == 32 && id.bytes().all(|byte| byte.is_ascii_hexdigit()))
                .ok_or("卸载测试标识无效")
        })
        .transpose()?;
    let registration = qa_id.map_or_else(
        || "{81CCDD80-9A70-4694-94EB-AC6D96651B70}".to_owned(),
        |id| format!("Action75WallpaperQA-{id}"),
    );
    let root = registered_id(root, &registration)?;
    let _helper = game_compat::win::validate_process(std::process::id(), &manager(&root))?;
    let endpoint = installation_endpoint(&root)?;
    request_panel_exit(&endpoint)?;
    let deadline = Instant::now() + Duration::from_secs(12);
    wait_for_exit(&[manager(&root)], deadline)?;
    game_compat::stop_player(&root)?;
    wait_for_exit(&[manager(&root), player(&root)], deadline)?;
    startup::remove_install_entry(&root, qa_id)
}

fn request_panel_exit(endpoint: &str) -> Result<(), String> {
    let name = HSTRING::from(format!(r"Local\{endpoint}-ManagerQuit"));
    let event = match unsafe { OpenEventW(EVENT_MODIFY_STATE, false, &name) } {
        Ok(handle) => Event(handle),
        Err(error) if error.code() == ERROR_FILE_NOT_FOUND.to_hresult() => return Ok(()),
        Err(error) => return Err(error.to_string()),
    };
    unsafe { SetEvent(event.0) }.map_err(|error| error.to_string())?;

    struct Notice(HSTRING, u32);
    unsafe extern "system" fn notify(window: HWND, data: LPARAM) -> BOOL {
        let notice = unsafe { &*(data.0 as *const Notice) };
        unsafe {
            if !GetPropW(window, &notice.0).is_invalid() {
                let _ = PostMessageW(window, notice.1, WPARAM(0), LPARAM(0));
            }
        }
        true.into()
    }
    let message = unsafe { RegisterWindowMessageW(w!("Action75.QuitSession")) };
    let notice = Notice(HSTRING::from(format!("{endpoint}-ManagerWindow")), message);
    if message != 0 {
        unsafe { EnumWindows(Some(notify), LPARAM(&notice as *const Notice as isize)) }
            .map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn wait_for_exit(paths: &[PathBuf], deadline: Instant) -> Result<(), String> {
    let paths: Vec<_> = paths
        .iter()
        .map(|path| path.canonicalize().map_err(|error| error.to_string()))
        .collect::<Result<_, _>>()?;
    while session_running(&paths)? {
        if Instant::now() >= deadline {
            return Err("当前安装目录的程序未及时退出".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Ok(())
}

fn session_running(paths: &[PathBuf]) -> Result<bool, String> {
    let mut pids = vec![0u32; 1024];
    let mut bytes = 0;
    loop {
        let capacity = (size_of::<u32>() * pids.len()) as u32;
        unsafe { EnumProcesses(pids.as_mut_ptr(), capacity, &mut bytes) }
            .map_err(|error| error.to_string())?;
        if bytes < capacity {
            break;
        }
        pids.resize(pids.len() * 2, 0);
    }
    let mut name = vec![0u16; 32768];
    for &pid in &pids[..bytes as usize / size_of::<u32>()] {
        if pid == std::process::id() {
            continue;
        }
        let Ok(handle) = (unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) })
        else {
            continue;
        };
        let process = Event(handle);
        let mut len = name.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(
                process.0,
                PROCESS_NAME_WIN32,
                PWSTR(name.as_mut_ptr()),
                &mut len,
            )
        }
        .is_ok()
        {
            let image = PathBuf::from(String::from_utf16_lossy(&name[..len as usize]));
            if paths
                .iter()
                .any(|path| path.file_name() == image.file_name())
                && image
                    .canonicalize()
                    .is_ok_and(|image| paths.contains(&image))
            {
                return Ok(true);
            }
        }
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        os::windows::process::CommandExt,
        process::{Child, Command, Stdio},
    };

    #[test]
    #[ignore = "child-process fixture; started only by uninstall detection tests"]
    fn uninstall_case_child_fixture() {
        std::thread::sleep(Duration::from_secs(30));
    }

    #[test]
    fn uninstall_waits_for_the_same_executable_launched_with_different_case() {
        struct TestChild(Child);
        impl Drop for TestChild {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }

        let scratch = tempfile::tempdir().unwrap();
        let executable = scratch.path().join("Action75UninstallCaseFixture.exe");
        std::fs::copy(std::env::current_exe().unwrap(), &executable).unwrap();
        let alias = executable.with_file_name("ACTION75UNINSTALLCASEFIXTURE.EXE");
        let _child = TestChild(
            Command::new(alias)
                .args(["--ignored", "--nocapture", "uninstall_case_child_fixture"])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .creation_flags(0x08000000)
                .spawn()
                .unwrap(),
        );
        assert!(session_running(&[executable.canonicalize().unwrap()]).unwrap());
    }
}
