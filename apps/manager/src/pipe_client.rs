//! Local, bounded native-player IPC. No subprocess or reader threads per request.
use serde_json::Value;
use std::time::{Duration, Instant};

const MAX_MESSAGE: usize = 65_536;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(6);

#[cfg(all(windows, test))]
pub(crate) use windows::test_server;

fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 100
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')
}

pub fn request(pipe: &str, value: &Value) -> Result<Value, String> {
    request_timeout(pipe, value, REQUEST_TIMEOUT)
}

pub fn request_timeout(pipe: &str, value: &Value, timeout: Duration) -> Result<Value, String> {
    request_timeout_for_pid(pipe, value, timeout, None)
}

pub(crate) fn request_timeout_for_pid(
    pipe: &str,
    value: &Value,
    timeout: Duration,
    expected_pid: Option<u32>,
) -> Result<Value, String> {
    request_with_delivery(pipe, value, timeout, expected_pid, None)
}

pub(crate) fn request_with_delivery(
    pipe: &str,
    value: &Value,
    timeout: Duration,
    expected_pid: Option<u32>,
    attempted: Option<&mut bool>,
) -> Result<Value, String> {
    if !valid_name(pipe) {
        return Err("本地通信通道无效".into());
    }
    let mut bytes = serde_json::to_vec(value).map_err(|e| e.to_string())?;
    bytes.push(b'\n');
    // Match the native server's strict request bound, including the terminator.
    if bytes.len() >= MAX_MESSAGE {
        return Err("设置数据过大".into());
    }
    #[cfg(windows)]
    {
        windows::request(
            pipe,
            &bytes,
            Instant::now() + timeout,
            expected_pid,
            attempted,
        )
    }
    #[cfg(not(windows))]
    {
        let _ = (bytes, timeout, expected_pid, attempted);
        Err("播放器通信仅支持 Windows".into())
    }
}

#[cfg(windows)]
mod windows {
    use super::*;
    use std::{ffi::c_void, ptr, thread};

    type HandleValue = *mut c_void;
    const INVALID_HANDLE: HandleValue = -1isize as HandleValue;
    const IO_PENDING: u32 = 997;
    const PIPE_BUSY: u32 = 231;
    const FILE_NOT_FOUND: u32 = 2;
    const WAIT_OBJECT: u32 = 0;
    const WAIT_TIMEOUT: u32 = 258;

    #[repr(C)]
    struct Overlapped {
        internal: usize,
        internal_high: usize,
        offset: u32,
        offset_high: u32,
        event: HandleValue,
    }

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            security: *const c_void,
            disposition: u32,
            flags: u32,
            template: HandleValue,
        ) -> HandleValue;
        fn CloseHandle(handle: HandleValue) -> i32;
        fn GetLastError() -> u32;
        fn WaitNamedPipeW(name: *const u16, timeout: u32) -> i32;
        fn GetNamedPipeServerProcessId(pipe: HandleValue, pid: *mut u32) -> i32;
        fn CreateEventW(
            security: *const c_void,
            manual: i32,
            initial: i32,
            name: *const u16,
        ) -> HandleValue;
        fn ReadFile(
            handle: HandleValue,
            data: *mut c_void,
            size: u32,
            count: *mut u32,
            op: *mut Overlapped,
        ) -> i32;
        fn WriteFile(
            handle: HandleValue,
            data: *const c_void,
            size: u32,
            count: *mut u32,
            op: *mut Overlapped,
        ) -> i32;
        fn GetOverlappedResult(
            handle: HandleValue,
            op: *mut Overlapped,
            count: *mut u32,
            wait: i32,
        ) -> i32;
        fn CancelIoEx(handle: HandleValue, op: *mut Overlapped) -> i32;
        fn WaitForSingleObject(handle: HandleValue, timeout: u32) -> u32;
    }

    struct Handle(HandleValue);
    impl Drop for Handle {
        fn drop(&mut self) {
            if !self.0.is_null() && self.0 != INVALID_HANDLE {
                unsafe {
                    CloseHandle(self.0);
                }
            }
        }
    }

    fn remaining(deadline: Instant) -> Result<u32, String> {
        let duration = deadline.saturating_duration_since(Instant::now());
        if duration.is_zero() {
            return Err("播放器响应超时".into());
        }
        Ok(duration.as_millis().max(1).min(u32::MAX as u128) as u32)
    }

    fn connect(name: &[u16], deadline: Instant) -> Result<Handle, String> {
        let mut absent_since = None;
        loop {
            remaining(deadline)?;
            let handle = unsafe {
                CreateFileW(
                    name.as_ptr(),
                    0xC000_0000,
                    0,
                    ptr::null(),
                    3,
                    0x4000_0000,
                    ptr::null_mut(),
                )
            };
            if handle != INVALID_HANDLE {
                return Ok(Handle(handle));
            }
            let error = unsafe { GetLastError() };
            if error != PIPE_BUSY && error != FILE_NOT_FOUND {
                return Err(if error == 5 {
                    "播放器权限不匹配（5）：请重新连接管理员播放器。".into()
                } else {
                    format!("无法联系播放器（{error}）")
                });
            }
            if error == FILE_NOT_FOUND {
                let since = absent_since.get_or_insert_with(Instant::now);
                // A stopped player should become "offline" promptly, while a
                // short server re-creation gap is retried rather than exposed.
                if since.elapsed() >= Duration::from_millis(200) {
                    return Err("播放器未启动".into());
                }
            } else {
                absent_since = None;
            }
            let wait = remaining(deadline)?.min(50);
            if unsafe { WaitNamedPipeW(name.as_ptr(), wait) } == 0
                && unsafe { GetLastError() } == FILE_NOT_FOUND
            {
                thread::sleep(Duration::from_millis(u64::from(wait.min(10))));
            }
        }
    }

    struct Operation {
        event: Handle,
        overlapped: Overlapped,
        bytes: Vec<u8>,
    }

    // The OVERLAPPED and buffer are heap-owned because a timed-out OS request may
    // still be completing cancellation while the caller is leaving this function.
    fn transfer(
        pipe: &Handle,
        write: bool,
        bytes: Vec<u8>,
        deadline: Instant,
    ) -> Result<(Vec<u8>, usize), String> {
        let timeout = remaining(deadline)?;
        let event = Handle(unsafe { CreateEventW(ptr::null(), 1, 0, ptr::null()) });
        if event.0.is_null() {
            return Err("无法创建通信事件".into());
        }
        let mut pending = Box::new(Operation {
            overlapped: Overlapped {
                internal: 0,
                internal_high: 0,
                offset: 0,
                offset_high: 0,
                event: event.0,
            },
            event,
            bytes,
        });
        let mut count = 0u32;
        let ok = unsafe {
            if write {
                WriteFile(
                    pipe.0,
                    pending.bytes.as_ptr().cast(),
                    pending.bytes.len() as u32,
                    &mut count,
                    &mut pending.overlapped,
                )
            } else {
                ReadFile(
                    pipe.0,
                    pending.bytes.as_mut_ptr().cast(),
                    pending.bytes.len() as u32,
                    &mut count,
                    &mut pending.overlapped,
                )
            }
        };
        if ok == 0 {
            let error = unsafe { GetLastError() };
            if error != IO_PENDING {
                return Err(format!("播放器通信中断（{error}）"));
            }
            let wait = unsafe { WaitForSingleObject(pending.event.0, timeout) };
            if wait != WAIT_OBJECT {
                unsafe {
                    CancelIoEx(pipe.0, &mut pending.overlapped);
                }
                // Named-pipe cancellation normally completes immediately. Keep a
                // finite grace period: a faulty driver must not freeze this worker.
                let cancelled = unsafe { WaitForSingleObject(pending.event.0, 1000) };
                if cancelled != WAIT_OBJECT {
                    // Safety last resort: the kernel still owns these addresses.
                    // Retain them rather than freeing an in-flight I/O buffer.
                    // The request's pipe handle is closed as its caller unwinds.
                    std::mem::forget(pending);
                }
                return Err(if wait == WAIT_TIMEOUT {
                    "播放器响应超时"
                } else {
                    "播放器通信等待失败"
                }
                .into());
            }
            if unsafe { GetOverlappedResult(pipe.0, &mut pending.overlapped, &mut count, 0) } == 0 {
                return Err(format!("播放器通信中断（{}）", unsafe {
                    GetLastError()
                }));
            }
        }
        if count == 0 {
            return Err("播放器已关闭通信".into());
        }
        if write {
            Ok((Vec::new(), count as usize))
        } else {
            pending.bytes.truncate(count as usize);
            Ok((std::mem::take(&mut pending.bytes), count as usize))
        }
    }

    pub(super) fn request(
        pipe_name: &str,
        bytes: &[u8],
        deadline: Instant,
        expected_pid: Option<u32>,
        attempted: Option<&mut bool>,
    ) -> Result<Value, String> {
        let name: Vec<u16> = format!(r"\\.\pipe\{pipe_name}")
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let pipe = connect(&name, deadline)?;
        if let Some(expected) = expected_pid {
            let mut pid = 0;
            if unsafe { GetNamedPipeServerProcessId(pipe.0, &mut pid) } == 0 || pid != expected {
                return Err("播放器身份已变更，请重试".into());
            }
        }
        if let Some(attempted) = attempted {
            // Cancellation can race completion of the write. From here onward
            // a missing reply does not prove that the player did not act.
            *attempted = true;
        }
        let mut offset = 0;
        while offset < bytes.len() {
            offset += transfer(&pipe, true, bytes[offset..].to_vec(), deadline)?.1;
        }
        let mut reply = Vec::with_capacity(4096);
        loop {
            let room = MAX_MESSAGE.saturating_sub(reply.len());
            if room == 0 {
                return Err("播放器响应过大".into());
            }
            let chunk = transfer(&pipe, false, vec![0; room.min(4096)], deadline)?.0;
            reply.extend_from_slice(&chunk);
            if let Some(newline) = reply.iter().position(|b| *b == b'\n') {
                if newline >= MAX_MESSAGE {
                    return Err("播放器响应过大".into());
                }
                // Acknowledging the complete message prevents server disconnect
                // from discarding unread output. This is not another command.
                let _ = transfer(&pipe, true, vec![b'\n'], deadline);
                return serde_json::from_slice(&reply[..newline])
                    .map_err(|_| "播放器返回了无效数据".into());
            }
        }
    }

    #[cfg(test)]
    pub(crate) mod test_server {
        use super::*;
        use std::sync::{
            atomic::{AtomicU32, Ordering},
            mpsc,
        };
        static NEXT: AtomicU32 = AtomicU32::new(1);
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn CreateNamedPipeW(
                name: *const u16,
                open: u32,
                mode: u32,
                instances: u32,
                out_size: u32,
                in_size: u32,
                timeout: u32,
                security: *const c_void,
            ) -> HandleValue;
            fn ConnectNamedPipe(pipe: HandleValue, op: *mut Overlapped) -> i32;
        }
        pub fn spawn(
            reply: Vec<Vec<u8>>,
            delay: Duration,
        ) -> (String, thread::JoinHandle<Vec<u8>>) {
            let name = format!(
                "Action75RustPipeTest-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            );
            let path: Vec<u16> = format!(r"\\.\pipe\{name}")
                .encode_utf16()
                .chain(Some(0))
                .collect();
            let (ready, receiver) = mpsc::channel();
            let handle = thread::spawn(move || {
                let pipe = Handle(unsafe {
                    CreateNamedPipeW(path.as_ptr(), 3, 8, 1, 65536, 65536, 0, ptr::null())
                });
                assert!(pipe.0 != INVALID_HANDLE);
                ready.send(()).unwrap();
                let connected = unsafe { ConnectNamedPipe(pipe.0, ptr::null_mut()) };
                assert!(connected != 0 || unsafe { GetLastError() } == 535);
                let mut request = Vec::new();
                loop {
                    let mut b = [0u8; 256];
                    let mut count = 0;
                    if unsafe {
                        ReadFile(
                            pipe.0,
                            b.as_mut_ptr().cast(),
                            b.len() as u32,
                            &mut count,
                            ptr::null_mut(),
                        )
                    } == 0
                        || count == 0
                    {
                        break;
                    }
                    request.extend_from_slice(&b[..count as usize]);
                    if request.contains(&b'\n') {
                        break;
                    }
                }
                thread::sleep(delay);
                for chunk in reply {
                    let mut sent = 0;
                    if unsafe {
                        WriteFile(
                            pipe.0,
                            chunk.as_ptr().cast(),
                            chunk.len() as u32,
                            &mut sent,
                            ptr::null_mut(),
                        )
                    } == 0
                    {
                        return request;
                    }
                }
                let mut ack = [0u8; 1];
                let mut count = 0;
                unsafe {
                    ReadFile(
                        pipe.0,
                        ack.as_mut_ptr().cast(),
                        1,
                        &mut count,
                        ptr::null_mut(),
                    );
                }
                if count != 0 {
                    assert_eq!(ack[0], b'\n');
                }
                request
            });
            receiver.recv_timeout(Duration::from_secs(2)).unwrap();
            (name, handle)
        }

        /// Serve a finite script on a caller-owned test endpoint. A missing
        /// response models disconnect after delivery, before acknowledgement.
        pub fn spawn_script(
            name: String,
            replies: Vec<Option<Value>>,
            mut received: impl FnMut(&Value) + Send + 'static,
        ) -> thread::JoinHandle<Vec<Value>> {
            let (ready, receiver) = mpsc::channel();
            let handle = thread::spawn(move || {
                let path: Vec<u16> = format!(r"\\.\pipe\{name}")
                    .encode_utf16()
                    .chain(Some(0))
                    .collect();
                let mut requests = Vec::new();
                for (index, reply) in replies.into_iter().enumerate() {
                    let pipe = Handle(unsafe {
                        CreateNamedPipeW(path.as_ptr(), 3, 8, 1, 65536, 65536, 0, ptr::null())
                    });
                    assert!(pipe.0 != INVALID_HANDLE);
                    if index == 0 {
                        ready.send(()).unwrap();
                    }
                    let connected = unsafe { ConnectNamedPipe(pipe.0, ptr::null_mut()) };
                    assert!(connected != 0 || unsafe { GetLastError() } == 535);
                    let mut request = Vec::new();
                    loop {
                        let mut bytes = [0u8; 256];
                        let mut count = 0;
                        let read = unsafe {
                            ReadFile(
                                pipe.0,
                                bytes.as_mut_ptr().cast(),
                                bytes.len() as u32,
                                &mut count,
                                ptr::null_mut(),
                            )
                        };
                        assert!(read != 0 && count != 0);
                        request.extend_from_slice(&bytes[..count as usize]);
                        if request.contains(&b'\n') {
                            break;
                        }
                    }
                    let request = serde_json::from_slice(&request).unwrap();
                    received(&request);
                    requests.push(request);
                    if let Some(reply) = reply {
                        let mut bytes = serde_json::to_vec(&reply).unwrap();
                        bytes.push(b'\n');
                        let mut sent = 0;
                        assert_ne!(
                            unsafe {
                                WriteFile(
                                    pipe.0,
                                    bytes.as_ptr().cast(),
                                    bytes.len() as u32,
                                    &mut sent,
                                    ptr::null_mut(),
                                )
                            },
                            0
                        );
                        assert_eq!(sent as usize, bytes.len());
                        let mut ack = [0u8; 1];
                        let mut count = 0;
                        assert_ne!(
                            unsafe {
                                ReadFile(
                                    pipe.0,
                                    ack.as_mut_ptr().cast(),
                                    1,
                                    &mut count,
                                    ptr::null_mut(),
                                )
                            },
                            0
                        );
                        assert_eq!(&ack, b"\n");
                    }
                }
                requests
            });
            receiver.recv_timeout(Duration::from_secs(2)).unwrap();
            handle
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_nonlocal_pipe_names_and_oversize_commands() {
        assert!(request("../bad", &serde_json::json!({})).is_err());
        assert!(request("", &serde_json::json!({})).is_err());
        assert!(request("Valid", &serde_json::json!({"x":"x".repeat(MAX_MESSAGE)})).is_err());
    }
    #[cfg(windows)]
    #[test]
    fn reads_fragmented_utf8_reply_and_acknowledges() {
        let line = serde_json::to_vec(
            &serde_json::json!({"ok":true,"result":{"name":"水彩","long":"a".repeat(12000)}}),
        )
        .unwrap();
        let mut framed = line.clone();
        framed.push(b'\n');
        let pieces = framed.chunks(3).map(|p| p.to_vec()).collect();
        let (pipe, server) = windows::test_server::spawn(pieces, Duration::ZERO);
        let result = request(&pipe, &serde_json::json!({"action":"status"})).unwrap();
        assert_eq!(result["result"]["name"], "水彩");
        assert_eq!(result["result"]["long"].as_str().unwrap().len(), 12000);
        let sent = server.join().unwrap();
        assert_eq!(
            serde_json::from_slice::<Value>(&sent).unwrap()["action"],
            "status"
        );
    }
    #[cfg(windows)]
    #[test]
    fn bounds_no_reply_and_missing_server_waits() {
        let (pipe, server) = windows::test_server::spawn(vec![], Duration::from_millis(160));
        let start = Instant::now();
        assert!(request_timeout(&pipe, &serde_json::json!({}), Duration::from_millis(25)).is_err());
        assert!(start.elapsed() < Duration::from_millis(500));
        server.join().unwrap();
        let start = Instant::now();
        assert!(
            request_timeout(
                "Action75DefinitelyMissingTest",
                &serde_json::json!({}),
                Duration::from_millis(20)
            )
            .is_err()
        );
        assert!(start.elapsed() < Duration::from_millis(500));
    }
    #[cfg(windows)]
    #[test]
    fn rejects_malformed_or_oversize_responses() {
        for bytes in [b"not-json\n".to_vec(), vec![b'x'; MAX_MESSAGE]] {
            let (pipe, server) = windows::test_server::spawn(vec![bytes], Duration::ZERO);
            assert!(request(&pipe, &serde_json::json!({})).is_err());
            server.join().unwrap();
        }
    }
}
