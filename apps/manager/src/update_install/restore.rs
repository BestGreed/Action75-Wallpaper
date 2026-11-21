use std::{
    process::Child,
    thread,
    time::{Duration, Instant},
};

pub(super) fn confirm(
    child: &mut Child,
    timeout: Duration,
    mut status: impl FnMut(Duration) -> Result<Option<u32>, String>,
) -> Result<(), String> {
    let mut pending = PendingRestore {
        child,
        confirmed: false,
    };
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(result) = pending.child.try_wait().map_err(|e| e.to_string())? {
            return Err(format!("恢复播放器失败（{result}）"));
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err("恢复播放器超时".into());
        }
        let observed = status(remaining)?;
        if Instant::now() >= deadline {
            return Err("恢复播放器超时".into());
        }
        if let Some(pid) = observed {
            if pid == pending.child.id() {
                if let Some(result) = pending.child.try_wait().map_err(|e| e.to_string())? {
                    return Err(format!("恢复播放器失败（{result}）"));
                }
                pending.confirmed = true;
                return Ok(());
            }
            return Err("恢复期间播放器实例已发生变化".into());
        }
        thread::sleep(
            deadline
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(100)),
        );
    }
}

// A failed restore must not leave an unconfirmed player behind. The stored
// Child handle identifies only the process created by this restoration attempt.
struct PendingRestore<'a> {
    child: &'a mut Child,
    confirmed: bool,
}

impl Drop for PendingRestore<'_> {
    fn drop(&mut self) {
        if !self.confirmed && self.child.try_wait().ok().flatten().is_none() {
            if self.child.kill().is_ok() {
                let _ = self.child.wait();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(windows)]
    use std::os::windows::process::CommandExt;
    use std::process::{Command, Stdio};

    struct Fixture(Child);
    impl Fixture {
        fn start() -> Self {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command
                .args(["--ignored", "--nocapture", "restore_child_fixture"])
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
    #[ignore = "child-process fixture; started only by restore tests"]
    fn restore_child_fixture() {
        thread::sleep(Duration::from_secs(30));
    }

    #[test]
    fn failed_restore_reaps_only_its_own_child() {
        for failure in ["读取超时", "播放器权限不匹配（5）"] {
            let mut fixture = Fixture::start();
            let mut unrelated = Fixture::start();
            let result = confirm(&mut fixture.0, Duration::from_millis(50), |_| {
                Err(failure.into())
            });
            assert_eq!(result, Err(failure.into()));
            assert!(
                fixture.0.try_wait().unwrap().is_some(),
                "failed restore left its spawned player running"
            );
            assert!(unrelated.0.try_wait().unwrap().is_none());
        }
    }

    #[test]
    fn unready_or_replaced_restore_reaps_its_child() {
        for replaced in [false, true] {
            let mut fixture = Fixture::start();
            let other = fixture.0.id().wrapping_add(1);
            assert!(
                confirm(&mut fixture.0, Duration::from_millis(20), |_| {
                    Ok(replaced.then_some(other))
                })
                .is_err()
            );
            assert!(fixture.0.try_wait().unwrap().is_some());
        }
    }

    #[test]
    fn expired_restore_does_not_start_another_status_request() {
        let mut fixture = Fixture::start();
        let start = Instant::now();
        let mut requests = 0;
        assert!(
            confirm(&mut fixture.0, Duration::from_millis(15), |remaining| {
                requests += 1;
                thread::sleep(remaining);
                Ok(None)
            })
            .is_err()
        );
        assert_eq!(requests, 1);
        assert!(start.elapsed() < Duration::from_millis(500));
        assert!(fixture.0.try_wait().unwrap().is_some());
    }

    #[test]
    fn late_confirmation_cannot_keep_a_failed_restore_running() {
        let mut fixture = Fixture::start();
        let pid = fixture.0.id();
        assert!(
            confirm(&mut fixture.0, Duration::from_millis(5), |_| {
                thread::sleep(Duration::from_millis(15));
                Ok(Some(pid))
            })
            .is_err()
        );
        assert!(fixture.0.try_wait().unwrap().is_some());
    }

    #[test]
    fn confirmed_restore_keeps_the_exact_child_running() {
        let mut fixture = Fixture::start();
        let pid = fixture.0.id();
        assert!(
            confirm(&mut fixture.0, Duration::from_millis(50), |remaining| {
                assert!(!remaining.is_zero());
                assert!(remaining <= Duration::from_millis(50));
                Ok(Some(pid))
            })
            .is_ok()
        );
        assert!(fixture.0.try_wait().unwrap().is_none());
    }
}
