use serde_json::{Value, json};
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::{
    path::Path,
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

const START_TIMEOUT: Duration = Duration::from_secs(6);
const OFFLINE: &str = "播放器未启动";

pub(crate) fn start(root: &Path) -> Result<(), String> {
    let root = std::path::absolute(root).map_err(|error| error.to_string())?;
    start_with(
        |timeout| {
            crate::player_control::command_timeout(&root, json!({"action":"status"}), timeout)
        },
        || {
            let mut command = Command::new(crate::player(&root));
            command
                .arg("--root")
                .arg(&root)
                .current_dir(&root)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            #[cfg(windows)]
            command.creation_flags(0x08000000);
            command
                .spawn()
                .map_err(|error| format!("无法启动播放器：{error}"))
        },
        START_TIMEOUT,
    )
}

fn ready_pid(status: &Value) -> Result<u32, String> {
    (status["running"] == true)
        .then(|| {
            status["playerPid"]
                .as_u64()
                .and_then(|pid| u32::try_from(pid).ok())
        })
        .flatten()
        .filter(|pid| *pid != 0)
        .ok_or_else(|| "播放器未提供有效的就绪状态".into())
}

fn start_with(
    mut status: impl FnMut(Duration) -> Result<Value, String>,
    spawn: impl FnOnce() -> Result<Child, String>,
    timeout: Duration,
) -> Result<(), String> {
    match status(timeout) {
        Ok(status) => return ready_pid(&status).map(|_| ()),
        // A timeout, invalid reply or identity failure does not prove absence.
        Err(error) if error == OFFLINE => {}
        Err(error) => return Err(error),
    }
    let mut child = spawn()?;
    confirm(&mut child, timeout, status)
}

fn confirm(
    child: &mut Child,
    timeout: Duration,
    mut status: impl FnMut(Duration) -> Result<Value, String>,
) -> Result<(), String> {
    let mut pending = PendingStart {
        child,
        ready: false,
    };
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(exit) = pending
            .child
            .try_wait()
            .map_err(|error| error.to_string())?
        {
            return Err(format!("播放器在就绪前退出（{exit}）"));
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err("播放器没有就绪，请检查副屏和运行目录".into());
        }
        let response = status(remaining);
        if Instant::now() >= deadline {
            return Err("播放器没有就绪，请检查副屏和运行目录".into());
        }
        match response {
            Ok(status) => {
                if ready_pid(&status)? != pending.child.id() {
                    return Err("启动期间播放器实例已变化，请重新连接".into());
                }
                if pending
                    .child
                    .try_wait()
                    .map_err(|error| error.to_string())?
                    .is_some()
                {
                    return Err("播放器在就绪前退出".into());
                }
                pending.ready = true;
                return Ok(());
            }
            Err(error) if error == OFFLINE => {}
            Err(error) => return Err(error),
        }
        thread::sleep(
            deadline
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(100)),
        );
    }
}

struct PendingStart<'a> {
    child: &'a mut Child,
    ready: bool,
}

impl Drop for PendingStart<'_> {
    fn drop(&mut self) {
        if !self.ready
            && self.child.try_wait().ok().flatten().is_none()
            && self.child.kill().is_ok()
        {
            let _ = self.child.wait();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Fixture(Child);
    impl Fixture {
        fn start() -> Self {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command
                .args(["--ignored", "--nocapture", "player_start_child_fixture"])
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            #[cfg(windows)]
            command.creation_flags(0x08000000);
            Self(command.spawn().unwrap())
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }

    #[test]
    #[ignore = "owned child fixture; never starts a player or opens a window"]
    fn player_start_child_fixture() {
        thread::sleep(Duration::from_secs(30));
    }

    #[test]
    fn ambiguous_connection_failures_never_spawn_another_player() {
        for error in [
            "读取超时",
            "播放器运行目录与当前项目不一致",
            "播放器权限不匹配（5）",
            "无效协议",
        ] {
            assert_eq!(
                start_with(
                    |_| Err(error.into()),
                    || panic!("must not spawn"),
                    START_TIMEOUT
                ),
                Err(error.into())
            );
        }
        assert!(
            start_with(
                |_| Ok(json!({"running":false,"playerPid":42})),
                || panic!("must not spawn"),
                START_TIMEOUT
            )
            .is_err()
        );
        assert!(
            start_with(
                |_| Ok(json!({"running":true,"playerPid":42})),
                || panic!("must not spawn"),
                START_TIMEOUT
            )
            .is_ok()
        );
    }

    #[test]
    fn only_confirmed_absence_attempts_launch_and_reports_spawn_failure() {
        assert_eq!(
            start_with(
                |_| Err(OFFLINE.into()),
                || Err("无法启动播放器：missing fixture".into()),
                START_TIMEOUT
            ),
            Err("无法启动播放器：missing fixture".into())
        );
    }

    #[test]
    fn failed_or_replaced_start_reaps_only_its_owned_child() {
        for replaced in [false, true] {
            let mut child = Fixture::start();
            let mut unrelated = Fixture::start();
            let other = child.0.id().wrapping_add(1);
            assert!(
                confirm(&mut child.0, Duration::from_millis(50), |_| {
                    if replaced {
                        Ok(json!({"running":true,"playerPid":other}))
                    } else {
                        Err("读取超时".into())
                    }
                })
                .is_err()
            );
            assert!(child.0.try_wait().unwrap().is_some());
            assert!(unrelated.0.try_wait().unwrap().is_none());
        }
    }

    #[test]
    fn startup_deadline_bounds_the_probe_and_rejects_late_success() {
        let mut child = Fixture::start();
        let pid = child.0.id();
        let start = Instant::now();
        let mut calls = 0;
        assert!(
            confirm(&mut child.0, Duration::from_millis(15), |remaining| {
                calls += 1;
                assert!(remaining <= Duration::from_millis(15));
                thread::sleep(remaining + Duration::from_millis(5));
                Ok(json!({"running":true,"playerPid":pid}))
            })
            .is_err()
        );
        assert_eq!(calls, 1);
        assert!(start.elapsed() < Duration::from_secs(1));
        assert!(child.0.try_wait().unwrap().is_some());
    }

    #[test]
    fn exact_ready_child_remains_running() {
        let mut child = Fixture::start();
        let pid = child.0.id();
        assert!(
            confirm(&mut child.0, Duration::from_millis(100), |_| Ok(
                json!({"running":true,"playerPid":pid})
            ))
            .is_ok()
        );
        assert!(child.0.try_wait().unwrap().is_none());
    }
}
