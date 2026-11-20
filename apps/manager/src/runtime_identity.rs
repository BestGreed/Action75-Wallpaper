//! The installed runtime is named for its directory object on Windows.
use std::path::Path;

#[cfg(windows)]
pub fn installation_endpoint(root: &Path) -> Result<String, String> {
    use std::{
        fmt::Write,
        fs::OpenOptions,
        os::windows::{fs::OpenOptionsExt, io::AsRawHandle},
    };
    use windows::Win32::{
        Foundation::HANDLE,
        Storage::FileSystem::{
            BY_HANDLE_FILE_INFORMATION, FILE_FLAG_BACKUP_SEMANTICS, FILE_ID_INFO,
            FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, FileIdInfo,
            GetFileInformationByHandle, GetFileInformationByHandleEx,
        },
    };

    if !root.is_dir() {
        return Err("播放器运行目录无效".into());
    }
    let directory = OpenOptions::new()
        .access_mode(0)
        .share_mode((FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE).0)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS.0)
        .open(root)
        .map_err(|error| format!("无法打开播放器运行目录，{error}"))?;
    let handle = HANDLE(directory.as_raw_handle());
    let mut info = FILE_ID_INFO::default();
    if unsafe {
        GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            (&mut info as *mut FILE_ID_INFO).cast(),
            size_of::<FILE_ID_INFO>() as u32,
        )
    }
    .is_ok()
    {
        let mut name = format!("Action75Root-{:016x}-", info.VolumeSerialNumber);
        for byte in info.FileId.Identifier {
            write!(name, "{byte:02x}").expect("writing to a String is infallible");
        }
        return Ok(name);
    }
    let mut legacy = BY_HANDLE_FILE_INFORMATION::default();
    unsafe { GetFileInformationByHandle(handle, &mut legacy) }
        .map_err(|error| format!("无法识别播放器运行目录，{error}"))?;
    Ok(format!(
        "Action75Root-legacy-{:08x}-{:08x}{:08x}",
        legacy.dwVolumeSerialNumber, legacy.nFileIndexHigh, legacy.nFileIndexLow
    ))
}

#[cfg(not(windows))]
pub fn installation_endpoint(_root: &Path) -> Result<String, String> {
    Err("播放器通信仅支持 Windows".into())
}
