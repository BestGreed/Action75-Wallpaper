use std::{
    ffi::{OsStr, OsString},
    fs,
    mem::size_of,
    os::windows::ffi::OsStrExt,
    path::Path,
};

use windows::{
    Win32::{
        Foundation::{
            CloseHandle, E_ACCESSDENIED, GENERIC_ALL, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT,
        },
        Security::{
            ACL, ACL_REVISION, AddAccessAllowedAce, CreateRestrictedToken, CreateWellKnownSid,
            DISABLE_MAX_PRIVILEGE, GetLengthSid, GetSidSubAuthority, GetSidSubAuthorityCount,
            GetTokenInformation, InitializeAcl, PSID, SID_AND_ATTRIBUTES, SetTokenInformation,
            TOKEN_ADJUST_DEFAULT, TOKEN_ASSIGN_PRIMARY, TOKEN_DEFAULT_DACL, TOKEN_DUPLICATE,
            TOKEN_MANDATORY_LABEL, TOKEN_QUERY, TOKEN_USER, TokenDefaultDacl, TokenIntegrityLevel,
            TokenUser, WinBuiltinAdministratorsSid, WinLocalSystemSid, WinMediumLabelSid,
        },
        System::{
            JobObjects::{
                AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_ACTIVE_PROCESS,
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOB_OBJECT_LIMIT_PROCESS_MEMORY,
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
                JobObjectBasicAccountingInformation, JobObjectExtendedLimitInformation,
                QueryInformationJobObject, SetInformationJobObject, TerminateJobObject,
            },
            StationsAndDesktops::{
                CloseDesktop, CreateDesktopW, DESKTOP_CONTROL_FLAGS, GetThreadDesktop, HDESK,
                SetThreadDesktop,
            },
            Threading::{
                BELOW_NORMAL_PRIORITY_CLASS, CREATE_NO_WINDOW, CREATE_SUSPENDED,
                CreateProcessAsUserW, GetCurrentProcess, GetCurrentThreadId, GetExitCodeProcess,
                OpenProcessToken, PROCESS_INFORMATION, ResumeThread, STARTUPINFOW,
                TerminateProcess, WaitForSingleObject,
            },
        },
    },
    core::{PCWSTR, PWSTR},
};

const TIMEOUT_MS: u32 = 8_000;
const INTEGRITY_GROUP: u32 = 0x20;
const MEDIUM_INTEGRITY: u32 = 0x2000;
const WORKER: &str = "Action75WallpaperWorker.exe";

struct Handle(HANDLE);

impl Drop for Handle {
    fn drop(&mut self) {
        let _ = unsafe { CloseHandle(self.0) };
    }
}

struct Desktop {
    trial: HDESK,
    previous: HDESK,
}

impl Drop for Desktop {
    fn drop(&mut self) {
        if unsafe { SetThreadDesktop(self.previous) }.is_ok() {
            let _ = unsafe { CloseDesktop(self.trial) };
        }
    }
}

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value.as_ref().encode_wide().chain(Some(0)).collect()
}

fn checked<T>(result: windows::core::Result<T>, context: &str) -> windows::core::Result<T> {
    result.map_err(|error| windows::core::Error::new(error.code(), format!("{context}，{error}")))
}

fn ordinary_token() -> windows::core::Result<Handle> {
    // Aligned storage for the small well-known SIDs.
    let mut administrators = [0u32; 17];
    let mut medium = [0u32; 17];
    let admin_sid = PSID(administrators.as_mut_ptr().cast());
    let medium_sid = PSID(medium.as_mut_ptr().cast());
    let mut source = HANDLE::default();
    let mut restricted = HANDLE::default();
    unsafe {
        checked(
            OpenProcessToken(
                GetCurrentProcess(),
                TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY,
                &mut source,
            ),
            "无法读取控制台权限",
        )?;
        let source = Handle(source);
        let elevated = token_integrity(source.0)? > MEDIUM_INTEGRITY;
        CreateWellKnownSid(
            WinBuiltinAdministratorsSid,
            PSID::default(),
            admin_sid,
            &mut 68,
        )?;
        checked(
            CreateRestrictedToken(
                source.0,
                DISABLE_MAX_PRIVILEGE,
                Some(&[SID_AND_ATTRIBUTES {
                    Sid: admin_sid,
                    Attributes: 0,
                }]),
                None,
                None,
                &mut restricted,
            ),
            "无法限制试播权限",
        )?;
        let restricted = Handle(restricted);
        CreateWellKnownSid(WinMediumLabelSid, PSID::default(), medium_sid, &mut 68)?;
        let label = TOKEN_MANDATORY_LABEL {
            Label: SID_AND_ATTRIBUTES {
                Sid: medium_sid,
                Attributes: INTEGRITY_GROUP,
            },
        };
        checked(
            SetTokenInformation(
                restricted.0,
                TokenIntegrityLevel,
                (&label as *const TOKEN_MANDATORY_LABEL).cast(),
                size_of::<TOKEN_MANDATORY_LABEL>() as u32 + GetLengthSid(medium_sid),
            ),
            "无法降为普通权限",
        )?;
        if elevated {
            checked(
                set_trial_default_dacl(restricted.0),
                "无法设置试播进程的默认访问权限",
            )?;
        }
        Ok(restricted)
    }
}

fn set_trial_default_dacl(token: HANDLE) -> windows::core::Result<()> {
    let mut user = [0usize; 16];
    let mut system = [0u32; 17];
    // ACL header plus two access-allowed ACEs with maximum-length SIDs.
    let mut storage = [0u32; 40];
    let acl = storage.as_mut_ptr().cast::<ACL>();
    let system_sid = PSID(system.as_mut_ptr().cast());
    unsafe {
        GetTokenInformation(
            token,
            TokenUser,
            Some(user.as_mut_ptr().cast()),
            size_of::<[usize; 16]>() as u32,
            &mut 0,
        )?;
        let user_sid = (*user.as_ptr().cast::<TOKEN_USER>()).User.Sid;
        CreateWellKnownSid(WinLocalSystemSid, PSID::default(), system_sid, &mut 68)?;
        InitializeAcl(acl, size_of::<[u32; 40]>() as u32, ACL_REVISION)?;
        // An elevated token's default ACL can grant access through Administrators,
        // which is deny-only after restriction. Give the trial owner access directly.
        AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL.0, user_sid)?;
        AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL.0, system_sid)?;
        let default_dacl = TOKEN_DEFAULT_DACL { DefaultDacl: acl };
        SetTokenInformation(
            token,
            TokenDefaultDacl,
            (&default_dacl as *const TOKEN_DEFAULT_DACL).cast(),
            size_of::<TOKEN_DEFAULT_DACL>() as u32,
        )
    }
}

pub(super) struct Exit {
    pub pid: u32,
    pub status: u32,
    pub timed_out: bool,
    pub integrity: u32,
    pub peak_bytes: usize,
    pub processes: u32,
}

pub(super) fn run(
    executable: &Path,
    stage: &Path,
    nonce: &str,
    native: bool,
    integrated: bool,
    cancel: Option<&std::sync::atomic::AtomicBool>,
) -> Result<Exit, String> {
    if native {
        let worker = executable.with_file_name(WORKER);
        if !fs::symlink_metadata(&worker)
            .is_ok_and(|metadata| metadata.is_file() && !super::is_reparse(&metadata))
        {
            return Err(format!("原生壁纸需要播放器同目录的 {WORKER}"));
        }
    }
    run_process(executable, stage, nonce, native, integrated, cancel)
        .map_err(|e| format!("无法运行隔离试播，{e}"))
}

fn run_process(
    exe: &Path,
    stage: &Path,
    nonce: &str,
    native: bool,
    integrated: bool,
    cancel: Option<&std::sync::atomic::AtomicBool>,
) -> windows::core::Result<Exit> {
    let token = ordinary_token()?;
    let desktop_name = format!("Action75Import-{nonce}");
    let desktop_path = wide(format!("WinSta0\\{desktop_name}"));
    let mut command = OsString::from("\"");
    command.push(exe);
    command.push("\" --root \"");
    command.push(stage);
    command.push("\" --import-trial");
    if integrated {
        command.push(" --integrated-gpu");
    }
    let mut command = wide(command);
    let application = wide(exe);
    let working_dir = wide(exe.parent().unwrap());
    let mut process = PROCESS_INFORMATION::default();
    let startup = STARTUPINFOW {
        cb: size_of::<STARTUPINFOW>() as u32,
        lpDesktop: PWSTR(desktop_path.as_ptr().cast_mut()),
        ..Default::default()
    };

    unsafe {
        let previous = GetThreadDesktop(GetCurrentThreadId())?;
        let _desktop = Desktop {
            trial: CreateDesktopW(
                PCWSTR(wide(desktop_name).as_ptr()),
                PCWSTR::null(),
                None,
                DESKTOP_CONTROL_FLAGS(0),
                0x01ff,
                None,
            )?,
            previous,
        };
        let job = Handle(CreateJobObjectW(None, PCWSTR::null())?);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS
            | JOB_OBJECT_LIMIT_PROCESS_MEMORY
            | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        // For a native import the player owns a child with its own single-process job.
        limits.BasicLimitInformation.ActiveProcessLimit = if native { 2 } else { 1 };
        limits.ProcessMemoryLimit = 768 * 1024 * 1024;
        SetInformationJobObject(
            job.0,
            JobObjectExtendedLimitInformation,
            (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
            size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
        )?;

        checked(
            CreateProcessAsUserW(
                token.0,
                PCWSTR(application.as_ptr()),
                PWSTR(command.as_mut_ptr()),
                None,
                None,
                false,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                None,
                PCWSTR(working_dir.as_ptr()),
                &startup,
                &mut process,
            ),
            "无法启动普通权限子进程",
        )?;
        let child = Handle(process.hProcess);
        let primary_thread = Handle(process.hThread);
        if let Err(error) = AssignProcessToJobObject(job.0, child.0) {
            let _ = TerminateProcess(child.0, 1);
            WaitForSingleObject(child.0, 2_000);
            return checked(Err(error), "无法限制试播进程资源");
        }
        let integrity = process_integrity(child.0)?;
        if integrity > MEDIUM_INTEGRITY {
            return Err(windows::core::Error::new(
                E_ACCESSDENIED,
                "试播进程的权限高于普通用户",
            ));
        }
        if ResumeThread(primary_thread.0) == u32::MAX {
            return Err(windows::core::Error::from_win32());
        }
        let started = std::time::Instant::now();
        let wait = loop {
            let remaining = TIMEOUT_MS
                .saturating_sub(started.elapsed().as_millis().min(u32::MAX as u128) as u32);
            let interval = if cancel.is_some() {
                remaining.min(100)
            } else {
                remaining
            };
            let state = WaitForSingleObject(child.0, interval);
            if state != WAIT_TIMEOUT || remaining == interval {
                break state;
            }
            if cancel.is_some_and(|flag| flag.load(std::sync::atomic::Ordering::Relaxed)) {
                TerminateJobObject(job.0, 1223)?;
                WaitForSingleObject(child.0, 2_000);
                return Err(windows::core::Error::new(
                    windows::core::HRESULT::from_win32(1223),
                    "已取消导入",
                ));
            }
        };
        let timed_out = wait == WAIT_TIMEOUT;
        if timed_out {
            TerminateJobObject(job.0, 124)?;
            if WaitForSingleObject(child.0, 2_000) != WAIT_OBJECT_0 {
                return Err(windows::core::Error::from_win32());
            }
        } else if wait != WAIT_OBJECT_0 {
            return Err(windows::core::Error::from_win32());
        }
        let mut status = 0;
        GetExitCodeProcess(child.0, &mut status)?;
        let peak_bytes = QueryInformationJobObject(
            job.0,
            JobObjectExtendedLimitInformation,
            (&mut limits as *mut JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
            size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            None,
        )
        .map_or(0, |()| limits.PeakProcessMemoryUsed);
        let mut accounting = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION::default();
        let processes = QueryInformationJobObject(
            job.0,
            JobObjectBasicAccountingInformation,
            (&mut accounting as *mut JOBOBJECT_BASIC_ACCOUNTING_INFORMATION).cast(),
            size_of::<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION>() as u32,
            None,
        )
        .map_or(0, |()| accounting.TotalProcesses);
        Ok(Exit {
            pid: process.dwProcessId,
            status,
            timed_out,
            integrity,
            peak_bytes,
            processes,
        })
    }
}

fn process_integrity(process: HANDLE) -> windows::core::Result<u32> {
    let mut token = HANDLE::default();
    unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token)? };
    let token = Handle(token);
    token_integrity(token.0)
}

fn token_integrity(token: HANDLE) -> windows::core::Result<u32> {
    let mut buffer = [0usize; 16];
    unsafe {
        GetTokenInformation(
            token,
            TokenIntegrityLevel,
            Some(buffer.as_mut_ptr().cast()),
            size_of::<[usize; 16]>() as u32,
            &mut 0,
        )?;
        let label = &*buffer.as_ptr().cast::<TOKEN_MANDATORY_LABEL>();
        let count = *GetSidSubAuthorityCount(label.Label.Sid);
        Ok(*GetSidSubAuthority(label.Label.Sid, u32::from(count - 1)))
    }
}
