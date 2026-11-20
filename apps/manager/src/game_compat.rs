use crate as core;
use eframe::egui;
use serde_json::{Value, json};
use std::os::windows::process::CommandExt;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

#[derive(Clone)]
pub struct Handoff {
    pub directory: PathBuf,
    pub token: String,
}
impl Handoff {
    fn create() -> Result<Self, String> {
        let token = win::random_token()?;
        let directory =
            std::env::temp_dir().join(format!("action75-admin-{}-{token}", std::process::id()));
        fs::create_dir(&directory).map_err(|e| e.to_string())?;
        Ok(Self { directory, token })
    }
    pub fn from_args(
        directory: Option<PathBuf>,
        token: Option<String>,
    ) -> Result<Option<Self>, String> {
        match (directory, token) {
            (None, None) => Ok(None),
            (Some(directory), Some(token))
                if token.len() == 32
                    && token.bytes().all(|b| b.is_ascii_hexdigit())
                    && directory.is_dir() =>
            {
                Ok(Some(Self { directory, token }))
            }
            _ => Err("管理员交接参数无效".into()),
        }
    }
    fn report(
        &self,
        root: &Path,
        ready: bool,
        restart: bool,
        message: &str,
        native_pid: u32,
    ) -> Result<(), String> {
        let value = json!({"token":self.token,"managerPid":std::process::id(),"runtimeRoot":root,"ready":ready,"restart":restart,"error":message,"playerPid":native_pid});
        let temp = self.directory.join("outcome.tmp");
        fs::write(
            &temp,
            serde_json::to_vec(&value).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        core::atomic_replace(&temp, &self.directory.join("outcome.json"))
    }
}
fn same_path(a: &Path, b: &Path) -> bool {
    match (fs::canonicalize(a), fs::canonicalize(b)) {
        (Ok(a), Ok(b)) => a.to_string_lossy().to_lowercase() == b.to_string_lossy().to_lowercase(),
        _ => false,
    }
}
fn identity(root: &Path, status: &Value) -> Result<u32, String> {
    let reported = status["runtimeRoot"]
        .as_str()
        .ok_or("播放器未提供目录身份，请先更新播放器")?;
    if !same_path(root, Path::new(reported)) {
        return Err("当前播放器不属于此软件目录".into());
    }
    status["playerPid"]
        .as_u64()
        .and_then(|p| u32::try_from(p).ok())
        .filter(|p| *p != 0 && *p != std::process::id())
        .ok_or_else(|| "播放器进程身份无效".into())
}
trait Operations {
    fn status(&mut self) -> Result<Option<Value>, String>;
    fn validate(&mut self, pid: u32) -> Result<(), String>;
    fn stop_and_wait(&mut self, pid: u32) -> Result<(), String>;
    fn start_and_confirm(&mut self) -> Result<u32, String>;
}
fn take_over<O: Operations>(root: &Path, ops: &mut O, restart: &mut bool) -> Result<u32, String> {
    if let Some(status) = ops.status()? {
        let pid = identity(root, &status)?;
        ops.validate(pid)?;
        if status["inputElevated"] == true {
            return Ok(pid);
        }
        // Once stop is sent, the broker must be able to restore the old role
        // even if the response is lost after the server acted.
        *restart = true;
        ops.stop_and_wait(pid)?;
    }
    ops.start_and_confirm()
}
struct NativeOps {
    root: PathBuf,
    started: Option<Child>,
}
impl Operations for NativeOps {
    fn status(&mut self) -> Result<Option<Value>, String> {
        match core::command(&self.root, json!({"action":"status"})) {
            Ok(s) => Ok(Some(s)),
            Err(e) if e == "播放器未启动" => Ok(None),
            Err(e) => Err(e),
        }
    }
    fn validate(&mut self, pid: u32) -> Result<(), String> {
        win::validate_process(pid, &core::player(&self.root)).map(|_| ())
    }
    fn stop_and_wait(&mut self, pid: u32) -> Result<(), String> {
        let process = win::validate_process(pid, &core::player(&self.root))?;
        // Recheck identity immediately before addressing the existing pipe.
        let current = core::command(&self.root, json!({"action":"status"}))?;
        if identity(&self.root, &current)? != pid {
            return Err("播放器已切换，请重试".into());
        }
        core::command(
            &self.root,
            json!({"action":"stop","expectedPlayerPid":pid,"expectedRuntimeRoot":current["runtimeRoot"]}),
        )?;
        if process.wait(Duration::from_secs(7))? {
            Ok(())
        } else {
            Err("原播放器尚未退出，未启动重复进程".into())
        }
    }
    fn start_and_confirm(&mut self) -> Result<u32, String> {
        if let Some(status) = self.status()? {
            let pid = identity(&self.root, &status)?;
            self.validate(pid)?;
            if status["inputElevated"] == true {
                return Ok(pid);
            }
            return Err("播放器已由其他会话重新启动，请重试".into());
        }
        let child = Command::new(core::player(&self.root))
            .arg("--root")
            .arg(&self.root)
            .current_dir(&self.root)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .creation_flags(0x08000000)
            .spawn()
            .map_err(|e| e.to_string())?;
        let pid = child.id();
        self.started = Some(child);
        let deadline = Instant::now() + Duration::from_secs(12);
        loop {
            if let Ok(status) = core::command(&self.root, json!({"action":"status"})) {
                if identity(&self.root, &status)? == pid && status["inputElevated"] == true {
                    win::validate_process(pid, &core::player(&self.root))?;
                    return Ok(pid);
                }
            }
            if self
                .started
                .as_mut()
                .unwrap()
                .try_wait()
                .map_err(|e| e.to_string())?
                .is_some()
            {
                return Err("管理员播放器启动失败".into());
            }
            if Instant::now() >= deadline {
                return Err("管理员播放器没有就绪".into());
            }
            thread::sleep(Duration::from_millis(100));
        }
    }
}
impl NativeOps {
    fn cleanup(&mut self) {
        if let Some(mut child) = self.started.take() {
            if child.try_wait().ok().flatten().is_none() {
                if let Ok(status) = core::command(&self.root, json!({"action":"status"})) {
                    if identity(&self.root, &status).ok() == Some(child.id()) {
                        let _ = core::command(
                            &self.root,
                            json!({"action":"stop","expectedPlayerPid":child.id(),"expectedRuntimeRoot":status["runtimeRoot"]}),
                        );
                    }
                }
                for _ in 0..20 {
                    if child.try_wait().ok().flatten().is_some() {
                        return;
                    }
                    thread::sleep(Duration::from_millis(50));
                }
                // Only this exact Child was spawned by this startup attempt.
                let _ = child.kill();
                let _ = child.wait();
            }
        }
    }
}

pub(crate) fn stop_player(root: &Path) -> Result<(), String> {
    let mut ops = NativeOps {
        root: root.to_owned(),
        started: None,
    };
    if let Some(status) = ops.status()? {
        ops.stop_and_wait(identity(root, &status)?)?;
    }
    Ok(())
}

pub struct Prepared {
    ops: NativeOps,
    pub pid: u32,
    restart: bool,
    handoff: Option<Handoff>,
    committed: bool,
}
impl Prepared {
    pub fn ready(mut self) -> Result<(), String> {
        if let Some(h) = &self.handoff {
            h.report(&self.ops.root, true, false, "", self.pid)?;
        }
        self.committed = true;
        self.ops.started.take();
        Ok(())
    }
}
impl Drop for Prepared {
    fn drop(&mut self) {
        if !self.committed {
            self.ops.cleanup();
            if let Some(h) = &self.handoff {
                let _ = h.report(&self.ops.root, false, self.restart, "管理员界面未能启动", 0);
            }
        }
    }
}
pub fn prepare(root: &Path, handoff: Option<Handoff>) -> Result<Prepared, String> {
    if !win::is_elevated()? {
        return Err("需要在 Windows 提示中确认管理员权限".into());
    }
    let mut ops = NativeOps {
        root: root.to_owned(),
        started: None,
    };
    let mut restart = false;
    match take_over(root, &mut ops, &mut restart) {
        Ok(pid) => Ok(Prepared {
            ops,
            pid,
            restart,
            handoff,
            committed: false,
        }),
        Err(error) => {
            ops.cleanup();
            if let Some(h) = handoff {
                let _ = h.report(root, false, restart, &error, 0);
            }
            Err(error)
        }
    }
}
pub fn is_elevated() -> Result<bool, String> {
    win::is_elevated()
}
fn outcome(
    root: &Path,
    h: &Handoff,
    pid: u32,
    value: &Value,
) -> Result<(bool, bool, String), String> {
    if value["token"] != h.token
        || value["managerPid"].as_u64() != Some(pid as u64)
        || !value["runtimeRoot"]
            .as_str()
            .is_some_and(|r| same_path(root, Path::new(r)))
    {
        return Err("管理员交接身份不匹配".into());
    }
    Ok((
        value["ready"] == true,
        value["restart"] == true,
        value["error"]
            .as_str()
            .unwrap_or("管理员启动失败")
            .to_owned(),
    ))
}
fn restore_medium(root: &Path) -> Result<(), String> {
    match core::command(root, json!({"action":"status"})) {
        Ok(status) => {
            let pid = identity(root, &status)?;
            win::validate_process(pid, &core::player(root))?;
            Ok(())
        }
        Err(error) if error == "播放器未启动" => core::start_player(root),
        Err(error) => Err(format!("无法确认播放器状态，未启动重复进程：{error}")),
    }
}
pub fn elevate_and_wait(root: &Path) -> Result<(), String> {
    let h = Handoff::create()?;
    let result = (|| {
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        let args = [
            "--root".to_owned(),
            root.to_string_lossy().into_owned(),
            "--game-compatible".into(),
            "--handoff".into(),
            h.directory.to_string_lossy().into_owned(),
            "--handoff-token".into(),
            h.token.clone(),
        ];
        let process = win::run_as(&exe, &args)?;
        let pid = process.pid()?;
        loop {
            if let Ok(bytes) = fs::read(h.directory.join("outcome.json")) {
                if let Ok(value) = serde_json::from_slice::<Value>(&bytes) {
                    if let Ok((ready, restart, message)) = outcome(root, &h, pid, &value) {
                        if ready {
                            return Ok(());
                        }
                        if restart {
                            restore_medium(root)
                                .map_err(|e| format!("{message}；恢复原播放器失败：{e}"))?;
                        }
                        return Err(message);
                    }
                }
                // Never trust or abandon a malformed/mismatched ACK while its
                // child is still alive. Exit below restores only a verified role.
            }
            if process.wait(Duration::from_millis(100))? {
                restore_medium(root).map_err(|e| format!("管理员界面提前退出；{e}"))?;
                return Err("管理员界面在交接前退出，原窗口和播放器已保留".into());
            }
            // Keep the broker and handoff alive while the accepted child is alive.
            // A late READY must never create a second independent GUI.
        }
    })();
    // Files belong to this request; never enumerate or remove other directories.
    let _ = fs::remove_file(h.directory.join("outcome.json"));
    let _ = fs::remove_file(h.directory.join("outcome.tmp"));
    let _ = fs::remove_dir(&h.directory);
    result
}
pub fn begin(root: PathBuf, ctx: egui::Context) -> mpsc::Receiver<Result<(), String>> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let result = elevate_and_wait(&root);
        let _ = tx.send(result);
        ctx.request_repaint_of(egui::ViewportId::ROOT);
    });
    rx
}
pub fn button(ui: &mut egui::Ui, enabled: bool) -> egui::Response {
    ui.add_enabled(enabled, egui::Button::new("管理员游戏兼容"))
}

pub(crate) mod win {
    use super::*;
    use std::{ffi::OsStr, os::windows::ffi::OsStrExt};
    #[repr(C)]
    struct ShellInfo {
        size: u32,
        mask: u32,
        hwnd: isize,
        verb: *const u16,
        file: *const u16,
        parameters: *const u16,
        directory: *const u16,
        show: i32,
        instance: isize,
        id_list: *mut std::ffi::c_void,
        class: *const u16,
        key: isize,
        hot_key: u32,
        icon: isize,
        process: isize,
    }
    #[link(name = "shell32")]
    unsafe extern "system" {
        fn ShellExecuteExW(info: *mut ShellInfo) -> i32;
    }
    #[link(name = "ole32")]
    unsafe extern "system" {
        fn CoInitializeEx(reserved: *mut std::ffi::c_void, mode: u32) -> i32;
        fn CoUninitialize();
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn CloseHandle(handle: *mut std::ffi::c_void) -> i32;
        fn GetCurrentProcess() -> isize;
        fn OpenProcess(access: u32, inherit: i32, pid: u32) -> isize;
        fn WaitForSingleObject(handle: *mut std::ffi::c_void, ms: u32) -> u32;
        fn GetProcessId(handle: isize) -> u32;
        fn QueryFullProcessImageNameW(
            handle: isize,
            flags: u32,
            buffer: *mut u16,
            size: *mut u32,
        ) -> i32;
    }
    #[link(name = "advapi32")]
    unsafe extern "system" {
        fn OpenProcessToken(process: isize, access: u32, token: *mut isize) -> i32;
        fn GetTokenInformation(
            token: isize,
            class: u32,
            data: *mut u32,
            size: u32,
            returned: *mut u32,
        ) -> i32;
    }
    #[link(name = "bcrypt")]
    unsafe extern "system" {
        fn BCryptGenRandom(algorithm: isize, buffer: *mut u8, size: u32, flags: u32) -> i32;
    }
    pub struct Process(isize);
    impl Drop for Process {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0 as *mut _);
            }
        }
    }
    impl Process {
        pub fn pid(&self) -> Result<u32, String> {
            let p = unsafe { GetProcessId(self.0) };
            if p == 0 {
                Err(std::io::Error::last_os_error().to_string())
            } else {
                Ok(p)
            }
        }
        pub fn wait(&self, time: Duration) -> Result<bool, String> {
            match unsafe {
                WaitForSingleObject(
                    self.0 as *mut _,
                    time.as_millis().min(u32::MAX as u128) as u32,
                )
            } {
                0 => Ok(true),
                258 => Ok(false),
                _ => Err(std::io::Error::last_os_error().to_string()),
            }
        }
    }
    pub fn is_elevated() -> Result<bool, String> {
        let mut token = 0;
        if unsafe { OpenProcessToken(GetCurrentProcess(), 8, &mut token) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let token = Process(token);
        let mut value = 0;
        let mut size = 0;
        if unsafe { GetTokenInformation(token.0, 20, &mut value, 4, &mut size) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        Ok(value != 0)
    }
    pub fn random_token() -> Result<String, String> {
        let mut bytes = [0u8; 16];
        if unsafe { BCryptGenRandom(0, bytes.as_mut_ptr(), 16, 2) } < 0 {
            return Err("无法创建管理员交接标识".into());
        }
        Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
    }
    fn wide(value: &OsStr) -> Vec<u16> {
        value.encode_wide().chain(Some(0)).collect()
    }
    pub fn quote(value: &str) -> String {
        let mut result = String::from("\"");
        let mut slashes = 0;
        for c in value.chars() {
            if c == '\\' {
                slashes += 1;
            } else {
                if c == '"' {
                    result.extend(std::iter::repeat_n('\\', slashes * 2 + 1));
                } else {
                    result.extend(std::iter::repeat_n('\\', slashes));
                }
                result.push(c);
                slashes = 0;
            }
        }
        result.extend(std::iter::repeat_n('\\', slashes * 2));
        result.push('"');
        result
    }
    pub fn run_as(exe: &Path, args: &[String]) -> Result<Process, String> {
        let initialized = unsafe { CoInitializeEx(std::ptr::null_mut(), 6) } >= 0;
        let verb = wide(OsStr::new("runas"));
        let file = wide(exe.as_os_str());
        let params = wide(OsStr::new(
            &args.iter().map(|a| quote(a)).collect::<Vec<_>>().join(" "),
        ));
        let mut info = ShellInfo {
            size: std::mem::size_of::<ShellInfo>() as u32,
            mask: 0x40 | 0x100 | 0x400,
            hwnd: 0,
            verb: verb.as_ptr(),
            file: file.as_ptr(),
            parameters: params.as_ptr(),
            directory: std::ptr::null(),
            show: 1,
            instance: 0,
            id_list: std::ptr::null_mut(),
            class: std::ptr::null(),
            key: 0,
            hot_key: 0,
            icon: 0,
            process: 0,
        };
        let success = unsafe { ShellExecuteExW(&mut info) };
        let error = std::io::Error::last_os_error();
        if initialized {
            unsafe {
                CoUninitialize();
            }
        }
        if success == 0 {
            return Err(if error.raw_os_error() == Some(1223) {
                "已取消管理员授权".into()
            } else {
                error.to_string()
            });
        }
        if info.process == 0 {
            return Err("Windows 未返回管理员进程".into());
        }
        Ok(Process(info.process))
    }
    pub fn validate_process(pid: u32, expected: &Path) -> Result<Process, String> {
        let handle = unsafe { OpenProcess(0x1000 | 0x100000, 0, pid) };
        if handle == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let process = Process(handle);
        let mut buffer = vec![0u16; 32768];
        let mut size = buffer.len() as u32;
        if unsafe { QueryFullProcessImageNameW(handle, 0, buffer.as_mut_ptr(), &mut size) } == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let path = PathBuf::from(String::from_utf16_lossy(&buffer[..size as usize]));
        if !same_path(&path, expected) {
            return Err("播放器 PID 对应的程序不属于此目录".into());
        }
        Ok(process)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Fake {
        root: PathBuf,
        status: Option<Value>,
        trace: Vec<&'static str>,
        fail: bool,
    }
    impl Operations for Fake {
        fn status(&mut self) -> Result<Option<Value>, String> {
            self.trace.push("status");
            Ok(self.status.clone())
        }
        fn validate(&mut self, _: u32) -> Result<(), String> {
            self.trace.push("validate");
            Ok(())
        }
        fn stop_and_wait(&mut self, _: u32) -> Result<(), String> {
            self.trace.push("stop+wait");
            Ok(())
        }
        fn start_and_confirm(&mut self) -> Result<u32, String> {
            self.trace.push("start+high-ack");
            if self.fail {
                Err("start failed".into())
            } else {
                Ok(123)
            }
        }
    }
    fn fake(elevated: bool) -> Fake {
        let root = std::env::temp_dir();
        Fake {
            status: Some(json!({"runtimeRoot":root,"playerPid":123,"inputElevated":elevated})),
            root,
            trace: vec![],
            fail: false,
        }
    }
    #[test]
    fn own_medium_is_stopped_and_waited_before_elevated_start() {
        let mut f = fake(false);
        let root = f.root.clone();
        let mut restore = false;
        assert_eq!(take_over(&root, &mut f, &mut restore).unwrap(), 123);
        assert!(restore);
        assert_eq!(
            f.trace,
            vec!["status", "validate", "stop+wait", "start+high-ack"]
        );
    }
    #[test]
    fn already_high_does_not_restart() {
        let mut f = fake(true);
        let root = f.root.clone();
        let mut restore = false;
        take_over(&root, &mut f, &mut restore).unwrap();
        assert!(!restore);
        assert_eq!(f.trace, vec!["status", "validate"]);
    }
    #[test]
    fn foreign_root_or_missing_pid_never_stops_anything() {
        for missing in [true, false] {
            let mut f = fake(false);
            if missing {
                f.status.as_mut().unwrap()["playerPid"] = Value::Null;
            } else {
                f.status.as_mut().unwrap()["runtimeRoot"] = json!(std::env::current_exe().unwrap());
            }
            let root = f.root.clone();
            let mut restore = false;
            assert!(take_over(&root, &mut f, &mut restore).is_err());
            assert!(!restore);
            assert_eq!(f.trace, vec!["status"]);
        }
    }
    #[test]
    fn failed_start_marks_medium_broker_restore() {
        let mut f = fake(false);
        f.fail = true;
        let root = f.root.clone();
        let mut restore = false;
        assert!(take_over(&root, &mut f, &mut restore).is_err());
        assert!(restore);
    }
    #[test]
    fn handoff_checks_token_pid_and_root() {
        let root = std::env::temp_dir();
        let h = Handoff {
            directory: root.clone(),
            token: "1".repeat(32),
        };
        let mut value = json!({"token":h.token,"managerPid":123,"runtimeRoot":root,"ready":true});
        assert!(outcome(&root, &h, 123, &value).unwrap().0);
        value["managerPid"] = json!(124);
        assert!(outcome(&root, &h, 123, &value).is_err());
        value["managerPid"] = json!(123);
        value["token"] = json!("bad");
        assert!(outcome(&root, &h, 123, &value).is_err());
    }
    #[test]
    fn windows_arguments_preserve_spaces_quotes_and_trailing_slashes() {
        assert_eq!(win::quote(r"D:\My Root\"), "\"D:\\My Root\\\\\"");
        assert_eq!(win::quote("a\"b"), "\"a\\\"b\"");
    }
    #[test]
    fn actual_button_is_disabled_until_profile_and_calibration_are_idle() {
        use egui::{Event, Modifiers, PointerButton, Pos2, RawInput, Rect, vec2};
        let ctx = egui::Context::default();
        for enabled in [false, true] {
            let mut clicked = false;
            let mut rect = Rect::NOTHING;
            let mut frame = |events| {
                let _ = ctx.run(
                    RawInput {
                        screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 200.))),
                        events,
                        ..Default::default()
                    },
                    |ctx| {
                        egui::CentralPanel::default().show(ctx, |ui| {
                            let r = button(ui, enabled);
                            rect = r.rect;
                            clicked |= r.clicked();
                        });
                    },
                );
                rect
            };
            let p = frame(vec![]).center();
            frame(vec![Event::PointerMoved(p)]);
            for pressed in [true, false] {
                frame(vec![Event::PointerButton {
                    pos: p,
                    button: PointerButton::Primary,
                    pressed,
                    modifiers: Modifiers::NONE,
                }]);
            }
            assert_eq!(clicked, enabled);
        }
    }
}
