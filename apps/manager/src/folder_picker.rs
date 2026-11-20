use eframe::egui;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{os::windows::ffi::OsStrExt, path::PathBuf, sync::mpsc, thread};
use windows::{
    Win32::{
        Foundation::{E_INVALIDARG, ERROR_CANCELLED, HWND},
        System::Com::{
            CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE,
            CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize,
        },
        UI::Shell::{
            FOS_FORCEFILESYSTEM, FOS_PATHMUSTEXIST, FOS_PICKFOLDERS, FileOpenDialog,
            IFileOpenDialog, IShellItem, SHCreateItemFromParsingName, SIGDN_FILESYSPATH,
        },
    },
    core::{Error, HRESULT, HSTRING, PCWSTR},
};

pub type Reply = Result<Option<String>, String>;

pub fn open(
    frame: &eframe::Frame,
    ctx: &egui::Context,
    initial: PathBuf,
    title: &'static str,
) -> Result<mpsc::Receiver<Reply>, String> {
    let Ok(RawWindowHandle::Win32(window)) = frame.window_handle().map(|handle| handle.as_raw())
    else {
        return Err("无法取得控制台窗口".into());
    };
    let owner = window.hwnd;
    let (send, receive) = mpsc::channel();
    let ctx = ctx.clone();
    // The native modal pumps messages on its own STA while the console and its IPC worker keep running.
    thread::Builder::new()
        .name("folder-picker".into())
        .spawn(move || {
            let result = pick(HWND(owner.get() as *mut _), initial, title)
                .map_err(|error| error.to_string());
            let _ = send.send(result);
            ctx.request_repaint();
        })
        .map_err(|error| error.to_string())?;
    Ok(receive)
}

struct Apartment;

impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

fn pick(owner: HWND, initial: PathBuf, title: &str) -> windows::core::Result<Option<String>> {
    // The dialog, its shell items, and the result allocation stay on this thread.
    unsafe {
        CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE).ok()?;
        let _apartment = Apartment;
        let dialog: IFileOpenDialog =
            CoCreateInstance(&FileOpenDialog, None, CLSCTX_INPROC_SERVER)?;
        dialog.SetOptions(
            dialog.GetOptions()? | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST,
        )?;
        dialog.SetTitle(&HSTRING::from(title))?;
        let wide: Vec<u16> = initial.as_os_str().encode_wide().chain(Some(0)).collect();
        if let Ok(folder) =
            SHCreateItemFromParsingName::<_, _, IShellItem>(PCWSTR(wide.as_ptr()), None)
        {
            let _ = dialog.SetFolder(&folder);
        }
        match dialog.Show(owner) {
            Err(error) if error.code() == HRESULT::from_win32(ERROR_CANCELLED.0) => {
                return Ok(None);
            }
            result => result?,
        }
        let selected = dialog.GetResult()?.GetDisplayName(SIGDN_FILESYSPATH)?;
        let path = selected.to_string();
        CoTaskMemFree(Some(selected.0.cast()));
        path.map(Some)
            .map_err(|error| Error::new(E_INVALIDARG, error.to_string()))
    }
}
