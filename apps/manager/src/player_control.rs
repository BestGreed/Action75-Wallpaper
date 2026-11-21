//! Route commands to the player for this runtime directory.
use crate::{pack_model::Bundle, pipe_client, runtime_identity::installation_endpoint};
use serde_json::{Value, json};
use std::{
    path::Path,
    time::{Duration, Instant},
};

const COMMAND_TIMEOUT: Duration = Duration::from_secs(6);
const SELECT_REPLY_TIMEOUT: Duration = Duration::from_secs(6);
const SELECT_CONFIRM_TIMEOUT: Duration = Duration::from_secs(2);

pub(crate) struct SelectionFailure {
    pub error: String,
    pub observed: Option<Value>,
}

impl From<String> for SelectionFailure {
    fn from(error: String) -> Self {
        Self {
            error,
            observed: None,
        }
    }
}

#[cfg(windows)]
struct SelectionPlayer {
    pipe: String,
    root: String,
    endpoint: String,
    pid: u32,
    process: std::os::windows::io::OwnedHandle,
}

#[cfg(windows)]
impl SelectionPlayer {
    fn resolve(root: &Path) -> Result<(Self, Value), String> {
        use std::os::windows::io::FromRawHandle;
        use windows::Win32::System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE};

        let deadline = Instant::now() + COMMAND_TIMEOUT;
        let root = std::path::absolute(root).map_err(|error| error.to_string())?;
        let endpoint = installation_endpoint(&root)?;
        let absolute = root.to_str().ok_or("播放器运行目录无法编码")?;
        let remaining = || deadline.saturating_duration_since(Instant::now());
        let probe = json!({"action":"status"});
        let explicit = if let Some(name) = explicit_endpoint(&root) {
            explicit_status(
                exchange(&name, &probe, remaining(), pipe_client::request_timeout),
                &endpoint,
            )?
            .map(|(status, pid)| (name, status, pid))
        } else {
            None
        };
        let (pipe, status, pid) = if let Some(explicit) = explicit {
            explicit
        } else {
            let query = json!({"action":"status","expectedRuntimeRoot":absolute});
            let status = exchange(&endpoint, &query, remaining(), pipe_client::request_timeout)?;
            expect_runtime_root(&status["runtimeRoot"], &endpoint)?;
            let pid = status_pid(&status)?;
            (endpoint.clone(), status, pid)
        };
        // Keep a handle to the original process object across the switch. A
        // replacement with the same runtime path or recycled PID cannot confirm it.
        let process = unsafe { OpenProcess(PROCESS_SYNCHRONIZE, false, pid) }
            .map_err(|error| format!("无法确认播放器实例，{error}"))?;
        let process = unsafe { std::os::windows::io::OwnedHandle::from_raw_handle(process.0) };
        let player = Self {
            pipe,
            root: absolute.to_owned(),
            endpoint,
            pid,
            process,
        };
        player.check_status(&status)?;
        Ok((player, status))
    }

    fn ensure_alive(&self) -> Result<(), String> {
        use std::os::windows::io::AsRawHandle;
        use windows::Win32::{
            Foundation::{HANDLE, WAIT_TIMEOUT},
            System::Threading::WaitForSingleObject,
        };
        if unsafe { WaitForSingleObject(HANDLE(self.process.as_raw_handle()), 0) } != WAIT_TIMEOUT {
            return Err("原播放器实例已退出".into());
        }
        Ok(())
    }

    fn request(
        &self,
        mut request: Value,
        timeout: Duration,
        attempted: Option<&mut bool>,
    ) -> Result<Value, String> {
        self.ensure_alive()?;
        request["expectedRuntimeRoot"] = json!(self.root);
        request["expectedPlayerPid"] = json!(self.pid);
        let reply = pipe_client::request_with_delivery(
            &self.pipe,
            &request,
            timeout,
            Some(self.pid),
            attempted,
        )?;
        self.ensure_alive()?;
        Ok(reply)
    }

    fn check_status(&self, status: &Value) -> Result<(), String> {
        self.ensure_alive()?;
        expect_runtime_root(&status["runtimeRoot"], &self.endpoint)?;
        if status_pid(status)? != self.pid {
            return Err("播放器实例已变化".into());
        }
        if !status["packId"].is_string() {
            return Err("播放器未报告当前壁纸".into());
        }
        Ok(())
    }
}

#[cfg(windows)]
pub(crate) fn select_pack(root: &Path, id: &str) -> Result<(Value, Bundle), SelectionFailure> {
    let (player, before) = SelectionPlayer::resolve(root)?;
    let mut attempted = false;
    let reply = player.request(
        json!({"action":"selectPack","id":id}),
        SELECT_REPLY_TIMEOUT,
        Some(&mut attempted),
    );
    if let Ok(reply) = &reply
        && reply["ok"] == false
    {
        return Err(rejection(reply).into());
    }
    let status = match reply.and_then(|reply| {
        let status = reply_result(reply)?;
        player.check_status(&status)?;
        Ok(status)
    }) {
        Ok(status) => selection_status(id, status, None)?,
        Err(error) if !attempted => return Err(error.into()),
        Err(error) => {
            let status = player
                .request(json!({"action":"status"}), SELECT_CONFIRM_TIMEOUT, None)
                .and_then(reply_result)
                .and_then(|status| {
                    player.check_status(&status)?;
                    Ok(status)
                })
                .map_err(|reason| format!("无法确认壁纸切换结果，{error}。{reason}"))?;
            let status = selection_status(id, status, Some(&before))?;
            crate::storage::log(
                root,
                "INFO",
                &format!(
                    "未收到有效切换回执，已向原播放器 {} 确认壁纸 {id}",
                    player.pid
                ),
            );
            status
        }
    };
    let bundle = player
        .request(
            json!({"action":"getPackSettings","id":id}),
            COMMAND_TIMEOUT,
            None,
        )
        .and_then(reply_result)
        .and_then(|reply| {
            serde_json::from_value::<Bundle>(reply).map_err(|e| format!("壁纸配置格式错误：{e}"))
        })
        .and_then(|bundle| {
            if bundle.pack_id == id {
                Ok(bundle)
            } else {
                Err("播放器返回了其他壁纸的参数".into())
            }
        });
    match bundle {
        Ok(bundle) => Ok((status, bundle)),
        Err(error) => Err(SelectionFailure {
            error,
            observed: Some(status),
        }),
    }
}

#[cfg(not(windows))]
pub(crate) fn select_pack(_root: &Path, _id: &str) -> Result<(Value, Bundle), SelectionFailure> {
    Err("播放器通信仅支持 Windows".to_owned().into())
}

fn selection_status(
    id: &str,
    status: Value,
    before: Option<&Value>,
) -> Result<Value, SelectionFailure> {
    let Some(active) = status["packId"].as_str() else {
        return Err("无法确认壁纸切换结果，播放器未报告当前壁纸"
            .to_owned()
            .into());
    };
    if active != id {
        return Err(SelectionFailure {
            error: "壁纸切换未生效，已读取播放器当前选择".into(),
            observed: Some(status),
        });
    }
    if let Some(before) = before
        && before["packId"] == id
        && !status["importedRevision"].as_str().is_some_and(|revision| {
            !revision.is_empty() && before["importedRevision"].as_str() != Some(revision)
        })
    {
        return Err(SelectionFailure {
            error: "原播放器已在播放这款壁纸，无法确认本次切换".into(),
            observed: Some(status),
        });
    }
    Ok(status)
}

pub fn command(root: &Path, request: Value) -> Result<Value, String> {
    command_timeout(root, request, COMMAND_TIMEOUT)
}

pub(crate) fn command_timeout(
    root: &Path,
    mut request: Value,
    timeout: Duration,
) -> Result<Value, String> {
    let deadline = Instant::now() + timeout;
    let root = std::path::absolute(root).map_err(|error| error.to_string())?;
    let endpoint = installation_endpoint(&root)?;
    let path = root.to_str().ok_or("播放器运行目录无法编码")?;
    let fields = request.as_object_mut().ok_or("播放器请求必须是对象")?;
    if let Some(expected) = fields.get("expectedRuntimeRoot") {
        expect_runtime_root(expected, &endpoint)?;
    }
    fields.insert("expectedRuntimeRoot".into(), json!(path));
    let status_request = request["action"] == "status";
    let remaining = || deadline.saturating_duration_since(Instant::now());

    // Test tools can publish an explicit pipe for a --pipe player. Probe only
    // status before using that name, since the file can also be copied.
    if let Some(name) = explicit_endpoint(&root) {
        // Read-only identity discovery must also identify a copied foreign pipe;
        // a root-guarded probe would reject it before revealing its real root.
        let probe = json!({"action":"status"});
        if let Some((status, pid)) = explicit_status(
            exchange(&name, &probe, remaining(), pipe_client::request_timeout), &endpoint,
        )? {
            if status_request {
                if request.get("expectedPlayerPid").is_some_and(|expected| expected.as_u64() != Some(u64::from(pid))) {
                    return Err("播放器实例已变化".into());
                }
                return Ok(status);
            }
            return exchange(&name, &request, remaining(), |pipe, data, time| {
                pipe_client::request_timeout_for_pid(pipe, data, time, Some(pid))
            });
        }
    }
    let result = exchange(
        &endpoint,
        &request,
        remaining(),
        pipe_client::request_timeout,
    )?;
    if status_request {
        expect_runtime_root(&result["runtimeRoot"], &endpoint)?;
    }
    Ok(result)
}

fn explicit_endpoint(root: &Path) -> Option<String> {
    let stored = std::fs::read_to_string(root.join("data/player-pipe.txt")).ok()?;
    let name = stored.trim();
    let lower = name.to_ascii_lowercase();
    let generated = lower == "action75nativeplayer"
        || lower.starts_with("action75root-")
        || lower.strip_prefix("action75studio-").is_some_and(|suffix| {
            suffix.len() == 16 && suffix.bytes().all(|c| c.is_ascii_hexdigit())
        });
    (!generated && !name.is_empty()).then(|| name.to_owned())
}

fn explicit_status(reply: Result<Value, String>, endpoint: &str) -> Result<Option<(Value, u32)>, String> {
    let status = match reply {
        Err(error) if error == "播放器未启动" => return Ok(None),
        Err(error) => return Err(error),
        Ok(status) => status,
    };
    let root = status["runtimeRoot"].as_str().ok_or("显式播放器没有提供运行目录")?;
    let reported = installation_endpoint(Path::new(root))?;
    if reported != endpoint {
        // A copied installation may retain another installation's pipe file.
        // Only a positively identified foreign root is safe to ignore.
        return Ok(None);
    }
    let pid = status_pid(&status)?;
    Ok(Some((status, pid)))
}

fn status_pid(status: &Value) -> Result<u32, String> {
    status["playerPid"]
        .as_u64()
        .and_then(|pid| u32::try_from(pid).ok())
        .filter(|pid| *pid != 0)
        .ok_or_else(|| "播放器没有提供有效实例身份".into())
}

fn expect_runtime_root(value: &Value, endpoint: &str) -> Result<(), String> {
    if value
        .as_str()
        .and_then(|path| installation_endpoint(Path::new(path)).ok())
        .as_deref()
        == Some(endpoint)
    {
        Ok(())
    } else {
        Err("播放器运行目录与当前项目不一致".into())
    }
}

fn exchange(
    endpoint: &str,
    request: &Value,
    timeout: Duration,
    transport: impl FnOnce(&str, &Value, Duration) -> Result<Value, String>,
) -> Result<Value, String> {
    reply_result(transport(endpoint, request, timeout)?)
}

fn rejection(reply: &Value) -> String {
    reply["error"].as_str().unwrap_or("播放器请求失败").into()
}

fn reply_result(mut reply: Value) -> Result<Value, String> {
    if reply["ok"] == true {
        Ok(reply["result"].take())
    } else {
        Err(rejection(&reply))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn recovered_switch_requires_the_requested_selection_or_a_new_import_revision() {
        let other = json!({"packId":"old","importedRevision":""});
        let target = json!({"packId":"new","importedRevision":"first"});
        assert_eq!(
            selection_status("new", target.clone(), Some(&other)).ok(),
            Some(target.clone())
        );
        let failure = selection_status("new", other.clone(), Some(&other)).unwrap_err();
        assert_eq!(failure.observed, Some(other));
        let pending = selection_status("new", target.clone(), Some(&target)).unwrap_err();
        assert!(pending.error.contains("无法确认本次切换"));
        let updated = json!({"packId":"new","importedRevision":"second"});
        assert_eq!(
            selection_status("new", updated.clone(), Some(&target)).ok(),
            Some(updated)
        );
        assert!(selection_status("new", target, None).is_ok());
    }

    #[test]
    fn status_result_and_deadline_cross_the_transport_unchanged() {
        let deadline = Duration::from_millis(75);
        let result = exchange(
            "Action75Root-test",
            &json!({"action":"status"}),
            deadline,
            |pipe, request, timeout| {
                assert_eq!(pipe, "Action75Root-test");
                assert_eq!(request, &json!({"action":"status"}));
                assert_eq!(timeout, deadline);
                Ok(json!({"ok":true,"result":{"paused":false}}))
            },
        );
        assert_eq!(result.unwrap(), json!({"paused":false}));
    }

    #[test]
    fn host_rejection_and_transport_failure_remain_distinct() {
        let rejection = exchange("fixture", &json!({}), COMMAND_TIMEOUT, |_, _, _| {
            Ok(json!({"ok":false,"error":"stale revision"}))
        });
        let disconnected = exchange("fixture", &json!({}), COMMAND_TIMEOUT, |_, _, _| {
            Err("pipe unavailable".into())
        });
        assert_eq!(rejection.unwrap_err(), "stale revision");
        assert_eq!(disconnected.unwrap_err(), "pipe unavailable");
    }

    #[test]
    fn missing_error_keeps_legacy_fallback() {
        let result = exchange("fixture", &json!({}), COMMAND_TIMEOUT, |_, _, _| {
            Ok(json!({"ok":false}))
        });
        assert_eq!(result.unwrap_err(), "播放器请求失败");
    }

    #[test]
    fn explicit_probe_errors_cannot_be_hidden_by_an_absent_default_pipe() {
        for error in ["播放器权限不匹配（5）", "读取超时", "无效协议"] {
            assert_eq!(explicit_status(Err(error.into()), "unused-endpoint").unwrap_err(), error);
        }
        assert!(explicit_status(Err("播放器未启动".into()), "unused-endpoint").unwrap().is_none());
        assert!(explicit_status(Ok(json!({"playerPid":42})), "unused-endpoint").is_err());
    }

    #[cfg(windows)]
    #[test]
    fn copied_foreign_pipe_records_fall_back_but_invalid_local_identity_does_not() {
        let root = std::env::temp_dir().join(format!("action75-explicit-status-{}-{}", std::process::id(), std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
        std::fs::create_dir(&root).unwrap();
        std::fs::create_dir(root.join("other")).unwrap();
        let endpoint = installation_endpoint(&root).unwrap();
        let foreign = json!({"runtimeRoot":root.join("other"),"playerPid":42});
        assert!(explicit_status(Ok(foreign), &endpoint).unwrap().is_none());
        let local = json!({"runtimeRoot":root,"playerPid":42});
        assert_eq!(explicit_status(Ok(local), &endpoint).unwrap().unwrap().1, 42);
        assert!(explicit_status(Ok(json!({"runtimeRoot":root,"playerPid":0})), &endpoint).is_err());
        std::fs::remove_dir_all(root).unwrap();
    }
}
