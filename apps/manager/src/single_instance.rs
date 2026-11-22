//! A second launch activates the existing control panel.
use action75_manager::runtime_identity::installation_endpoint;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{
    ffi::c_void,
    path::Path,
    sync::atomic::{AtomicBool, Ordering},
};
use windows::Win32::{
    Foundation::{HWND, LPARAM, LRESULT, WPARAM},
    UI::{
        Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass},
        WindowsAndMessaging::{WM_HOTKEY, WM_NCDESTROY},
    },
};
type Handle = isize;
#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateMutexW(attributes: *mut c_void, owner: i32, name: *const u16) -> Handle;
    fn CreateEventW(
        attributes: *mut c_void,
        manual_reset: i32,
        initial: i32,
        name: *const u16,
    ) -> Handle;
    fn WaitForSingleObject(handle: Handle, ms: u32) -> u32;
    fn GetLastError() -> u32;
    fn CloseHandle(handle: Handle) -> i32;
}
#[link(name = "user32")]
unsafe extern "system" {
    fn EnumWindows(callback: unsafe extern "system" fn(Handle, isize) -> i32, marker: isize)
    -> i32;
    fn SetPropW(window: Handle, name: *const u16, data: Handle) -> i32;
    fn GetPropW(window: Handle, name: *const u16) -> Handle;
    fn GetWindowThreadProcessId(window: Handle, pid: *mut u32) -> u32;
    fn IsWindowVisible(window: Handle) -> i32;
    fn RegisterHotKey(window: Handle, id: i32, modifiers: u32, key: u32) -> i32;
    fn UnregisterHotKey(window: Handle, id: i32) -> i32;
    fn RegisterWindowMessageW(text: *const u16) -> u32;
    fn PostMessageW(window: Handle, message: u32, wparam: usize, lparam: isize) -> i32;
    fn ShowWindowAsync(window: Handle, command: i32) -> i32;
    fn SetForegroundWindow(window: Handle) -> i32;
}
pub struct Instance {
    handle: Handle,
    exit_event: Handle,
    marker: Vec<u16>,
    player_marker: Vec<u16>,
}
impl Instance {
    pub fn mark_window(&self, window: &eframe::CreationContext) {
        let context = window.egui_ctx.clone();
        if let Ok(handle) = window.window_handle()
            && let RawWindowHandle::Win32(window) = handle.as_raw()
        {
            unsafe { SetPropW(window.hwnd.get(), self.marker.as_ptr(), 1) };
            ExitShortcut::attach(window.hwnd.get(), self.player_marker.clone(), context);
            if unsafe { WaitForSingleObject(self.exit_event, 0) } == 0 {
                FULL_EXIT.store(true, Ordering::Release);
            }
        }
    }
}
impl Drop for Instance {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.handle);
            CloseHandle(self.exit_event);
        }
    }
}

unsafe extern "system" fn activate_window(window: Handle, marker: isize) -> i32 {
    if unsafe { GetPropW(window, marker as *const u16) } == 0 {
        return 1;
    }
    unsafe {
        ShowWindowAsync(window, 9);
        SetForegroundWindow(window);
    }
    0
}

pub fn manager_marker(endpoint: &str) -> Vec<u16> {
    wide(&format!("{endpoint}-ManagerWindow"))
}

pub fn panel_ready(pid: u32, marker: &[u16]) -> bool {
    struct Search<'a> {
        pid: u32,
        marker: &'a [u16],
        found: bool,
    }
    unsafe extern "system" fn visit(window: Handle, data: isize) -> i32 {
        let search = unsafe { &mut *(data as *mut Search<'_>) };
        let mut pid = 0;
        unsafe { GetWindowThreadProcessId(window, &mut pid) };
        if pid == search.pid
            && unsafe { GetPropW(window, search.marker.as_ptr()) } != 0
            && unsafe { IsWindowVisible(window) } != 0
        {
            search.found = true;
            return 0;
        }
        1
    }
    let mut search = Search { pid, marker, found: false };
    unsafe { EnumWindows(visit, (&mut search as *mut Search<'_>) as isize) };
    search.found
}

pub fn acquire(root: &Path, administrator_handoff: bool) -> Result<Option<Instance>, String> {
    let endpoint = installation_endpoint(root)?;
    let name: Vec<u16> = format!("Local\\{endpoint}-Manager")
        .encode_utf16()
        .chain(Some(0))
        .collect();
    let marker = manager_marker(&endpoint);
    let handle = unsafe { CreateMutexW(std::ptr::null_mut(), 0, name.as_ptr()) };
    if handle == 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    if unsafe { GetLastError() } == 183 && !administrator_handoff {
        unsafe {
            CloseHandle(handle);
            EnumWindows(activate_window, marker.as_ptr() as isize);
        }
        return Ok(None);
    }
    let exit_event = unsafe {
        CreateEventW(
            std::ptr::null_mut(),
            1,
            0,
            wide(&format!(r"Local\{endpoint}-ManagerQuit")).as_ptr(),
        )
    };
    if exit_event == 0 {
        let error = std::io::Error::last_os_error();
        unsafe {
            CloseHandle(handle);
        }
        return Err(error.to_string());
    }
    let player_marker = wide(&format!("{endpoint}-PlayerWindow"));
    Ok(Some(Instance {
        handle,
        exit_event,
        marker,
        player_marker,
    }))
}

const EXIT_HOTKEY: i32 = 0x0A75;
static FULL_EXIT: AtomicBool = AtomicBool::new(false);
static OWN_SHORTCUT: AtomicBool = AtomicBool::new(false);

pub fn full_exit_requested() -> bool {
    FULL_EXIT.load(Ordering::Acquire)
}
pub fn owns_exit_shortcut() -> bool {
    OWN_SHORTCUT.load(Ordering::Acquire)
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

struct ExitShortcut {
    player_marker: Vec<u16>,
    quit: u32,
    claim: u32,
    ctx: eframe::egui::Context,
}

impl ExitShortcut {
    fn attach(window: Handle, player_marker: Vec<u16>, ctx: eframe::egui::Context) {
        let state = Box::new(Self {
            player_marker,
            quit: unsafe { RegisterWindowMessageW(wide("Action75.QuitSession").as_ptr()) },
            claim: unsafe { RegisterWindowMessageW(wide("Action75.ClaimExitShortcut").as_ptr()) },
            ctx,
        });
        let data = Box::into_raw(state);
        if unsafe {
            SetWindowSubclass(
                HWND(window as *mut _),
                Some(exit_proc),
                EXIT_HOTKEY as usize,
                data as usize,
            )
        }
        .as_bool()
        {
            Self::claim(window);
        } else {
            unsafe { drop(Box::from_raw(data)) };
        }
    }

    fn claim(window: Handle) {
        if !full_exit_requested() && !owns_exit_shortcut() {
            // MOD_NOREPEAT | MOD_SHIFT | MOD_CONTROL | MOD_ALT.
            OWN_SHORTCUT.store(
                unsafe { RegisterHotKey(window, EXIT_HOTKEY, 0x4007, u32::from(b'Q')) } != 0,
                Ordering::Release,
            );
        }
    }

    fn notify_player(&self, message: u32) {
        struct Notice<'a>(&'a [u16], u32);
        unsafe extern "system" fn notify(window: Handle, data: isize) -> i32 {
            let notice = unsafe { &*(data as *const Notice<'_>) };
            if unsafe { GetPropW(window, notice.0.as_ptr()) } != 0 {
                unsafe {
                    PostMessageW(window, notice.1, 0, 0);
                }
            }
            1
        }
        let notice = Notice(&self.player_marker, message);
        unsafe {
            EnumWindows(notify, &notice as *const _ as isize);
        }
    }
}

unsafe extern "system" fn exit_proc(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    id: usize,
    data: usize,
) -> LRESULT {
    let state = unsafe { &*(data as *const ExitShortcut) };
    if message == WM_HOTKEY && wparam.0 == EXIT_HOTKEY as usize
        || message != 0 && message == state.quit
    {
        if !FULL_EXIT.swap(true, Ordering::AcqRel) {
            if message == WM_HOTKEY {
                state.notify_player(state.quit);
            }
            state.ctx.request_repaint();
        }
        return LRESULT(0);
    }
    if message != 0 && message == state.claim {
        ExitShortcut::claim(window.0 as Handle);
        return LRESULT(0);
    }
    if message == WM_NCDESTROY {
        if OWN_SHORTCUT.swap(false, Ordering::AcqRel) {
            unsafe {
                UnregisterHotKey(window.0 as Handle, EXIT_HOTKEY);
            }
            if !full_exit_requested() {
                state.notify_player(state.claim);
            }
        }
        unsafe {
            let _ = RemoveWindowSubclass(window, Some(exit_proc), id);
            drop(Box::from_raw(data as *mut ExitShortcut));
        }
    }
    unsafe { DefSubclassProc(window, message, wparam, lparam) }
}
