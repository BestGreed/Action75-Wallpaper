use std::path::Path;
use windows::{
    Win32::{
        Foundation::{CloseHandle, ERROR_FILE_NOT_FOUND, HANDLE, HLOCAL, LocalFree, VARIANT_BOOL},
        Security::{
            Authorization::ConvertSidToStringSidW, GetTokenInformation, LookupAccountNameW, PSID,
            SID_NAME_USE, TOKEN_QUERY, TOKEN_USER, TokenUser,
        },
        System::{
            Com::{
                CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
                CoUninitialize,
            },
            TaskScheduler::{
                IExecAction, ILogonTrigger, IRegisteredTask, ITaskService,
                TASK_LOGON_INTERACTIVE_TOKEN, TASK_LOGON_TYPE, TASK_RUNLEVEL_HIGHEST,
                TASK_RUNLEVEL_TYPE, TaskScheduler,
            },
            Threading::{GetCurrentProcess, OpenProcessToken},
        },
    },
    core::{BSTR, HRESULT, Interface, PCWSTR, PWSTR, VARIANT},
};

use super::LegacyStartup;

pub(super) const TASK_NAME: &str = "ACTION75-Autostart";

struct ComApartment;

impl Drop for ComApartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

struct RegisteredTask {
    task: IRegisteredTask,
    _apartment: ComApartment,
}

impl RegisteredTask {
    fn find() -> windows::core::Result<Option<Self>> {
        unsafe {
            CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
            let apartment = ComApartment;
            let service: ITaskService =
                CoCreateInstance(&TaskScheduler, None, CLSCTX_INPROC_SERVER)?;
            let local = VARIANT::default();
            service.Connect(&local, &local, &local, &local)?;
            let folder = service.GetFolder(&BSTR::from("\\"))?;
            match folder.GetTask(&BSTR::from(TASK_NAME)) {
                Ok(task) => Ok(Some(Self {
                    task,
                    _apartment: apartment,
                })),
                Err(error) if error.code() == HRESULT::from_win32(ERROR_FILE_NOT_FOUND.0) => {
                    Ok(None)
                }
                Err(error) => Err(error),
            }
        }
    }

    fn is_trial_for_current_user(&self) -> windows::core::Result<bool> {
        unsafe {
            let definition = self.task.Definition()?;
            let actions = definition.Actions()?;
            let triggers = definition.Triggers()?;
            let mut action_count = 0;
            let mut trigger_count = 0;
            actions.Count(&mut action_count)?;
            triggers.Count(&mut trigger_count)?;
            if action_count != 1 || trigger_count != 1 {
                return Ok(false);
            }
            let Ok(action) = actions.get_Item(1)?.cast::<IExecAction>() else {
                return Ok(false);
            };
            let Ok(trigger) = triggers.get_Item(1)?.cast::<ILogonTrigger>() else {
                return Ok(false);
            };
            let mut executable = BSTR::new();
            let mut arguments = BSTR::new();
            let mut working_directory = BSTR::new();
            action.Path(&mut executable)?;
            action.Arguments(&mut arguments)?;
            action.WorkingDirectory(&mut working_directory)?;
            if !matches_trial_command(
                &executable.to_string(),
                &arguments.to_string(),
                &working_directory.to_string(),
            ) {
                return Ok(false);
            }

            let principal = definition.Principal()?;
            let mut principal_id = BSTR::new();
            let mut trigger_id = BSTR::new();
            let mut logon_type = TASK_LOGON_TYPE::default();
            let mut run_level = TASK_RUNLEVEL_TYPE::default();
            let mut trigger_enabled = VARIANT_BOOL::default();
            principal.UserId(&mut principal_id)?;
            principal.LogonType(&mut logon_type)?;
            principal.RunLevel(&mut run_level)?;
            trigger.UserId(&mut trigger_id)?;
            trigger.Enabled(&mut trigger_enabled)?;
            let sid = current_user_sid()?;
            Ok(logon_type == TASK_LOGON_INTERACTIVE_TOKEN
                && run_level == TASK_RUNLEVEL_HIGHEST
                && trigger_enabled.as_bool()
                && matches_logon_user(&sid, &principal_id.to_string(), &trigger_id.to_string()))
        }
    }
}

pub(super) fn inspect() -> LegacyStartup {
    probe().unwrap_or(LegacyStartup::Unavailable)
}

fn probe() -> windows::core::Result<LegacyStartup> {
    let Some(task) = RegisteredTask::find()? else {
        return Ok(LegacyStartup::None);
    };
    if !unsafe { task.task.Enabled()? }.as_bool() {
        return Ok(LegacyStartup::None);
    }
    Ok(if task.is_trial_for_current_user()? {
        LegacyStartup::Active
    } else {
        LegacyStartup::Unidentified
    })
}

pub(super) fn set_enabled(enabled: bool) -> Result<(), String> {
    let task = RegisteredTask::find()
        .map_err(|error| error.to_string())?
        .ok_or("旧版登录任务已不存在")?;
    if !task
        .is_trial_for_current_user()
        .map_err(|error| error.to_string())?
    {
        return Err("旧版登录任务已变化，无法确认归属".into());
    }
    unsafe { task.task.SetEnabled(VARIANT_BOOL::from(enabled)) }.map_err(|error| error.to_string())
}

fn matches_trial_command(executable: &str, arguments: &str, directory: &str) -> bool {
    let root = Path::new(directory);
    root.is_absolute()
        && root
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| name.eq_ignore_ascii_case("Action75-v2-Trial"))
        && executable.eq_ignore_ascii_case(
            &root
                .join("dist\\Action75Manager\\Action75Manager.exe")
                .to_string_lossy(),
        )
        && arguments == format!("--root \"{directory}\" --game-compatible")
}

fn matches_logon_user(current_sid: &str, principal: &str, trigger_user: &str) -> bool {
    let matches = |account: &str| {
        account.eq_ignore_ascii_case(current_sid)
            || (!account.is_empty() && account_sid(account).is_ok_and(|sid| sid == current_sid))
    };
    matches(principal) && matches(trigger_user)
}

fn account_sid(account: &str) -> windows::core::Result<String> {
    let name = windows::core::HSTRING::from(account);
    // SECURITY_MAX_SID_SIZE is 68 bytes.
    let mut sid = [0u32; 17];
    let mut domain = [0u16; 256];
    let mut sid_bytes = size_of_val(&sid) as u32;
    let mut domain_chars = domain.len() as u32;
    let sid = PSID(sid.as_mut_ptr().cast());
    unsafe {
        LookupAccountNameW(
            PCWSTR::null(),
            &name,
            sid,
            &mut sid_bytes,
            PWSTR(domain.as_mut_ptr()),
            &mut domain_chars,
            &mut SID_NAME_USE::default(),
        )?;
    }
    sid_string(sid)
}

fn sid_string(sid: PSID) -> windows::core::Result<String> {
    unsafe {
        let mut string = PWSTR::null();
        ConvertSidToStringSidW(sid, &mut string)?;
        let result = string.to_string();
        let _ = LocalFree(HLOCAL(string.0.cast()));
        Ok(result?)
    }
}

fn current_user_sid() -> windows::core::Result<String> {
    #[repr(C)]
    struct UserBuffer {
        user: TOKEN_USER,
        sid_storage: [u8; 68],
    }
    let mut buffer = UserBuffer {
        user: TOKEN_USER::default(),
        sid_storage: [0; 68],
    };
    let mut token = HANDLE::default();
    let mut bytes = 0;
    unsafe {
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token)?;
        let result = GetTokenInformation(
            token,
            TokenUser,
            Some((&mut buffer as *mut UserBuffer).cast()),
            size_of::<UserBuffer>() as u32,
            &mut bytes,
        );
        let _ = CloseHandle(token);
        result?;

        sid_string(buffer.user.User.Sid)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trial_signature_requires_the_same_absolute_root_and_game_compatibility() {
        let root = r"D:\Action75 QA\Action75-v2-Trial";
        let executable = format!(r"{root}\dist\Action75Manager\Action75Manager.exe");
        let arguments = format!(r#"--root "{root}" --game-compatible"#);
        assert!(matches_trial_command(&executable, &arguments, root));
        assert!(!matches_trial_command(&executable, "--background", root));
        assert!(!matches_trial_command(
            &executable,
            &arguments,
            r"E:\Action75-v2-Trial"
        ));
        assert!(!matches_trial_command(
            r"D:\unrelated.exe",
            &arguments,
            root
        ));
        let relative = "Action75-v2-Trial";
        assert!(!matches_trial_command(
            r"Action75-v2-Trial\dist\Action75Manager\Action75Manager.exe",
            r#"--root "Action75-v2-Trial" --game-compatible"#,
            relative,
        ));
    }

    #[test]
    fn scheduled_task_accepts_windows_account_names() {
        let sid = current_user_sid().unwrap();
        let name = std::env::var("USERNAME").unwrap();
        let domain = std::env::var("USERDOMAIN").unwrap();
        assert!(matches_logon_user(
            &sid,
            &name,
            &format!(r"{domain}\{name}")
        ));
    }

    #[test]
    fn current_user_must_own_the_principal_and_the_logon_trigger() {
        let current = "S-1-5-21-123-456-789-1001";
        let different = "S-1-5-21-123-456-789-1002";
        assert!(matches_logon_user(current, current, current));
        assert!(!matches_logon_user(current, current, ""));
        assert!(!matches_logon_user(current, different, current));
        assert!(!matches_logon_user(current, current, different));
    }
}
