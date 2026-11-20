use action75_manager::APP_NAME;
use serde_json::Value;
use std::fmt::Write;
use windows::Win32::{
    Foundation::HANDLE,
    System::{
        ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS},
        Threading::GetCurrentProcess,
    },
};

pub fn report(controller_renderer: &str, status: &Value, error: &str) -> String {
    let mut report = format!(
        "{APP_NAME} {}\n控制台 {controller_renderer}",
        env!("CARGO_PKG_VERSION")
    );
    if let Some(memory) = working_set_bytes(unsafe { GetCurrentProcess() }) {
        let memory = memory as f64 / (1024. * 1024.);
        let _ = write!(report, " · 工作集 {memory:.1} MiB");
    }
    if status["running"] == true {
        let renderer = status["renderer"].as_str().unwrap_or("D3D11");
        let _ = write!(report, "\n播放器 {renderer}");
        if let Some(adapter) = status["gpuAdapter"]
            .as_str()
            .filter(|name| !name.is_empty())
        {
            let _ = write!(report, "（{adapter}）");
        }
        if let Some(memory) = status["memoryMB"].as_f64() {
            let _ = write!(report, " · 工作集 {memory:.1} MiB");
        }
        if status["monitorConnected"] == true {
            report.push_str("\n键盘屏 已连接");
            if let (Some(width), Some(height)) = (
                status["outputWidth"].as_u64(),
                status["outputHeight"].as_u64(),
            ) {
                let _ = write!(report, " · {width}×{height}");
            }
            if let Some(fps) = status["fps"].as_u64() {
                let _ = write!(report, " · {fps} FPS");
            }
        } else {
            report.push_str("\n键盘屏 未连接");
        }
        if let Some(id) = status["packId"].as_str().filter(|id| !id.is_empty()) {
            let _ = write!(report, "\n壁纸 {id}");
        }
    } else {
        report.push_str("\n播放器 未连接");
    }
    if !error.is_empty() {
        let _ = write!(report, "\n最近错误 {error}");
    }
    report
}

pub fn working_set_bytes(process: HANDLE) -> Option<u64> {
    let size = size_of::<PROCESS_MEMORY_COUNTERS>() as u32;
    let mut counters = PROCESS_MEMORY_COUNTERS {
        cb: size,
        ..Default::default()
    };
    unsafe { GetProcessMemoryInfo(process, &mut counters, size) }.ok()?;
    Some(counters.WorkingSetSize as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn diagnostics_include_rendering_details_and_omit_internal_identifiers() {
        let status = json!({
            "running": true, "renderer": "D3D11/BlackHole", "gpuAdapter": "Test GPU",
            "monitorConnected": true, "outputWidth": 1920, "outputHeight": 720, "fps": 60,
            "memoryMB": 56.0, "runtimeRoot": "internal-path", "deviceGroup": "internal-device"
        });
        let text = report("DirectX 12", &status, "连接失败");
        for expected in [
            "控制台 DirectX 12",
            "D3D11/BlackHole（Test GPU）",
            "56.0 MiB",
            "1920×720",
            "60 FPS",
            "连接失败",
        ] {
            assert!(text.contains(expected), "{text}");
        }
        assert!(!text.contains("internal-"));
        assert!(report("OpenGL", &Value::Null, "").contains("播放器 未连接"));
    }
}
