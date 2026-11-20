//! Route commands to the player for this runtime directory.
use crate::{pipe_client, runtime_identity::installation_endpoint};
use serde_json::{Value, json};
use std::{
    path::Path,
    time::{Duration, Instant},
};

const COMMAND_TIMEOUT: Duration = Duration::from_secs(6);

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
        let probe = json!({"action":"status","expectedRuntimeRoot":path});
        let probe = if status_request { &request } else { &probe };
        if let Ok(status) = exchange(&name, probe, remaining(), pipe_client::request_timeout)
            && expect_runtime_root(&status["runtimeRoot"], &endpoint).is_ok()
            && let Some(pid) = status["playerPid"]
                .as_u64()
                .and_then(|pid| u32::try_from(pid).ok())
            && pid != 0
        {
            if status_request {
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
    let reply = transport(endpoint, request, timeout)?;
    if reply["ok"] == true {
        Ok(reply["result"].clone())
    } else {
        Err(reply["error"].as_str().unwrap_or("播放器请求失败").into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

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
}
