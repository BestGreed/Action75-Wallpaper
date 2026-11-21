use super::{FILE_BYTES, MAX_DEPTH, MAX_ENTRIES, PACKAGE_BYTES, Preparation, prepare};
use std::{
    fs::{self, File},
    io::{self, Read},
    path::{Path, PathBuf},
};
use zip::ZipArchive;

pub fn prepare_zip(root: &Path, source: &Path) -> Result<Preparation, String> {
    let scratch = tempfile::tempdir().map_err(|e| e.to_string())?;
    let file = File::open(source).map_err(|e| format!("无法打开 ZIP 文件，{e}"))?;
    let package = extract(file, scratch.path())?;
    prepare(root, &package)
}

fn package_path(name: &str) -> Result<PathBuf, String> {
    let mut path = PathBuf::new();
    for part in name.trim_end_matches(['/', '\\']).split(['/', '\\']) {
        let stem = part
            .split('.')
            .next()
            .unwrap_or_default()
            .trim_end()
            .to_ascii_uppercase();
        let reserved = matches!(
            stem.as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
        ) || stem
            .strip_prefix("COM")
            .or_else(|| stem.strip_prefix("LPT"))
            .is_some_and(|s| {
                matches!(
                    s,
                    "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
                )
            });
        if part.is_empty()
            || part.ends_with(['.', ' '])
            || reserved
            || part.contains([':', '<', '>', '|', '"', '?', '*'])
            || part.chars().any(char::is_control)
        {
            return Err(format!("ZIP 内含无效路径：{name}"));
        }
        path.push(part);
    }
    if path.components().count() > MAX_DEPTH + 2 {
        return Err("ZIP 内的目录过深".into());
    }
    Ok(path)
}

fn extract(file: File, dest: &Path) -> Result<PathBuf, String> {
    if file.metadata().map_err(|e| e.to_string())?.len() > PACKAGE_BYTES + 2 * 1024 * 1024 {
        return Err("壁纸 ZIP 超过大小限制".into());
    }
    let mut zip = ZipArchive::new(file).map_err(|e| format!("无法读取 ZIP，{e}"))?;
    if zip.len() > MAX_ENTRIES + 1 {
        return Err("ZIP 内的文件和目录过多".into());
    }
    let mut total = 0;
    for i in 0..zip.len() {
        let mut entry = zip
            .by_index(i)
            .map_err(|e| format!("无法读取 ZIP 条目，{e}"))?;
        let path = dest.join(package_path(entry.name())?);
        if entry
            .unix_mode()
            .is_some_and(|mode| !matches!(mode & 0o170000, 0 | 0o040000 | 0o100000))
        {
            return Err("ZIP 内不能包含链接或特殊文件".into());
        }
        if entry.is_dir() {
            fs::create_dir_all(path).map_err(|e| e.to_string())?;
            continue;
        }
        let size = entry.size();
        if size > FILE_BYTES || total + size > PACKAGE_BYTES {
            return Err("ZIP 内单个文件限 64 MiB，解压总量限 128 MiB".into());
        }
        fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
        let mut out = File::create_new(path).map_err(|e| format!("无法写入 ZIP 条目，{e}"))?;
        let copied = io::copy(&mut (&mut entry).take(size + 1), &mut out)
            .map_err(|e| format!("ZIP 解压失败，{e}"))?;
        if copied != size {
            return Err("ZIP 条目的实际大小与目录记录不符".into());
        }
        total += copied;
    }
    let mut root = dest.to_owned();
    while !root.join("manifest.json").is_file() {
        let mut files = fs::read_dir(&root).map_err(|e| e.to_string())?;
        match (files.next(), files.next()) {
            (Some(Ok(entry)), None) if entry.path().is_dir() => root = entry.path(),
            _ => {
                return Err("请选择只包含一个 Action75 壁纸包的 ZIP，包内需要 manifest.json".into());
            }
        }
    }
    Ok(root)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::tempdir;
    use zip::{ZipWriter, write::SimpleFileOptions};

    fn unzip(names: &[&str], dir: &Path) -> Result<PathBuf, String> {
        let mut writer = ZipWriter::new(tempfile::tempfile().unwrap());
        for name in names {
            writer
                .start_file(*name, SimpleFileOptions::default())
                .unwrap();
            writer.write_all(b"{}").unwrap();
        }
        extract(writer.finish().unwrap(), dir)
    }

    #[test]
    fn accepts_flat_and_wrapped_packages() {
        for prefix in ["", "磁流体/"] {
            let dir = tempdir().unwrap();
            let root = unzip(&[&format!("{prefix}manifest.json")], dir.path()).unwrap();
            assert_eq!(fs::read(root.join("manifest.json")).unwrap(), b"{}");
        }
    }

    #[test]
    fn rejects_multiple_packages_and_invalid_windows_names() {
        let dir = tempdir().unwrap();
        assert!(unzip(&["a/manifest.json", "b/manifest.json"], dir.path()).is_err());
        for name in [
            "../file",
            r"a\..\file",
            "/file",
            "x:stream",
            "dir/NUL.txt",
            "a./file",
            r"\server\file",
        ] {
            assert!(package_path(name).is_err(), "{name}");
        }
    }
}
