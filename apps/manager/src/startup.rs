mod scheduled_task;

use std::{
    ffi::OsString,
    io,
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::Path,
};
use windows::{
    Win32::{
        Foundation::{
            CloseHandle, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS,
            ERROR_UNSUPPORTED_TYPE, HANDLE, HLOCAL, LocalFree, WAIT_ABANDONED_0, WAIT_OBJECT_0,
        },
        System::{
            Registry::{
                HKEY_CURRENT_USER, REG_SZ, RRF_RT_REG_SZ, RegDeleteKeyValueW, RegGetValueW,
                RegSetKeyValueW,
            },
            Threading::{CreateMutexW, ReleaseMutex, WaitForSingleObject},
        },
        UI::Shell::CommandLineToArgvW,
    },
    core::{HSTRING, PCWSTR, w},
};

// The tray reads this key and sends all changes through the controller.
const RUN_KEY: PCWSTR = w!("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const ENTRY: PCWSTR = w!("ACTION75");

#[derive(Clone, Copy)]
pub enum StartupAction {
    Inspect,
    Set(bool),
    Toggle,
    MigrateTrial,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LegacyStartup {
    None,
    Active,
    Unidentified,
    Unavailable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StartupState {
    pub enabled: bool,
    pub legacy: LegacyStartup,
}

pub fn update(root: &Path, action: StartupAction) -> Result<StartupState, String> {
    let _write = match action {
        StartupAction::Inspect => None,
        _ => Some(StartupWrite::acquire()?),
    };
    apply(&mut WindowsStartup { root }, action)
}

trait StartupBackend {
    fn enabled(&self) -> Result<bool, String>;
    fn legacy(&self) -> LegacyStartup;
    fn set_enabled(&mut self, enabled: bool) -> Result<(), String>;
    fn set_legacy(&mut self, enabled: bool) -> Result<(), String>;
}

fn apply(backend: &mut impl StartupBackend, action: StartupAction) -> Result<StartupState, String> {
    let enabled = backend
        .enabled()
        .map_err(|error| format!("无法读取开机运行，{error}"))?;
    let legacy = backend.legacy();
    let target = match action {
        StartupAction::Inspect => return Ok(StartupState { enabled, legacy }),
        StartupAction::Set(target) => target,
        StartupAction::Toggle => !enabled,
        StartupAction::MigrateTrial => {
            if legacy != LegacyStartup::Active {
                return Err("旧版登录任务已变化，请检查设置后再迁移".into());
            }
            backend.set_legacy(false).map_err(|error| {
                format!(
                    "无法停用旧试用版登录任务，{error}。可在 Windows 任务计划程序停用 {} 后，再开启新版开机运行。",
                    scheduled_task::TASK_NAME
                )
            })?;
            if let Err(error) = backend.set_enabled(true) {
                return Err(match backend.set_legacy(true) {
                    Ok(()) => format!("无法开启新版开机运行，{error}。已恢复旧试用版登录任务。"),
                    Err(restore) => format!(
                        "无法开启新版开机运行，{error}。恢复旧试用版登录任务也失败，{restore}。请核对 Windows 任务计划程序中的 {}。",
                        scheduled_task::TASK_NAME
                    ),
                });
            }
            return Ok(StartupState {
                enabled: true,
                legacy: LegacyStartup::None,
            });
        }
    };
    if target && legacy == LegacyStartup::Active {
        return Err("旧试用版登录任务已启用。请在新版「设置」的「启动」中迁移。".into());
    }
    backend
        .set_enabled(target)
        .map_err(|error| format!("无法修改开机运行，{error}"))?;
    Ok(StartupState {
        enabled: target,
        legacy,
    })
}

struct StartupWrite(HANDLE);

impl StartupWrite {
    fn acquire() -> Result<Self, String> {
        unsafe {
            let mutex = CreateMutexW(None, false, w!("Local\\Action75Wallpaper-Startup"))
                .map_err(|error| error.to_string())?;
            if matches!(
                WaitForSingleObject(mutex, 3000),
                WAIT_OBJECT_0 | WAIT_ABANDONED_0
            ) {
                Ok(Self(mutex))
            } else {
                let _ = CloseHandle(mutex);
                Err("另一个窗口正在修改开机运行，请稍后重试".into())
            }
        }
    }
}

impl Drop for StartupWrite {
    fn drop(&mut self) {
        unsafe {
            let _ = ReleaseMutex(self.0);
            let _ = CloseHandle(self.0);
        }
    }
}

struct WindowsStartup<'a> {
    root: &'a Path,
}

impl StartupBackend for WindowsStartup<'_> {
    fn enabled(&self) -> Result<bool, String> {
        owns_entry(self.root, ENTRY)
    }

    fn legacy(&self) -> LegacyStartup {
        scheduled_task::inspect()
    }

    fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
        if enabled {
            let command: Vec<u16> = run_command(self.root)
                .map_err(|error| error.to_string())?
                .encode_wide()
                .chain(Some(0))
                .collect();
            let result = unsafe {
                RegSetKeyValueW(
                    HKEY_CURRENT_USER,
                    RUN_KEY,
                    ENTRY,
                    REG_SZ.0,
                    Some(command.as_ptr().cast()),
                    (command.len() * size_of::<u16>()) as u32,
                )
            };
            if result != ERROR_SUCCESS {
                return Err(io::Error::from_raw_os_error(result.0 as i32).to_string());
            }
        } else {
            remove_owned(self.root, ENTRY)?;
        }
        Ok(())
    }

    fn set_legacy(&mut self, enabled: bool) -> Result<(), String> {
        scheduled_task::set_enabled(enabled)
    }
}

fn owns_entry(root: &Path, entry: PCWSTR) -> Result<bool, String> {
    let mut value = vec![0u16; 32768];
    let mut bytes = (value.len() * size_of::<u16>()) as u32;
    let result = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            RUN_KEY,
            entry,
            RRF_RT_REG_SZ,
            None,
            Some(value.as_mut_ptr().cast()),
            Some(&mut bytes),
        )
    };
    match result {
        ERROR_SUCCESS => {
            let end = value.iter().position(|&c| c == 0).unwrap_or(value.len());
            Ok(command_targets(root, &OsString::from_wide(&value[..end])))
        }
        ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND | ERROR_UNSUPPORTED_TYPE => Ok(false),
        error => Err(io::Error::from_raw_os_error(error.0 as i32).to_string()),
    }
}

fn command_targets(root: &Path, command: &std::ffi::OsStr) -> bool {
    if command.is_empty() {
        return false;
    }
    let command: Vec<_> = command.encode_wide().chain(Some(0)).collect();
    let mut count = 0;
    let argv = unsafe { CommandLineToArgvW(PCWSTR(command.as_ptr()), &mut count) };
    if argv.is_null() {
        return false;
    }
    let executable = (count > 0).then(|| unsafe { OsString::from_wide((*argv).as_wide()) });
    unsafe {
        let _ = LocalFree(HLOCAL(argv.cast()));
    }
    let expected = root.join("dist/Action75Manager/Action75Manager.exe");
    executable.is_some_and(|target| {
        let target = Path::new(&target);
        target.is_absolute()
            && std::fs::canonicalize(target)
                .ok()
                .zip(std::fs::canonicalize(expected).ok())
                .is_some_and(|(target, expected)| target == expected)
    })
}

fn remove_owned(root: &Path, entry: PCWSTR) -> Result<(), String> {
    if owns_entry(root, entry)? {
        let result = unsafe { RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, entry) };
        if !matches!(
            result,
            ERROR_SUCCESS | ERROR_FILE_NOT_FOUND | ERROR_PATH_NOT_FOUND
        ) {
            return Err(io::Error::from_raw_os_error(result.0 as i32).to_string());
        }
    }
    Ok(())
}

pub(crate) fn remove_install_entry(root: &Path, qa_id: Option<&str>) -> Result<(), String> {
    let _write = StartupWrite::acquire()?;
    if let Some(id) = qa_id {
        let value = HSTRING::from(format!("ACTION75-QA-{id}"));
        remove_owned(root, PCWSTR(value.as_ptr()))
    } else {
        remove_owned(root, ENTRY)
    }
}

fn run_command(root: &Path) -> io::Result<OsString> {
    let mut command = OsString::from("\"");
    command.push(std::path::absolute(root)?.join("dist\\Action75Manager\\Action75Manager.exe"));
    command.push("\" --background");
    Ok(command)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct MockStartup {
        enabled: bool,
        legacy: LegacyStartup,
        disable_denied: bool,
        run_denied: bool,
        restore_denied: bool,
        writes: Vec<(&'static str, bool)>,
    }

    impl MockStartup {
        fn new(legacy: LegacyStartup) -> Self {
            Self {
                enabled: false,
                legacy,
                disable_denied: false,
                run_denied: false,
                restore_denied: false,
                writes: vec![],
            }
        }
    }

    impl StartupBackend for MockStartup {
        fn enabled(&self) -> Result<bool, String> {
            Ok(self.enabled)
        }
        fn legacy(&self) -> LegacyStartup {
            self.legacy
        }
        fn set_enabled(&mut self, enabled: bool) -> Result<(), String> {
            self.writes.push(("run", enabled));
            if self.run_denied {
                return Err("Run write denied".into());
            }
            self.enabled = enabled;
            Ok(())
        }
        fn set_legacy(&mut self, enabled: bool) -> Result<(), String> {
            self.writes.push(("trial", enabled));
            if (enabled && self.restore_denied) || (!enabled && self.disable_denied) {
                return Err("Task update denied".into());
            }
            self.legacy = if enabled {
                LegacyStartup::Active
            } else {
                LegacyStartup::None
            };
            Ok(())
        }
    }

    #[test]
    fn reports_both_startup_routes_without_writing() {
        let mut mock = MockStartup::new(LegacyStartup::Active);
        assert_eq!(
            apply(&mut mock, StartupAction::Inspect).unwrap(),
            StartupState {
                enabled: false,
                legacy: LegacyStartup::Active
            }
        );
        mock.enabled = true;
        assert!(apply(&mut mock, StartupAction::Inspect).unwrap().enabled);
        assert!(mock.writes.is_empty());
    }

    #[test]
    fn both_entry_points_prevent_duplicate_startup() {
        let mut mock = MockStartup::new(LegacyStartup::Active);
        assert!(apply(&mut mock, StartupAction::Set(true)).is_err());
        assert!(apply(&mut mock, StartupAction::Toggle).is_err());
        assert!(mock.writes.is_empty());
        mock.enabled = true;
        assert_eq!(
            apply(&mut mock, StartupAction::Toggle).unwrap(),
            StartupState {
                enabled: false,
                legacy: LegacyStartup::Active
            }
        );
        assert_eq!(mock.writes, [("run", false)]);
    }

    #[test]
    fn explicit_migration_disables_trial_before_enabling_current_startup() {
        let mut mock = MockStartup::new(LegacyStartup::Active);
        assert_eq!(
            apply(&mut mock, StartupAction::MigrateTrial).unwrap(),
            StartupState {
                enabled: true,
                legacy: LegacyStartup::None
            }
        );
        assert_eq!(mock.writes, [("trial", false), ("run", true)]);
    }

    #[test]
    fn permission_error_does_not_write_current_startup() {
        let mut mock = MockStartup::new(LegacyStartup::Active);
        mock.disable_denied = true;
        let error = apply(&mut mock, StartupAction::MigrateTrial).unwrap_err();
        assert!(error.contains(scheduled_task::TASK_NAME));
        assert_eq!(mock.writes, [("trial", false)]);
        assert!(!mock.enabled);
        assert_eq!(mock.legacy, LegacyStartup::Active);
    }

    #[test]
    fn failed_run_write_restores_trial_and_surfaces_restore_failure() {
        let mut mock = MockStartup::new(LegacyStartup::Active);
        mock.run_denied = true;
        let error = apply(&mut mock, StartupAction::MigrateTrial).unwrap_err();
        assert!(error.contains("已恢复"));
        assert_eq!(
            mock.writes,
            [("trial", false), ("run", true), ("trial", true)]
        );
        assert_eq!(mock.legacy, LegacyStartup::Active);
        mock.restore_denied = true;
        let error = apply(&mut mock, StartupAction::MigrateTrial).unwrap_err();
        assert!(error.contains("也失败"));
    }

    #[test]
    fn stale_and_unrecognized_tasks_cannot_be_migrated() {
        for legacy in [
            LegacyStartup::None,
            LegacyStartup::Unidentified,
            LegacyStartup::Unavailable,
        ] {
            let mut mock = MockStartup::new(legacy);
            assert!(apply(&mut mock, StartupAction::MigrateTrial).is_err());
            assert!(mock.writes.is_empty());
            assert!(apply(&mut mock, StartupAction::Set(true)).unwrap().enabled);
            assert_eq!(mock.writes, [("run", true)]);
        }
    }

    #[test]
    fn current_startup_targets_the_selected_installation_in_background() {
        assert_eq!(
            run_command(Path::new(r"C:\测试目录\Action75 QA")).unwrap(),
            r#""C:\测试目录\Action75 QA\dist\Action75Manager\Action75Manager.exe" --background"#,
        );
    }

    #[test]
    fn startup_ownership_matches_only_the_executable_argument() {
        let scratch = tempfile::tempdir().unwrap();
        let first = scratch.path().join("installed copy");
        let second = scratch.path().join("portable copy");
        for root in [&first, &second] {
            let path = root.join("dist/Action75Manager/Action75Manager.exe");
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, "").unwrap();
        }
        let owned = run_command(&first).unwrap();
        let other = run_command(&second).unwrap();
        assert!(command_targets(&first, &owned));
        assert!(!command_targets(&first, &other));
        assert!(!command_targets(
            &first,
            &OsString::from(format!("\"\" {}", owned.to_string_lossy()))
        ));
    }
}
