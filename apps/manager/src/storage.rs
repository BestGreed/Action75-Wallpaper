use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum InterfaceTheme {
    Graphite,
    #[default]
    #[serde(other)]
    Paper,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum GpuPreference {
    Integrated,
    #[default]
    #[serde(other)]
    Discrete,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct InterfacePreferences {
    pub wallpaper_order: Vec<String>,
    pub panel_open: Option<bool>,
    pub panel_width: Option<f32>,
    pub theme: InterfaceTheme,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Preferences {
    pub wallpaper_dir: String,
    pub log_dir: String,
    pub setup_complete: bool,
    pub gpu_preference: GpuPreference,
    #[serde(flatten)]
    pub interface: InterfacePreferences,
}
impl Preferences {
    pub fn load(root: &Path) -> Self {
        fs::read(root.join("data/app-settings.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default()
    }
    pub fn wallpapers(&self, root: &Path) -> PathBuf {
        if self.wallpaper_dir.is_empty() {
            root.join("wallpapers-native")
        } else {
            PathBuf::from(&self.wallpaper_dir)
        }
    }
    pub fn logs(&self, root: &Path) -> PathBuf {
        if self.log_dir.is_empty() {
            root.join("logs")
        } else {
            PathBuf::from(&self.log_dir)
        }
    }
    pub fn save(&self, root: &Path) -> Result<(), String> {
        let path = root.join("data/app-settings.json");
        fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
        let temp = path.with_extension("tmp");
        fs::write(
            &temp,
            serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?,
        )
        .map_err(|e| e.to_string())?;
        crate::atomic_replace(&temp, &path)
    }
}

#[cfg(test)]
#[test]
fn legacy_preferences_keep_their_paths_and_panel_state_when_the_theme_is_saved() {
    let mut preferences: Preferences = serde_json::from_str(
        r#"{"wallpaperDir":"custom-packs","setupComplete":true,"panelOpen":false}"#,
    )
    .unwrap();
    assert_eq!(preferences.interface.theme, InterfaceTheme::Paper);
    assert_eq!(preferences.gpu_preference, GpuPreference::Discrete);
    preferences.interface.theme = InterfaceTheme::Graphite;
    preferences.interface.panel_width = Some(316.);
    let saved = serde_json::to_value(&preferences).unwrap();
    assert_eq!(saved["wallpaperDir"], "custom-packs");
    assert_eq!(saved["setupComplete"], true);
    assert_eq!(saved["panelOpen"], false);
    assert_eq!(saved["panelWidth"], 316.);
    assert_eq!(saved["theme"], "graphite");
    assert_eq!(saved["gpuPreference"], "discrete");
}

pub fn bootstrap(root: &Path) -> Result<(), String> {
    let data = root.join("data");
    fs::create_dir_all(&data).map_err(|e| e.to_string())?;
    let calibration = data.join("fm-calibration.json");
    if !calibration.exists() {
        fs::copy(
            root.join("assets/defaults/fm-calibration.json"),
            &calibration,
        )
        .map_err(|e| e.to_string())?;
    }
    let state = data.join("native-player.json");
    if !state.exists() {
        fs::write(state,br#"{"version":1,"fps":60,"packId":"","monitorDevice":"","visible":false,"deviceGroup":"VID_1CA6&PID_1A0B"}"#).map_err(|e|e.to_string())?;
    }
    Ok(())
}

/// Low-frequency event logging only: never called from input or render loops.
pub fn log(root: &Path, level: &str, message: &str) {
    let result = (|| -> std::io::Result<()> {
        let dir = Preferences::load(root).logs(root);
        fs::create_dir_all(&dir)?;
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        let record = format!(
            "{}.{:03} {level} {}\n",
            now.as_secs(),
            now.subsec_millis(),
            message.replace(['\n', '\r'], " ")
        );
        let file = dir.join("manager.log");
        match fs::symlink_metadata(&file) {
            Ok(metadata) if !plain_file(&metadata) => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "Log path is not a regular file",
                ));
            }
            Err(error) if error.kind() != io::ErrorKind::NotFound => return Err(error),
            _ => {}
        }
        if fs::metadata(&file)
            .map(|m| m.len() + record.len() as u64 > 10 * 1024 * 1024)
            .unwrap_or(false)
        {
            fs::rename(&file, dir.join(format!("manager-{}.log", now.as_millis())))?;
        }
        let mut f = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(file)?;
        f.write_all(record.as_bytes())?;
        prune_logs(&dir)
    })();
    if let Err(e) = result {
        eprintln!("Log: {e}");
    }
}
// Shared by retention and migration: a user-selected directory need not be ours.
fn owned_log_name(path: &Path) -> bool {
    let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
        return false;
    };
    if matches!(name, "manager.log" | "player.log") {
        return true;
    }
    name.strip_suffix(".log")
        .and_then(|stem| {
            stem.strip_prefix("manager-")
                .or_else(|| stem.strip_prefix("player-"))
        })
        .is_some_and(|stamp| !stamp.is_empty() && stamp.bytes().all(|b| b.is_ascii_digit()))
}

fn plain_file(metadata: &fs::Metadata) -> bool {
    if !metadata.is_file() {
        return false;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        // Also exclude non-symlink reparse points, e.g. cloud placeholders.
        if metadata.file_attributes() & 0x400 != 0 {
            return false;
        }
    }
    true
}

fn owned_log_file(path: &Path) -> io::Result<bool> {
    Ok(owned_log_name(path) && plain_file(&fs::symlink_metadata(path)?))
}

pub fn prune_logs(dir: &Path) -> std::io::Result<()> {
    let mut files = Vec::new();
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if owned_log_file(&path)? {
            files.push(path);
        }
    }
    files.sort_by_key(|p| fs::metadata(p).and_then(|m| m.modified()).ok());
    let count = files.len().saturating_sub(10);
    for p in files
        .into_iter()
        .filter(|p| {
            !matches!(
                p.file_name().and_then(|s| s.to_str()),
                Some("manager.log" | "player.log")
            )
        })
        .take(count)
    {
        fs::remove_file(p)?;
    }
    Ok(())
}
fn same_file(a: &Path, b: &Path) -> Result<bool, String> {
    if fs::metadata(a).map_err(|e| e.to_string())?.len()
        != fs::metadata(b).map_err(|e| e.to_string())?.len()
    {
        return Ok(false);
    }
    let mut a = fs::File::open(a).map_err(|e| e.to_string())?;
    let mut b = fs::File::open(b).map_err(|e| e.to_string())?;
    same_contents(&mut a, &mut b).map_err(|e| e.to_string())
}

pub(crate) fn same_contents(a: &mut impl Read, b: &mut impl Read) -> io::Result<bool> {
    let mut x = [0u8; 65536];
    let mut y = [0u8; 65536];
    loop {
        let n = a.read(&mut x)?;
        let m = b.read(&mut y)?;
        if n != m || x[..n] != y[..m] {
            return Ok(false);
        }
        if n == 0 {
            return Ok(true);
        }
    }
}
fn files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), String> {
    if !dir.exists() {
        return Ok(());
    }
    for e in fs::read_dir(dir).map_err(|e| e.to_string())? {
        let e = e.map_err(|e| e.to_string())?;
        let ty = e.file_type().map_err(|e| e.to_string())?;
        if ty.is_symlink() {
            return Err("迁移目录中包含链接，请选择普通文件夹".into());
        }
        if ty.is_dir() {
            files(&e.path(), out)?
        } else {
            out.push(e.path())
        }
    }
    Ok(())
}

// Only destinations receiving new bytes enter this check. Different directories
// (including mount aliases) on one volume share a single capacity budget.
fn check_migration_space(
    allocations: &[(PathBuf, u64)],
    mut probe: impl FnMut(&Path) -> Result<(String, u64), String>,
) -> Result<(), String> {
    let mut volumes = std::collections::HashMap::<String, (u64, u64)>::new();
    for (target, bytes) in allocations {
        if *bytes == 0 {
            continue;
        }
        let (identity, available) = probe(target)?;
        let (required, free) = volumes.entry(identity).or_insert((0, available));
        *required = required
            .checked_add(*bytes)
            .ok_or("迁移文件大小超出支持范围")?;
        *free = (*free).min(available);
    }
    if volumes.values().any(|(required, free)| required > free) {
        return Err("目标磁盘空间不足".into());
    }
    Ok(())
}

fn allocation_directory(destination: &Path) -> Result<PathBuf, String> {
    let mut directory = destination.parent().ok_or("目标文件没有父目录")?;
    loop {
        match fs::metadata(directory) {
            Ok(metadata) if metadata.is_dir() => {
                return fs::canonicalize(directory).map_err(|e| e.to_string());
            }
            Ok(_) => return Err("目标文件的父路径不是文件夹".into()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                directory = directory.parent().ok_or("目标目录不存在")?;
            }
            Err(error) => return Err(error.to_string()),
        }
    }
}

#[cfg(windows)]
fn volume_space(target: &Path) -> Result<(String, u64), String> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetDiskFreeSpaceExW(
            path: *const u16,
            available: *mut u64,
            total: *mut u64,
            free: *mut u64,
        ) -> i32;
        fn GetVolumePathNameW(path: *const u16, volume: *mut u16, capacity: u32) -> i32;
        fn GetVolumeNameForVolumeMountPointW(
            mount: *const u16,
            name: *mut u16,
            capacity: u32,
        ) -> i32;
        fn GetDriveTypeW(root: *const u16) -> u32;
    }
    let path: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut mount = vec![0u16; 32768];
    if unsafe { GetVolumePathNameW(path.as_ptr(), mount.as_mut_ptr(), mount.len() as u32) } == 0 {
        return Err(io::Error::last_os_error().to_string());
    }
    let mut name = [0u16; 50];
    let identity = if unsafe {
        GetVolumeNameForVolumeMountPointW(mount.as_ptr(), name.as_mut_ptr(), name.len() as u32)
    } != 0
    {
        &name[..]
    } else {
        let error = io::Error::last_os_error();
        // SMB shares have no volume GUID. Their canonical share root provides
        // the capacity identity; local failures must not silently split a volume.
        if unsafe { GetDriveTypeW(mount.as_ptr()) } != 4 {
            return Err(error.to_string());
        }
        &mount[..]
    };
    let end = identity
        .iter()
        .position(|v| *v == 0)
        .unwrap_or(identity.len());
    let identity = String::from_utf16_lossy(&identity[..end]).to_lowercase();
    let mut available = 0;
    if unsafe {
        GetDiskFreeSpaceExW(
            path.as_ptr(),
            &mut available,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error().to_string());
    }
    Ok((identity, available))
}

/// Copy and verify before activation; only remove source files after an ACK.
pub fn migrate(
    root: &Path,
    next: &Preferences,
    activate: impl FnOnce() -> Result<(), String>,
) -> Result<(), String> {
    migrate_with_space_check(root, next, activate, |allocations| {
        #[cfg(windows)]
        check_migration_space(allocations, volume_space)?;
        #[cfg(not(windows))]
        let _ = allocations;
        Ok(())
    })
}

fn migrate_with_space_check(
    root: &Path,
    next: &Preferences,
    activate: impl FnOnce() -> Result<(), String>,
    check_space: impl FnOnce(&[(PathBuf, u64)]) -> Result<(), String>,
) -> Result<(), String> {
    let old = Preferences::load(root);
    let mut plan = Vec::new();
    let mut allocations = std::collections::BTreeMap::<PathBuf, u64>::new();
    let mut created = Vec::new();
    let result = (|| {
        let destinations = [next.wallpapers(root), next.logs(root)];
        for target in &destinations {
            if !target.is_absolute() {
                return Err("请选择绝对目录".into());
            }
            fs::create_dir_all(target).map_err(|e| e.to_string())?;
        }
        let a = fs::canonicalize(&destinations[0]).map_err(|e| e.to_string())?;
        let b = fs::canonicalize(&destinations[1]).map_err(|e| e.to_string())?;
        if a.starts_with(&b) || b.starts_with(&a) {
            return Err("壁纸和日志需要独立目录".into());
        }
        for (kind, (source, target)) in [
            (old.wallpapers(root), next.wallpapers(root)),
            (old.logs(root), next.logs(root)),
        ]
        .into_iter()
        .enumerate()
        {
            fs::create_dir_all(&source).map_err(|e| e.to_string())?;
            let source = fs::canonicalize(source).map_err(|e| e.to_string())?;
            let target = fs::canonicalize(target).map_err(|e| e.to_string())?;
            if source == target {
                continue;
            }
            if source.starts_with(&target) || target.starts_with(&source) {
                return Err("新旧目录不能互相包含".into());
            }
            let stamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos();
            let probe = target.join(format!(".action75-write-{}-{stamp}", std::process::id()));
            let probe_file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&probe)
                .map_err(|e| e.to_string())?;
            drop(probe_file);
            fs::remove_file(probe).map_err(|e| e.to_string())?;
            let mut list = Vec::new();
            for entry in fs::read_dir(&source).map_err(|e| e.to_string())? {
                let entry = entry.map_err(|e| e.to_string())?;
                let path = entry.path();
                if kind == 0 {
                    if entry.file_type().map_err(|e| e.to_string())?.is_dir()
                        && path.join("manifest.json").is_file()
                    {
                        files(&path, &mut list)?;
                    }
                } else if owned_log_file(&path).map_err(|e| e.to_string())? {
                    list.push(path);
                }
            }
            for file in list {
                let dest = target.join(file.strip_prefix(&source).unwrap());
                if dest.exists() {
                    if !same_file(&file, &dest)? {
                        return Err(format!("文件冲突：{}", dest.display()));
                    }
                } else {
                    let size = fs::metadata(&file).map_err(|e| e.to_string())?.len();
                    if size != 0 {
                        // Existing destination subdirectories can be mounted on
                        // another volume; charge the closest existing parent.
                        let bytes = allocations.entry(allocation_directory(&dest)?).or_default();
                        *bytes = bytes.checked_add(size).ok_or("迁移文件大小超出支持范围")?;
                    }
                }
                plan.push((file, dest));
            }
        }
        check_space(&allocations.into_iter().collect::<Vec<_>>())?;
        for (src, dst) in &plan {
            if dst.exists() {
                continue;
            }
            fs::create_dir_all(dst.parent().unwrap()).map_err(|e| e.to_string())?;
            let mut output = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(dst)
                .map_err(|e| e.to_string())?;
            created.push(dst.clone());
            let mut input = fs::File::open(src).map_err(|e| e.to_string())?;
            std::io::copy(&mut input, &mut output).map_err(|e| e.to_string())?;
            output.sync_all().map_err(|e| e.to_string())?;
            drop(output);
            if !same_file(src, dst)? {
                return Err(format!("校验失败：{}", src.display()));
            }
        }
        next.save(root)?;
        if let Err(error) = activate() {
            old.save(root)?;
            return Err(error);
        }
        Ok(())
    })();
    if result.is_err() {
        for p in created {
            let _ = fs::remove_file(p);
        }
        return result;
    }
    for (src, dst) in plan {
        if same_file(&src, &dst) != Ok(true) {
            log(
                root,
                "WARN",
                &format!("旧文件发生变化，予以保留：{}", src.display()),
            );
            continue;
        }
        if let Err(e) = fs::remove_file(&src) {
            log(root, "WARN", &format!("旧文件保留 {}: {e}", src.display()));
        }
    }
    log(root, "INFO", "目录迁移完成");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture_root(label: &str) -> PathBuf {
        let root = std::env::temp_dir().join(format!(
            "action75-{label}-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&root).unwrap();
        root
    }

    const UNRELATED_LOGS: &[&str] = &[
        "manager-sync.log",
        "player-backup.log",
        "managerial.log",
        "players.log",
        "manager-.log",
        "player--123.log",
        "manager-123x.log",
        "player-1.5.log",
        "manager-123.log.bak",
        "player-１２３.log",
    ];

    #[test]
    fn log_ownership_requires_exact_active_names_or_numeric_rotation_stamps() {
        for name in [
            "manager.log",
            "player.log",
            "manager-0.log",
            "player-1731845500123.log",
        ] {
            assert!(owned_log_name(Path::new(name)), "{name}");
        }
        for name in UNRELATED_LOGS {
            assert!(!owned_log_name(Path::new(name)), "{name}");
        }
    }

    #[test]
    fn migration_conflict_and_failed_ack_preserve_sources_and_preferences() {
        let root = std::env::temp_dir().join(format!(
            "action75-migrate-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(root.join("wallpapers-native/a")).unwrap();
        fs::create_dir_all(root.join("logs")).unwrap();
        fs::write(root.join("logs/manager.log"), b"existing log\n").unwrap();
        fs::write(root.join("logs/other-app.log"), b"unrelated").unwrap();
        for name in UNRELATED_LOGS {
            fs::write(root.join("logs").join(name), b"unrelated log").unwrap();
        }
        fs::write(root.join("logs/player-1731845500123.log"), b"owned history").unwrap();
        fs::create_dir(root.join("logs/manager-123.log")).unwrap();
        Preferences {
            log_dir: root.join("logs").display().to_string(),
            ..Default::default()
        }
        .save(&root)
        .unwrap();
        fs::write(root.join("wallpapers-native/a/file"), b"original").unwrap();
        fs::write(root.join("wallpapers-native/a/manifest.json"), b"{}").unwrap();
        fs::write(root.join("wallpapers-native/unrelated.txt"), b"leave here").unwrap();
        let next = Preferences {
            wallpaper_dir: root.join("new-packs").display().to_string(),
            log_dir: root.join("new-logs").display().to_string(),
            ..Default::default()
        };
        assert!(migrate(&root, &next, || Err("reload failed".into())).is_err());
        assert!(root.join("wallpapers-native/a/file").exists());
        assert!(!root.join("new-packs/a/file").exists());
        assert!(Preferences::load(&root).wallpaper_dir.is_empty());
        fs::create_dir_all(root.join("new-packs/a")).unwrap();
        fs::write(root.join("new-packs/a/file"), b"other").unwrap();
        assert!(migrate(&root, &next, || Ok(())).is_err());
        fs::write(root.join("new-packs/a/file"), b"original").unwrap();
        migrate(&root, &next, || Ok(())).unwrap();
        assert!(!root.join("wallpapers-native/a/file").exists());
        assert!(root.join("wallpapers-native/unrelated.txt").exists());
        assert!(!root.join("logs/manager.log").exists());
        assert!(
            fs::read(root.join("new-logs/manager.log"))
                .unwrap()
                .starts_with(b"existing log\n")
        );
        assert_eq!(
            fs::read(root.join("logs/other-app.log")).unwrap(),
            b"unrelated"
        );
        for name in UNRELATED_LOGS {
            assert_eq!(
                fs::read(root.join("logs").join(name)).unwrap(),
                b"unrelated log"
            );
            assert!(!root.join("new-logs").join(name).exists());
        }
        assert!(root.join("logs/manager-123.log").is_dir());
        assert!(!root.join("new-logs/manager-123.log").exists());
        assert!(!root.join("logs/player-1731845500123.log").exists());
        assert_eq!(
            fs::read(root.join("new-logs/player-1731845500123.log")).unwrap(),
            b"owned history"
        );
        assert_eq!(
            fs::read(root.join("new-packs/a/file")).unwrap(),
            b"original"
        );
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn logs_rotate_and_share_a_ten_file_retention_budget() {
        let root = std::env::temp_dir().join(format!(
            "a75-log-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let dir = root.join("shared-logs");
        fs::create_dir_all(&dir).unwrap();
        Preferences {
            log_dir: dir.display().to_string(),
            ..Default::default()
        }
        .save(&root)
        .unwrap();
        for name in UNRELATED_LOGS {
            fs::write(dir.join(name), b"unrelated log").unwrap();
        }
        fs::create_dir(dir.join("manager-123.log")).unwrap();
        fs::write(dir.join("player.log"), b"active player").unwrap();
        for i in 0..9 {
            fs::write(dir.join(format!("player-{i}.log")), b"history").unwrap();
        }
        fs::File::create(dir.join("manager.log"))
            .unwrap()
            .set_len(10 * 1024 * 1024)
            .unwrap();
        log(&root, "INFO", "rotation fixture");
        assert_eq!(
            fs::read_dir(&dir)
                .unwrap()
                .map(|e| e.unwrap().path())
                .filter(|p| owned_log_file(p).unwrap())
                .count(),
            10
        );
        for name in UNRELATED_LOGS {
            assert_eq!(fs::read(dir.join(name)).unwrap(), b"unrelated log");
        }
        assert!(dir.join("manager-123.log").is_dir());
        assert_eq!(fs::read(dir.join("player.log")).unwrap(), b"active player");
        assert!(fs::metadata(dir.join("manager.log")).unwrap().len() < 1024);
        assert!(
            fs::read_to_string(dir.join("manager.log"))
                .unwrap()
                .contains("rotation fixture")
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn migration_capacity_groups_only_new_bytes_by_volume() {
        let allocations = vec![(PathBuf::from("packs"), 70), (PathBuf::from("logs"), 30)];
        // Each disk fits its own data, although neither fits the total.
        check_migration_space(&allocations, |p| {
            Ok((
                p.display().to_string(),
                if p == Path::new("packs") { 70 } else { 30 },
            ))
        })
        .unwrap();
        // Two paths / mount aliases on one disk must fit their combined total.
        assert!(check_migration_space(&allocations, |_| Ok(("same-volume".into(), 99))).is_err());
        check_migration_space(&allocations, |_| Ok(("same-volume".into(), 100))).unwrap();
        // Free-space observations may change: use the conservative observation.
        assert!(
            check_migration_space(&allocations, |p| Ok((
                "same-volume".into(),
                if p == Path::new("packs") { 200 } else { 90 }
            )))
            .is_err()
        );
        check_migration_space(&[(PathBuf::from("unchanged-full-disk"), 0)], |_| {
            panic!("A zero-byte target must not be probed")
        })
        .unwrap();
        assert!(
            check_migration_space(
                &[
                    (PathBuf::from("packs"), u64::MAX),
                    (PathBuf::from("logs"), 1)
                ],
                |_| Ok(("same-volume".into(), u64::MAX))
            )
            .is_err()
        );
    }

    #[test]
    fn migration_checks_each_changed_destination_and_skips_unchanged_or_identical_files() {
        let root = fixture_root("migration-volumes");
        fs::create_dir_all(root.join("wallpapers-native/a")).unwrap();
        fs::create_dir(root.join("logs")).unwrap();
        fs::write(root.join("wallpapers-native/a/manifest.json"), b"{}").unwrap();
        fs::write(root.join("wallpapers-native/a/asset"), b"12345678").unwrap();
        fs::write(root.join("logs/player.log"), b"123").unwrap();
        let mut next = Preferences {
            wallpaper_dir: root.join("new-packs").display().to_string(),
            log_dir: root.join("new-logs").display().to_string(),
            ..Default::default()
        };
        let mut activate_called = false;
        assert!(
            migrate_with_space_check(
                &root,
                &next,
                || {
                    activate_called = true;
                    Ok(())
                },
                |allocations| {
                    assert_eq!(allocations.len(), 2);
                    assert_eq!(allocations.iter().map(|(_, bytes)| bytes).sum::<u64>(), 13);
                    check_migration_space(allocations, |_| Ok(("same-volume".into(), 12)))
                }
            )
            .is_err()
        );
        assert!(!activate_called);
        assert!(!root.join("new-packs/a/asset").exists());
        assert!(root.join("wallpapers-native/a/asset").exists());
        assert!(Preferences::load(&root).wallpaper_dir.is_empty());
        // A full old log disk must not block moving just wallpapers away.
        next.log_dir = root.join("logs").display().to_string();
        fs::create_dir_all(root.join("new-packs/a")).unwrap();
        let other_probe = root
            .join("new-packs")
            .join(format!(".action75-write-{}", std::process::id()));
        fs::write(&other_probe, b"unrelated user file").unwrap();
        fs::write(root.join("new-packs/a/manifest.json"), b"{}").unwrap();
        migrate_with_space_check(
            &root,
            &next,
            || Ok(()),
            |allocations| {
                assert_eq!(
                    allocations,
                    &[(fs::canonicalize(root.join("new-packs/a")).unwrap(), 8)]
                );
                check_migration_space(allocations, |_| Ok(("destination-volume".into(), 8)))
            },
        )
        .unwrap();
        assert_eq!(fs::read(&other_probe).unwrap(), b"unrelated user file");
        assert_eq!(
            fs::read(root.join("new-packs/a/asset")).unwrap(),
            b"12345678"
        );
        // Already identical destination files need no capacity on any disk.
        let last = Preferences {
            wallpaper_dir: root.join("final-packs").display().to_string(),
            log_dir: next.log_dir.clone(),
            ..Default::default()
        };
        fs::create_dir_all(root.join("final-packs/a")).unwrap();
        fs::copy(
            root.join("new-packs/a/manifest.json"),
            root.join("final-packs/a/manifest.json"),
        )
        .unwrap();
        fs::copy(
            root.join("new-packs/a/asset"),
            root.join("final-packs/a/asset"),
        )
        .unwrap();
        migrate_with_space_check(
            &root,
            &last,
            || Ok(()),
            |allocations| {
                assert!(allocations.is_empty());
                check_migration_space(allocations, |_| panic!("No new files to allocate"))
            },
        )
        .unwrap();
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn split_volume_migration_does_not_require_the_total_on_each_destination() {
        let root = fixture_root("split-volumes");
        fs::create_dir_all(root.join("wallpapers-native/a")).unwrap();
        fs::create_dir(root.join("logs")).unwrap();
        fs::write(root.join("wallpapers-native/a/manifest.json"), b"{}").unwrap();
        fs::write(root.join("wallpapers-native/a/asset"), b"12345678").unwrap();
        fs::write(root.join("logs/player.log"), b"123").unwrap();
        let next = Preferences {
            wallpaper_dir: root.join("new-packs").display().to_string(),
            log_dir: root.join("new-logs").display().to_string(),
            ..Default::default()
        };
        let mut probes = 0;
        migrate_with_space_check(
            &root,
            &next,
            || Ok(()),
            |allocations| {
                check_migration_space(allocations, |target| {
                    probes += 1;
                    if target.ends_with("new-packs") {
                        Ok(("wallpaper-volume".into(), 10))
                    } else if target.ends_with("new-logs") {
                        Ok(("log-volume".into(), 3))
                    } else {
                        panic!("Unexpected target: {}", target.display())
                    }
                })
            },
        )
        .unwrap();
        assert_eq!(probes, 2);
        assert_eq!(
            fs::read(root.join("new-packs/a/asset")).unwrap(),
            b"12345678"
        );
        assert!(
            fs::read(root.join("new-logs/player.log"))
                .unwrap()
                .starts_with(b"123")
        );
        assert!(!root.join("wallpapers-native/a/asset").exists());
        assert!(!root.join("logs/player.log").exists());
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn windows_volume_probe_resolves_different_directories_to_one_volume() {
        let root = fixture_root("volume-probe");
        fs::create_dir(root.join("nested")).unwrap();
        let (a, _) = volume_space(&fs::canonicalize(&root).unwrap()).unwrap();
        let (b, _) = volume_space(&fs::canonicalize(root.join("nested")).unwrap()).unwrap();
        assert_eq!(a, b);
        assert!(
            a.starts_with(r"\\?\volume{"),
            "Expected a real local volume GUID: {a}"
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn windows_existing_cross_volume_junction_uses_the_actual_allocation_volume() {
        use std::os::windows::process::CommandExt;
        let root = fixture_root("mounted-migration");
        let other = std::env::current_dir()
            .unwrap()
            .join("target")
            .join(root.file_name().unwrap());
        fs::create_dir_all(other.parent().unwrap()).unwrap();
        fs::create_dir(&other).unwrap();
        let root_volume = volume_space(&root).unwrap().0;
        let other_volume = volume_space(&other).unwrap().0;
        let link = root.join("new-packs").join("a");
        // cmd's mklink is used only for a disposable test fixture. Refuse shell
        // metacharacters rather than interpolating arbitrary checkout paths.
        if root_volume == other_volume
            || [&link, &other].iter().any(|p| {
                p.to_string_lossy()
                    .chars()
                    .any(|c| "&|<>^%!\"\r\n".contains(c))
            })
        {
            println!(
                "SKIP: cross-volume junction fixture needs a second local test volume and plain paths"
            );
            fs::remove_dir_all(other).unwrap();
            fs::remove_dir_all(root).unwrap();
            return;
        }
        fs::create_dir_all(root.join("new-packs")).unwrap();
        let made = std::process::Command::new("cmd.exe")
            .args(["/d", "/c", "mklink", "/J"])
            .arg(&link)
            .arg(&other)
            .creation_flags(0x08000000)
            .output()
            .unwrap();
        if !made.status.success() {
            println!(
                "SKIP: Windows refused the disposable junction fixture: {}",
                String::from_utf8_lossy(&made.stderr)
            );
            fs::remove_dir_all(other).unwrap();
            fs::remove_dir_all(root).unwrap();
            return;
        }
        fs::create_dir_all(root.join("wallpapers-native/a")).unwrap();
        fs::write(root.join("wallpapers-native/a/manifest.json"), b"{}").unwrap();
        fs::write(root.join("wallpapers-native/a/asset"), b"12345678").unwrap();
        let next = Preferences {
            wallpaper_dir: root.join("new-packs").display().to_string(),
            ..Default::default()
        };
        migrate_with_space_check(
            &root,
            &next,
            || Ok(()),
            |allocations| {
                assert_eq!(allocations, &[(fs::canonicalize(&other).unwrap(), 10)]);
                check_migration_space(allocations, |target| {
                    let space = volume_space(target)?;
                    assert_eq!(space.0, other_volume);
                    assert_ne!(space.0, root_volume);
                    Ok(space)
                })
            },
        )
        .unwrap();
        assert_eq!(fs::read(other.join("asset")).unwrap(), b"12345678");
        assert!(!root.join("wallpapers-native/a/asset").exists());
        println!(
            "PASS: actual cross-volume junction migration charged {other_volume} instead of {root_volume}"
        );
        fs::remove_dir(&link).unwrap();
        fs::remove_dir_all(other).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
