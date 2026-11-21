//! A wallpaper is published by replacing one manifest. Its resources live in
//! a revision directory, so the player can keep its loaded version during an update.
mod archive;
mod media;
pub use archive::prepare_zip;
mod trial;
pub use media::{media_filter_pattern, prepare_media};

use crate::wallpaper_library::{self, REVISION, VERSIONS, valid_revision};

use std::{
    collections::BTreeMap,
    fs::{self, File, Metadata, OpenOptions},
    io::{self, Read, Write},
    os::windows::{
        fs::{MetadataExt, OpenOptionsExt},
        io::AsRawHandle,
    },
    path::{Component, Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use serde_json::{Value, json};
use windows::Win32::{
    Foundation::HANDLE,
    Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_OPEN_REPARSE_POINT,
        FILE_NAME_NORMALIZED, FILE_SHARE_READ, GetFileInformationByHandle,
        GetFinalPathNameByHandleW, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    },
};

const STAGING: &str = ".action75-import";
const MANIFEST_BYTES: u64 = 64 * 1024;
const FILE_BYTES: u64 = 64 * 1024 * 1024;
const PACKAGE_BYTES: u64 = 128 * 1024 * 1024;
const MAX_ENTRIES: usize = 512;
const MAX_DEPTH: usize = 12;

pub enum Preparation {
    Installed(Receipt),
    Ready(Box<PreparedImport>),
}

pub struct Receipt {
    pub id: String,
    pub name: String,
    pub updated: bool,
}

#[derive(PartialEq, Eq)]
struct Installed {
    directory: PathBuf,
    before: Vec<u8>,
    id: String,
    name: String,
    revision: Option<String>,
}

impl Installed {
    fn original(&self) -> PathBuf {
        match &self.revision {
            Some(revision) => self.directory.join(VERSIONS).join(revision),
            None => self.directory.clone(),
        }
    }
}

pub struct PreparedImport {
    stage: Stage,
    root: PathBuf,
    library: PathBuf,
    package: PathBuf,
    manifest: Value,
    id: String,
    name: String,
    existing: Option<Installed>,
}

struct Stage {
    directory: PathBuf,
    parent: PathBuf,
    revision: String,
    _lock: File,
}

impl Drop for Stage {
    fn drop(&mut self) {
        remove_owned_directory(&self.parent, &self.directory);
    }
}

fn path_error(action: &str, path: &Path, error: io::Error) -> String {
    format!("{action} {}，{error}", path.display())
}

fn is_reparse(meta: &Metadata) -> bool {
    meta.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT.0 != 0
}

fn plain_directory(path: &Path) -> Result<PathBuf, String> {
    let meta = fs::symlink_metadata(path).map_err(|e| path_error("无法读取文件夹", path, e))?;
    if !meta.is_dir() || is_reparse(&meta) {
        return Err(format!(
            "请选择普通文件夹，链接或重解析点不可导入（{}）",
            path.display()
        ));
    }
    fs::canonicalize(path).map_err(|e| path_error("无法解析文件夹", path, e))
}

fn deletion_target(root: &Path, id: &str, path: &Path) -> Result<(), String> {
    let target = plain_directory(path)?;
    let configured = crate::storage::Preferences::load_checked(root)?.wallpapers(root);
    let in_library = crate::wallpaper_library::sources(root, configured)
        .into_iter()
        .any(|(dir, bundled)| {
            (!bundled || crate::wallpaper_library::bundled_id(id))
                && dir
                    .canonicalize()
                    .is_ok_and(|dir| target.parent() == Some(&dir))
        });
    if !in_library || target == root.canonicalize().map_err(|e| e.to_string())? {
        return Err("该文件夹不在壁纸库中".into());
    }
    let manifest = parse_manifest(&read_small(&target, &target.join("manifest.json"))?)?;
    if !manifest["id"]
        .as_str()
        .is_some_and(|current| current.eq_ignore_ascii_case(id))
    {
        return Err("壁纸已变更，请刷新后重试".into());
    }
    Ok(())
}

fn recycle(path: &Path) -> windows::core::Result<()> {
    use windows::{
        Win32::{System::Com::*, UI::Shell::*},
        core::HSTRING,
    };
    unsafe {
        CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok()?;
        let result = (|| {
            // Rust's canonical Windows paths can have a verbatim prefix. The
            // Shell namespace expects a drive path or a standard UNC path.
            let name = path.to_str().ok_or_else(|| {
                windows::core::Error::from_hresult(windows::Win32::Foundation::E_INVALIDARG)
            })?;
            let shell_name = if let Some(unc) = name.strip_prefix(r"\\?\UNC\") {
                HSTRING::from(format!(r"\\{unc}"))
            } else {
                HSTRING::from(name.strip_prefix(r"\\?\").unwrap_or(name))
            };
            let item: IShellItem = SHCreateItemFromParsingName(&shell_name, None)?;
            let op: IFileOperation = CoCreateInstance(&FileOperation, None, CLSCTX_INPROC_SERVER)?;
            op.SetOperationFlags(
                FOFX_RECYCLEONDELETE
                    | FOF_NOERRORUI
                    | FOF_NOCONFIRMATION
                    | FOF_SILENT
                    | FOFX_EARLYFAILURE,
            )?;
            op.DeleteItem(&item, None)?;
            op.PerformOperations()?;
            if op.GetAnyOperationsAborted()?.as_bool() || path.exists() {
                return Err(windows::core::Error::new(
                    windows::Win32::Foundation::E_FAIL,
                    "壁纸文件夹未移入回收站",
                ));
            }
            Ok(())
        })();
        CoUninitialize();
        result
    }
}

pub(crate) fn delete_pack(root: &Path, id: &str, path: &Path) -> Result<(), String> {
    deletion_target(root, id, path)?;
    let active = match crate::command(root, serde_json::json!({"action":"status"})) {
        Ok(status) => status["packId"] == id,
        Err(e) if e == "播放器未启动" => false,
        Err(e) => return Err(e),
    };
    let select =
        |pack: &str| crate::command(root, serde_json::json!({"action":"selectPack","id":pack}));
    if active {
        select("")?;
    }
    if let Err(error) = recycle(path) {
        if active && let Err(restore) = select(id) {
            return Err(format!("删除失败：{error}。恢复播放失败：{restore}"));
        }
        return Err(format!("无法移入回收站：{error}"));
    }
    Ok(())
}

fn private_directory(path: &Path) -> Result<PathBuf, String> {
    if let Err(error) = fs::create_dir(path)
        && error.kind() != io::ErrorKind::AlreadyExists
    {
        return Err(path_error("无法创建导入目录", path, error));
    }
    plain_directory(path)
}

fn remove_owned_directory(parent: &Path, path: &Path) {
    if let Ok(actual) = plain_directory(path)
        && actual.parent() == Some(parent)
    {
        let _ = fs::remove_dir_all(actual);
    }
}

impl Stage {
    fn create(library: &Path) -> Result<Self, String> {
        let parent = private_directory(&library.join(STAGING))?;
        if parent.parent() != Some(library) {
            return Err("导入暂存目录超出壁纸库".into());
        }
        let lock_path = parent.join("lock");
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .share_mode(0)
            .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT.0)
            .open(&lock_path)
            .map_err(|e| path_error("其他导入正在进行，或无法锁定壁纸库", &lock_path, e))?;
        assert_open_file(&parent, &lock, &lock_path)?;
        for entry in fs::read_dir(&parent).map_err(|e| e.to_string())?.flatten() {
            if entry.file_name().to_str().is_some_and(valid_revision) {
                remove_owned_directory(&parent, &entry.path());
            }
        }
        let time = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let revision = format!("v-{:08x}{time:032x}", std::process::id());
        let directory = parent.join(&revision);
        fs::create_dir(&directory)
            .map_err(|e| path_error("无法创建导入暂存目录", &directory, e))?;
        Ok(Self {
            directory,
            parent,
            revision,
            _lock: lock,
        })
    }
}

fn assert_open_file(base: &Path, file: &File, given: &Path) -> Result<(), String> {
    let meta = file
        .metadata()
        .map_err(|e| path_error("无法读取", given, e))?;
    if !meta.is_file() || is_reparse(&meta) {
        return Err(format!("包内不能包含链接或重解析点（{}）", given.display()));
    }
    let handle = HANDLE(file.as_raw_handle());
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    unsafe { GetFileInformationByHandle(handle, &mut info) }
        .map_err(|error| format!("无法检查资源连接（{}），{error}", given.display()))?;
    if info.nNumberOfLinks != 1 {
        return Err(format!("包内不能包含硬链接（{}）", given.display()));
    }
    let mut path = vec![0; 32_768];
    let len =
        unsafe { GetFinalPathNameByHandleW(handle, &mut path, FILE_NAME_NORMALIZED) } as usize;
    use std::os::windows::ffi::OsStringExt;
    if len == 0 || len >= path.len() {
        return Err(format!("无法检查资源路径（{}）", given.display()));
    }
    let final_path = PathBuf::from(std::ffi::OsString::from_wide(&path[..len]));
    if !final_path.starts_with(base) {
        return Err(format!("资源路径超出包目录（{}）", given.display()));
    }
    Ok(())
}

fn open_input(base: &Path, path: &Path) -> Result<File, String> {
    let file = OpenOptions::new()
        .read(true)
        .share_mode(FILE_SHARE_READ.0)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT.0)
        .open(path)
        .map_err(|e| path_error("无法读取", path, e))?;
    assert_open_file(base, &file, path)?;
    Ok(file)
}

fn read_small(base: &Path, path: &Path) -> Result<Vec<u8>, String> {
    let mut result = Vec::new();
    open_input(base, path)?
        .take(MANIFEST_BYTES + 1)
        .read_to_end(&mut result)
        .map_err(|e| path_error("无法读取", path, e))?;
    if result.len() as u64 > MANIFEST_BYTES {
        return Err(format!("JSON 文件超过 64 KiB（{}）", path.display()));
    }
    Ok(result)
}

fn parse_manifest(bytes: &[u8]) -> Result<Value, String> {
    serde_json::from_slice(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(bytes))
        .map_err(|error| format!("manifest.json 格式有误，{error}"))
}

fn valid_id(id: &str) -> bool {
    let upper = id.to_ascii_uppercase();
    !id.is_empty()
        && id.len() <= 64
        && id
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_'))
        && !matches!(upper.as_str(), "CON" | "NUL" | "PRN" | "AUX")
        && !(upper.len() == 4
            && (upper.starts_with("COM") || upper.starts_with("LPT"))
            && matches!(upper.as_bytes()[3], b'1'..=b'9'))
}

fn resource_path(value: &Value, key: &str, extensions: &[&str]) -> Result<PathBuf, String> {
    let asset = value
        .as_str()
        .ok_or_else(|| format!("清单中的 {key} 必须是资源路径"))?;
    relative_resource(asset, key, extensions)
}

fn relative_resource(asset: &str, key: &str, extensions: &[&str]) -> Result<PathBuf, String> {
    let path = Path::new(asset);
    if asset.is_empty()
        || asset.len() > 240
        || asset.split(['/', '\\']).any(|part| {
            part.is_empty()
                || part.ends_with(['.', ' '])
                || part.contains(':')
                || part.chars().any(char::is_control)
        })
        || !path
            .components()
            .all(|component| matches!(component, Component::Normal(_)))
    {
        return Err(format!("{key} 需使用包内相对路径（{asset}）"));
    }
    if !path
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            extensions
                .iter()
                .any(|allowed| extension.eq_ignore_ascii_case(allowed))
        })
    {
        return Err(format!("{key} 使用了当前不支持的资源格式（{asset}）"));
    }
    Ok(path.to_owned())
}

fn supported_native_backend(manifest: &Value) -> bool {
    match (manifest["abi"].as_u64(), manifest.get("renderer")) {
        (Some(1), None) => true,
        (Some(1), Some(renderer)) => renderer == "d3d11",
        (Some(2), Some(renderer)) => renderer == "d3d12",
        _ => false,
    }
}

fn validate_manifest(manifest: &Value, package: &Path) -> Result<(), String> {
    if manifest["version"] != 1 {
        return Err("目前支持 version 为 1 的 Action75 壁纸清单".into());
    }
    let kind = match manifest.get("kind") {
        Some(value) => value.as_str().ok_or("清单中的 kind 必须是后端类型名称")?,
        None => "effekseer",
    };
    if !matches!(
        kind,
        "spriteBurst" | "effekseer" | "fireworks" | "native" | "image" | "video"
    ) {
        return Err(format!("暂不支持导入 {kind} 类型的壁纸包"));
    }
    let fields = manifest.as_object().ok_or("清单必须是 JSON 对象")?;
    for key in fields.keys().map(String::as_str) {
        let allowed = match key {
            "id" | "name" | "version" | "kind" | "description" | "settings" | "preview"
            | "author" | "license" | "packVersion" => true,
            "enabled" => manifest[key] == true,
            "background" => matches!(kind, "spriteBurst" | "effekseer" | "image" | "video"),
            "effect" => matches!(kind, "spriteBurst" | "effekseer"),
            "columns" | "rows" | "diameter" | "duration" | "composition" => kind == "spriteBurst",
            "effects" | "scale" => kind == "effekseer",
            "runtimeControls" => {
                matches!(kind, "spriteBurst" | "effekseer") && manifest[key] == false
            }
            "abi" | "entry" | "renderer" => kind == "native",
            "desktopOpacityControl" => {
                kind == "native"
                    && manifest["abi"] == 2
                    && manifest["renderer"] == "d3d12"
                    && manifest[key].is_string()
            }
            _ => false,
        };
        if !allowed {
            return Err(format!("清单中的 {key} 功能暂不可导入"));
        }
    }
    let check = |value: &Value, key, formats: &[&str]| {
        let path = resource_path(value, key, formats)?;
        let file = package.join(&path);
        if !file.is_file() {
            return Err(format!("缺少资源 {}", path.display()));
        }
        Ok(())
    };
    for (key, extensions) in [
        ("background", media::extensions(kind).unwrap_or(&["png"])),
        ("settings", &["json"][..]),
        ("preview", &["png", "jpg", "jpeg"][..]),
    ] {
        if let Some(value) = manifest.get(key) {
            check(value, key, extensions)?;
        } else if key == "background" && media::extensions(kind).is_some() {
            return Err("媒体清单需要指定 background 媒体文件".into());
        } else if key == "background" && kind == "spriteBurst" {
            return Err("图集包需要在清单中指定 background 底图".into());
        }
    }
    if kind == "spriteBurst" {
        check(&manifest["effect"], "effect", &["png"])?;
        for key in ["columns", "rows", "duration", "diameter"] {
            if !manifest[key].is_number() {
                return Err(format!("图集包需要在清单中指定数值 {key}"));
            }
        }
    } else if kind == "effekseer" {
        if let Some(effect) = manifest.get("effect") {
            check(effect, "effect", &["efk", "efkefc"])?;
        } else if manifest.get("effects").is_none() {
            return Err("粒子包需要在清单中指定 effect 或 effects".into());
        }
        if let Some(effects) = manifest.get("effects") {
            let effects = effects
                .as_array()
                .filter(|v| (1..=32).contains(&v.len()))
                .ok_or("清单中的 effects 需要包含 1 至 32 个粒子文件")?;
            for effect in effects {
                check(effect, "effects", &["efk", "efkefc"])?;
            }
        }
    } else if kind == "native" {
        if !supported_native_backend(manifest) {
            return Err("原生壁纸需要 ABI 1（D3D11）或 ABI 2（renderer 为 d3d12）".into());
        }
        let entry = native_asset_path(manifest, "entry", &["dll"])?;
        if !package.join(&entry).is_file() {
            return Err(format!("缺少资源 {}", entry.display()));
        }
        if manifest.get("settings").is_none() {
            return Err("原生壁纸需要声明 settings 参数文件".into());
        }
        let settings = native_asset_path(manifest, "settings", &["json"])?;
        let bytes = read_small(package, &package.join(&settings))?;
        let schema: Value =
            serde_json::from_slice(bytes.strip_prefix(b"\xef\xbb\xbf").unwrap_or(&bytes))
                .map_err(|error| format!("原生壁纸参数文件格式有误，{error}"))?;
        if schema["version"] != 1 || !schema["groups"].is_array() {
            return Err("原生壁纸参数文件需要 version 1 和 groups".into());
        }
        if let Some(id) = manifest["desktopOpacityControl"].as_str() {
            let mut controls = schema["groups"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|group| group["controls"].as_array())
                .flatten()
                .filter(|control| control["id"] == id);
            let slider = controls.next();
            if id.is_empty()
                || id.len() > 64
                || !id
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_'))
                || controls.next().is_some()
                || !slider.is_some_and(|c| {
                    c["type"] == "slider"
                        && c["min"].as_f64() == Some(0.)
                        && c["max"].as_f64() == Some(100.)
                        && c["default"].as_f64() == Some(100.)
                })
            {
                return Err(
                    "desktopOpacityControl 需要引用默认 100、范围 0 到 100 的不透明度滑块".into(),
                );
            }
        }
    }
    Ok(())
}

fn native_asset_path(manifest: &Value, key: &str, formats: &[&str]) -> Result<PathBuf, String> {
    let asset = manifest[key]
        .as_str()
        .ok_or_else(|| format!("清单中的 {key} 必须是资源路径"))?;
    let revision = manifest
        .get(REVISION)
        .map(|value| {
            value
                .as_str()
                .filter(|revision| valid_revision(revision))
                .ok_or("发布版本无效")
        })
        .transpose()?;
    let mut parts = asset.splitn(3, ['/', '\\']);
    let nested = if parts
        .next()
        .is_some_and(|part| part.eq_ignore_ascii_case(VERSIONS))
    {
        parts
            .next()
            .filter(|part| revision.is_some_and(|r| part.eq_ignore_ascii_case(r)))
            .ok_or_else(|| format!("{key} 只能指向当前发布版本"))?;
        parts.next().ok_or_else(|| format!("缺少 {key} 资源路径"))?
    } else {
        asset
    };
    let relative = relative_resource(nested, key, formats)?;
    Ok(match revision {
        Some(revision) => Path::new(VERSIONS).join(revision).join(relative),
        None => relative,
    })
}

pub(crate) fn native_catalog_available(directory: &Path, manifest: &Value) -> bool {
    if manifest["version"] != 1 || !supported_native_backend(manifest) {
        return false;
    }
    let Ok(base) = plain_directory(directory) else {
        return false;
    };
    [("entry", &["dll"][..]), ("settings", &["json"][..])]
        .into_iter()
        .all(|(key, formats)| {
            let Ok(path) = native_asset_path(manifest, key, formats) else {
                return false;
            };
            let mut file = base.clone();
            for component in path.components() {
                file.push(component);
                if !fs::symlink_metadata(&file).is_ok_and(|meta| !is_reparse(&meta)) {
                    return false;
                }
            }
            file.is_file()
                && fs::canonicalize(file).is_ok_and(|resolved| resolved.starts_with(&base))
        })
}

fn allowed_package_file(path: &Path, native_entry: Option<&Path>) -> bool {
    if matches!(
        path.file_name().and_then(|name| name.to_str()),
        Some("LICENSE" | "COPYING")
    ) {
        return true;
    }
    let Some(extension) = path.extension().and_then(|ext| ext.to_str()) else {
        return false;
    };
    match extension.to_ascii_lowercase().as_str() {
        "json" | "png" | "jpg" | "jpeg" | "md" | "txt" => true,
        "dll" => native_entry.is_some_and(|entry| {
            entry
                .components()
                .map(|part| part.as_os_str().to_ascii_lowercase())
                .eq(path
                    .components()
                    .map(|part| part.as_os_str().to_ascii_lowercase()))
        }),
        "cso" | "dxbc" | "dxil" | "dds" => native_entry.is_some(),
        "efk" | "efkmat" => true,
        "efkmatd" => native_entry.is_some(),
        "efkefc" | "efkmodel" | "efkcurve" | "wav" => native_entry.is_none(),
        _ => false,
    }
}

fn collect_files(
    base: &Path,
    native_entry: Option<&Path>,
    media_file: Option<&Path>,
) -> Result<BTreeMap<PathBuf, u64>, String> {
    fn visit(
        base: &Path,
        dir: &Path,
        depth: usize,
        entries: &mut usize,
        bytes: &mut u64,
        files: &mut BTreeMap<PathBuf, u64>,
        native_entry: Option<&Path>,
        media_file: Option<&Path>,
    ) -> Result<(), String> {
        if depth > MAX_DEPTH {
            return Err("包的子目录层级超过 12 层".into());
        }
        if !plain_directory(dir)?.starts_with(base) {
            return Err(format!("子目录超出包目录（{}）", dir.display()));
        }
        for entry in fs::read_dir(dir).map_err(|e| path_error("无法读取", dir, e))? {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();
            let relative = path.strip_prefix(base).map_err(|e| e.to_string())?;
            let metadata =
                fs::symlink_metadata(&path).map_err(|e| path_error("无法读取", relative, e))?;
            *entries += 1;
            if *entries > MAX_ENTRIES {
                return Err("包最多可包含 512 个文件和子目录".into());
            }
            if is_reparse(&metadata) {
                return Err(format!(
                    "包内不能包含链接或重解析点（{}）",
                    relative.display()
                ));
            }
            if metadata.is_dir() {
                visit(
                    base,
                    &path,
                    depth + 1,
                    entries,
                    bytes,
                    files,
                    native_entry,
                    media_file,
                )?;
            } else {
                let media = Some(relative) == media_file;
                if !metadata.is_file() || (!media && metadata.len() > FILE_BYTES) {
                    return Err(format!(
                        "文件超过 64 MiB 或无法复制（{}）",
                        relative.display()
                    ));
                }
                if !media && !allowed_package_file(relative, native_entry) {
                    return Err(format!("包内文件格式暂不可导入（{}）", relative.display()));
                }
                if !media {
                    *bytes += metadata.len();
                }
                if *bytes > PACKAGE_BYTES {
                    return Err("包的文件总量超过 128 MiB".into());
                }
                files.insert(relative.to_path_buf(), metadata.len());
            }
        }
        Ok(())
    }
    let mut files = BTreeMap::new();
    visit(
        base,
        base,
        0,
        &mut 0,
        &mut 0,
        &mut files,
        native_entry,
        media_file,
    )?;
    Ok(files)
}

fn copy_package(
    source: &Path,
    package: &Path,
    native_entry: Option<&Path>,
    media_file: Option<&Path>,
) -> Result<(), String> {
    let files = collect_files(source, native_entry, media_file)?;
    if media_file.is_some() {
        media::ensure_space(
            package
                .ancestors()
                .find(|dir| dir.is_dir())
                .ok_or("暂存目录不可用")?,
            files.values().copied().fold(0u64, u64::saturating_add),
        )?;
    }
    for (relative, bytes) in files {
        let file = open_input(source, &source.join(&relative))?;
        let output = package.join(&relative);
        fs::create_dir_all(output.parent().unwrap())
            .map_err(|e| path_error("无法暂存文件", &relative, e))?;
        let mut destination = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&output)
            .map_err(|e| path_error("无法暂存文件", &relative, e))?;
        let copied = io::copy(&mut file.take(bytes + 1), &mut destination)
            .and_then(|copied| destination.sync_all().map(|_| copied))
            .map_err(|e| path_error("无法复制", &relative, e))?;
        if copied != bytes {
            return Err(format!(
                "文件在导入时发生变化，请重新选择（{}）",
                relative.display()
            ));
        }
    }
    Ok(())
}

fn identical(
    candidate: &Path,
    original: &Path,
    native_entry: Option<&Path>,
    media_file: Option<&Path>,
) -> bool {
    let Ok(original) = plain_directory(original) else {
        return false;
    };
    let Ok(candidate) = plain_directory(candidate) else {
        return false;
    };
    let (Ok(a), Ok(b)) = (
        collect_files(&candidate, native_entry, media_file),
        collect_files(&original, native_entry, media_file),
    ) else {
        return false;
    };
    a == b
        && a.keys().all(|relative| {
            let result = (|| {
                let mut left = open_input(&candidate, &candidate.join(relative))?;
                let mut right = open_input(&original, &original.join(relative))?;
                crate::storage::same_contents(&mut left, &mut right).map_err(|e| e.to_string())
            })();
            result == Ok(true)
        })
}

fn find_installed(root: &Path, library: &Path, id: &str) -> Result<Option<Installed>, String> {
    let mut scanned = Vec::new();
    let mut matches = Vec::new();
    let bundled_root = fs::canonicalize(root.join("wallpapers-native")).ok();
    for (folder, bundled_only) in wallpaper_library::sources(root, library.to_owned()) {
        if bundled_only && !wallpaper_library::bundled_id(id) {
            continue;
        }
        let Ok(folder) = fs::canonicalize(folder) else {
            continue;
        };
        if scanned.contains(&folder) {
            continue;
        }
        let is_library = folder == library;
        let entries = match fs::read_dir(&folder) {
            Ok(entries) => entries,
            Err(error) if is_library => return Err(path_error("无法检查壁纸库", &folder, error)),
            Err(_) => continue,
        };
        let from_bundled_root = bundled_root.as_deref() == Some(folder.as_path());
        scanned.push(folder);
        for entry in entries.flatten() {
            let dir = entry.path();
            if bundled_only
                && !dir
                    .file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.eq_ignore_ascii_case(id))
            {
                continue;
            }
            if is_library
                && dir
                    .file_name()
                    .and_then(|s| s.to_str())
                    .is_some_and(|s| s.eq_ignore_ascii_case(id))
                && !dir.join("manifest.json").is_file()
            {
                return Err(format!("壁纸库已存在名为 {id} 的文件或文件夹"));
            }
            if !dir.join("manifest.json").is_file() {
                continue;
            }
            let Ok(directory) = plain_directory(&dir) else {
                continue;
            };
            let Ok(before) = read_small(&directory, &directory.join("manifest.json")) else {
                continue;
            };
            let Ok(value) = parse_manifest(&before) else {
                continue;
            };
            let Some(existing_id) = value["id"]
                .as_str()
                .filter(|other| other.eq_ignore_ascii_case(id))
            else {
                continue;
            };
            let bundled = from_bundled_root && wallpaper_library::bundled_manifest(&dir, &value);
            if bundled_only && !bundled {
                continue;
            }
            let revision = value[REVISION]
                .as_str()
                .filter(|r| valid_revision(r))
                .map(str::to_owned);
            matches.push((
                Installed {
                    directory,
                    before,
                    id: existing_id.to_owned(),
                    name: value["name"].as_str().unwrap_or(existing_id).to_owned(),
                    revision,
                },
                bundled,
            ));
        }
    }
    if matches.len() > 1 {
        if let Some(bundled) = matches.iter().position(|(_, bundled)| *bundled) {
            let mut managed = matches
                .iter()
                .enumerate()
                .filter_map(|(index, (entry, _))| entry.revision.is_some().then_some(index));
            let winner = managed.next().unwrap_or(bundled);
            if managed.next().is_none() {
                return Ok(Some(matches.swap_remove(winner).0));
            }
        }
        return Err(format!("壁纸库中已有多个 ID 为 {id} 的包，请先解决冲突"));
    }
    Ok(matches.pop().map(|(entry, _)| entry))
}

fn library_at(root: &Path) -> Result<PathBuf, String> {
    let path = crate::storage::Preferences::load_checked(root)?.wallpapers(root);
    fs::create_dir_all(&path).map_err(|e| path_error("无法访问壁纸库", &path, e))?;
    fs::canonicalize(&path).map_err(|e| path_error("无法访问壁纸库", &path, e))
}

pub fn prepare(root: &Path, source: &Path) -> Result<Preparation, String> {
    prepare_with_trial(root, source, PreparedImport::verify_trial)
}

fn author_source(
    source: PathBuf,
    before: Vec<u8>,
    manifest: Value,
    id: &str,
) -> Result<(PathBuf, Vec<u8>, Value), String> {
    let Some(revision) = manifest.get(REVISION) else {
        return Ok((source, before, manifest));
    };
    let revision = revision
        .as_str()
        .filter(|value| valid_revision(value))
        .ok_or("复制的壁纸目录包含无效的发布版本")?;
    if manifest["version"] != 1 {
        return Err("复制的壁纸目录需要 version 为 1 的清单".into());
    }
    let versions = plain_directory(&source.join(VERSIONS))?;
    let snapshot = plain_directory(&versions.join(revision))?;
    if versions.parent() != Some(source.as_path()) || snapshot.parent() != Some(versions.as_path())
    {
        return Err("发布版本的资源目录超出所选壁纸文件夹".into());
    }
    // The wrapper only locates the published snapshot. Its paths, inactive
    // versions and any loose files are outside the author package.
    let bytes = read_small(&snapshot, &snapshot.join("manifest.json"))?;
    let author = parse_manifest(&bytes)?;
    if !author["id"]
        .as_str()
        .is_some_and(|author_id| author_id.eq_ignore_ascii_case(id))
    {
        return Err("发布版本的原始清单与壁纸 ID 不一致".into());
    }
    Ok((snapshot, bytes, author))
}

fn prepare_with_trial(
    root: &Path,
    source: &Path,
    verify: impl FnOnce(&PreparedImport) -> Result<(), String>,
) -> Result<Preparation, String> {
    let source = plain_directory(source)?;
    let manifest_path = source.join("manifest.json");
    if !manifest_path.is_file() {
        return Err(if source.join("project.json").is_file() {
            "该目录使用其他软件的项目格式，请选择 Action75 壁纸文件夹".into()
        } else {
            "所选文件夹缺少 manifest.json".into()
        });
    }
    let before = read_small(&source, &manifest_path)?;
    let manifest = parse_manifest(&before)?;
    let id = manifest["id"]
        .as_str()
        .filter(|id| valid_id(id))
        .ok_or("清单需要有效的 ID，最多 64 个英文字母、数字、连字符或下划线")?
        .to_owned();
    let library = library_at(root)?;
    let staging = library.join(STAGING);
    let staging = fs::canonicalize(&staging).unwrap_or(staging);
    if source.starts_with(&staging) || staging.starts_with(&source) {
        return Err("请单独选择作者的壁纸文件夹".into());
    }
    let stage = Stage::create(&library)?;
    let existing = find_installed(root, &library, &id)?;
    if let Some(installed) = &existing
        && source == installed.directory
        && before == installed.before
    {
        return Ok(Preparation::Installed(Receipt {
            id: installed.id.clone(),
            name: installed.name.clone(),
            updated: false,
        }));
    }
    let (source, before, manifest) = author_source(source, before, manifest, &id)?;
    let name = manifest["name"]
        .as_str()
        .filter(|name| {
            !name.trim().is_empty() && name.len() <= 256 && !name.chars().any(char::is_control)
        })
        .ok_or("清单需要可显示的壁纸名称，最多 256 字节")?
        .to_owned();
    validate_manifest(&manifest, &source)?;
    let native_entry = (manifest["kind"] == "native")
        .then(|| native_asset_path(&manifest, "entry", &["dll"]))
        .transpose()?;
    let package = stage.directory.join("wallpapers-native").join(&id);
    let media_path = media::extensions(manifest["kind"].as_str().unwrap_or_default())
        .map(|formats| resource_path(&manifest["background"], "background", formats))
        .transpose()?;
    copy_package(
        &source,
        &package,
        native_entry.as_deref(),
        media_path.as_deref(),
    )?;
    if read_small(&package, &package.join("manifest.json"))? != before {
        return Err("清单在导入时发生变化，请重新选择".into());
    }
    validate_manifest(&manifest, &package)?;
    if let Some(installed) = &existing {
        if identical(
            &package,
            &installed.original(),
            native_entry.as_deref(),
            media_path.as_deref(),
        ) {
            return Ok(Preparation::Installed(Receipt {
                id: installed.id.clone(),
                name: installed.name.clone(),
                updated: false,
            }));
        }
        if installed.revision.is_none() || installed.directory.parent() != Some(library.as_path()) {
            return Err(format!(
                "ID {id} 已用于内置壁纸或原有目录中的壁纸，请使用其他 ID"
            ));
        }
    }

    let mut prepared_manifest = manifest;
    prepared_manifest["id"] = json!(id);
    if prepared_manifest.get("preview").is_none() && package.join("preview.png").is_file() {
        prepared_manifest["preview"] = json!("preview.png");
    }
    let prepared = Box::new(PreparedImport {
        stage,
        root: root.to_owned(),
        library,
        package,
        manifest: prepared_manifest,
        id,
        name,
        existing,
    });
    verify(&prepared)?;
    Ok(Preparation::Ready(prepared))
}

impl PreparedImport {
    fn verify_trial(&self) -> Result<(), String> {
        self.verify_trial_cancellable(None)
    }

    fn verify_trial_cancellable(
        &self,
        cancel: Option<&std::sync::atomic::AtomicBool>,
    ) -> Result<(), String> {
        let stage = &self.stage;
        let native = self.manifest["kind"] == "native";
        let child = trial::run(
            &crate::player(&self.root),
            &stage.directory,
            &stage.revision,
            native,
            (self.manifest["renderer"] == "d3d12" || self.manifest["kind"] == "video")
                && crate::storage::Preferences::load(&self.root).gpu_preference
                    == crate::storage::GpuPreference::Integrated,
            cancel,
        )?;
        let report_path = stage.directory.join("trial-result.json");
        let result = read_small(&stage.directory, &report_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
        let frames = result
            .as_ref()
            .and_then(|r| r["framesRendered"].as_u64())
            .unwrap_or_default();
        crate::storage::log(
            &self.root,
            if child.status == 0 { "INFO" } else { "WARN" },
            &format!(
                "wallpaper import trial id={} pid={} exited={} timeout={} frames={frames} integrity={} peakBytes={} processes={}",
                self.id,
                child.pid,
                child.status,
                child.timed_out,
                child.integrity,
                child.peak_bytes,
                child.processes,
            ),
        );
        if child.timed_out {
            return Err("壁纸试播超过 8 秒，已结束试播。请检查资源，或减少素材后重试".into());
        }
        if child.status != 0
            || frames == 0
            || result
                .as_ref()
                .is_none_or(|r| r["ok"] != true || r["playerPid"] != child.pid)
        {
            return Err(result
                .as_ref()
                .and_then(|r| r["error"].as_str())
                .map(str::to_owned)
                .unwrap_or_else(|| "壁纸试播进程提前退出，请检查效果文件和素材".into()));
        }
        if native && child.processes < 2 {
            return Err("原生壁纸试播需要由隔离 Worker 完成".into());
        }
        Ok(())
    }
}

fn write_json(path: &Path, value: &Value) -> Result<(), String> {
    let mut file = File::create(path).map_err(|e| path_error("无法写入清单", path, e))?;
    serde_json::to_writer_pretty(&mut file, value).map_err(|e| e.to_string())?;
    file.flush()
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string())
}

fn move_directory(source: &Path, target: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::PCWSTR;
    let a: Vec<_> = source.as_os_str().encode_wide().chain(Some(0)).collect();
    let b: Vec<_> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    unsafe {
        MoveFileExW(
            PCWSTR(a.as_ptr()),
            PCWSTR(b.as_ptr()),
            MOVEFILE_WRITE_THROUGH,
        )
    }
    .map_err(|e| format!("无法发布到壁纸库，{e}"))
}

impl PreparedImport {
    pub fn update_names(&self) -> Option<(&str, &str)> {
        self.existing
            .as_ref()
            .map(|existing| (existing.name.as_str(), self.name.as_str()))
    }

    pub fn publish(mut self) -> Result<Receipt, String> {
        if library_at(&self.root)? != self.library
            || find_installed(&self.root, &self.library, &self.id)? != self.existing
        {
            return Err("壁纸库在确认期间发生变化，请重新导入".into());
        }
        let revision = &self.stage.revision;
        let prefix = format!("{VERSIONS}/{revision}");
        for key in ["background", "effect", "entry", "preview", "settings"] {
            if let Some(path) = self.manifest[key].as_str() {
                self.manifest[key] = json!(format!("{prefix}/{path}"));
            }
        }
        if let Some(effects) = self
            .manifest
            .get_mut("effects")
            .and_then(Value::as_array_mut)
        {
            for effect in effects {
                *effect = json!(format!("{prefix}/{}", effect.as_str().unwrap()));
            }
        }
        self.manifest[REVISION] = json!(revision);
        let updated = self.existing.is_some();
        if let Some(existing) = &self.existing {
            self.id.clone_from(&existing.id);
            self.manifest["id"] = json!(self.id);
            let versions = private_directory(&existing.directory.join(VERSIONS))?;
            if versions.parent() != Some(existing.directory.as_path()) {
                return Err("版本目录超出壁纸包".into());
            }
            let new = versions.join(revision);
            let manifest = self.stage.directory.join("publish.json");
            write_json(&manifest, &self.manifest)?;
            move_directory(&self.package, &new)?;
            if let Err(error) =
                crate::atomic_replace(&manifest, &existing.directory.join("manifest.json"))
            {
                remove_owned_directory(&versions, &new);
                return Err(format!("无法替换清单，旧版本仍可使用。{error}"));
            }
            prune_versions(
                &self.root,
                &self.id,
                &versions,
                revision,
                existing.revision.as_deref(),
            );
        } else {
            let envelope = self.stage.directory.join("publish");
            let versions = envelope.join(VERSIONS);
            fs::create_dir_all(&versions)
                .map_err(|e| path_error("无法准备壁纸目录", &versions, e))?;
            move_directory(&self.package, &versions.join(revision))?;
            write_json(&envelope.join("manifest.json"), &self.manifest)?;
            move_directory(&envelope, &self.library.join(&self.id))?;
        }
        Ok(Receipt {
            id: self.id,
            name: self.name,
            updated,
        })
    }
}

fn prune_versions(root: &Path, id: &str, folder: &Path, current: &str, previous: Option<&str>) {
    let Ok(response) = crate::command(
        root,
        json!({"action": "getProtectedImportRevisions", "id": id}),
    ) else {
        return;
    };
    let Some(protected) = response.as_array().filter(|versions| {
        versions
            .iter()
            .all(|revision| revision.as_str().is_some_and(valid_revision))
    }) else {
        return;
    };
    if let Ok(entries) = fs::read_dir(folder) {
        for item in entries.flatten() {
            let name = item.file_name();
            if let Some(name) = name.to_str().filter(|name| valid_revision(name))
                && name != current
                && Some(name) != previous
                && !protected
                    .iter()
                    .any(|revision| revision.as_str() == Some(name))
            {
                remove_owned_directory(folder, &item.path());
            }
        }
    }
}

#[cfg(test)]
mod wallpaper_library_regression {
    use super::*;

    struct Fixture {
        temp: PathBuf,
        root: PathBuf,
    }

    impl Fixture {
        fn new() -> Self {
            let temp = fs::canonicalize(std::env::temp_dir()).unwrap();
            let stamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let root = temp.join(format!("action75-library-{}-{stamp}", std::process::id()));
            fs::create_dir(&root).unwrap();
            Self { temp, root }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            remove_owned_directory(&self.temp, &self.root);
        }
    }

    fn author(directory: &Path, id: &str, name: &str) {
        fs::create_dir_all(directory.join("images")).unwrap();
        fs::write(
            directory.join("images/background.png"),
            b"author background",
        )
        .unwrap();
        fs::write(directory.join("images/atlas.png"), b"author atlas").unwrap();
        write_json(&directory.join("manifest.json"), &json!({
            "version": 1, "id": id, "name": name, "kind": "spriteBurst", "columns": 2, "rows": 1,
            "duration": 1, "diameter": 120, "background": "images/background.png", "effect": "images/atlas.png"
        })).unwrap();
    }

    #[test]
    fn delete_checks_library_and_wallpaper_identity() {
        let fixture = Fixture::new();
        let installed = fixture.root.join("wallpapers-native/sample");
        let source = fixture.root.join("original/sample");
        author(&installed, "sample", "Sample");
        author(&source, "sample", "Sample");
        deletion_target(&fixture.root, "sample", &installed).unwrap();
        assert!(deletion_target(&fixture.root, "sample", &source).is_err());
        assert!(deletion_target(&fixture.root, "other", &installed).is_err());
    }

    #[test]
    fn damaged_preferences_cannot_redirect_import_or_deletion_to_the_default_library() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        author(&source, "preferences-check", "Preferences check");
        fs::create_dir(fixture.root.join("data")).unwrap();
        let settings = fixture.root.join("data/app-settings.json");
        fs::write(&settings, b"{").unwrap();

        let result = prepare_with_trial(&fixture.root, &source, |_| Ok(()));
        assert!(
            result.is_err(),
            "a damaged library configuration must block import"
        );
        assert!(!fixture.root.join("wallpapers-native").exists());
        assert_eq!(fs::read(&settings).unwrap(), b"{");

        let installed = fixture.root.join("wallpapers-native/preferences-check");
        author(&installed, "preferences-check", "Preferences check");
        assert!(deletion_target(&fixture.root, "preferences-check", &installed).is_err());
        assert!(installed.join("manifest.json").is_file());
    }

    #[test]
    fn publication_rechecks_damaged_preferences_and_cleans_the_staged_package() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        author(&source, "preferences-check", "Preferences check");
        let pending = ready(prepare_with_trial(&fixture.root, &source, |_| Ok(())).unwrap());
        let stage = pending.stage.directory.clone();
        fs::create_dir(fixture.root.join("data")).unwrap();
        fs::write(fixture.root.join("data/app-settings.json"), b"{").unwrap();

        assert!(pending.publish().is_err());
        assert!(
            !fixture
                .root
                .join("wallpapers-native/preferences-check")
                .exists()
        );
        assert!(!stage.exists());
        assert_eq!(
            fs::read(fixture.root.join("data/app-settings.json")).unwrap(),
            b"{"
        );
    }

    #[test]
    #[ignore = "exercises Windows Recycle Bin on a disposable installed wallpaper"]
    fn delete_recycles_only_the_installed_folder() {
        let fixture = Fixture::new();
        let installed = fixture.root.join("wallpapers-native/sample");
        let source = fixture.root.join("original/sample");
        author(&installed, "sample", "Sample");
        author(&source, "sample", "Sample");
        delete_pack(&fixture.root, "sample", &installed).unwrap();
        assert!(!installed.exists());
        assert!(source.join("manifest.json").is_file());
    }

    fn ready(preparation: Preparation) -> Box<PreparedImport> {
        match preparation {
            Preparation::Ready(pack) => pack,
            Preparation::Installed(_) => panic!("expected a staged import"),
        }
    }

    fn published_revision(directory: &Path) -> String {
        let bytes = fs::read(directory.join("manifest.json")).unwrap();
        parse_manifest(&bytes).unwrap()[REVISION]
            .as_str()
            .unwrap()
            .to_owned()
    }

    const NATIVE_ENTRY: &str = "bin/sparks.dll";

    fn native_author(directory: &Path) -> Value {
        fs::create_dir_all(directory.join("bin")).unwrap();
        fs::create_dir_all(directory.join("resources")).unwrap();
        fs::write(directory.join(NATIVE_ENTRY), b"native test module").unwrap();
        fs::write(directory.join("resources/pixel.cso"), b"shader bytecode").unwrap();
        fs::write(directory.join("resources/texture.dds"), b"texture data").unwrap();
        write_json(
            &directory.join("resources/options.json"),
            &json!({
                "version":1, "presentation":"layered", "groups":[{
                    "id":"appearance", "label":"Appearance", "controls":[{
                        "id":"glow", "label":"Glow", "type":"slider",
                        "default":1, "min":0.2, "max":2, "step":0.1
                    }]
                }]
            }),
        )
        .unwrap();
        let manifest = json!({
            "version": 1, "id": "native-author", "name": "Native Author", "kind": "native",
            "abi": 1, "entry": NATIVE_ENTRY, "settings": "resources/options.json"
        });
        write_json(&directory.join("manifest.json"), &manifest).unwrap();
        manifest
    }

    #[test]
    fn native_import_versions_assets_and_browses_without_loading_modules() {
        let fixture = Fixture::new();
        let runtime = fixture.root.join("runtime");
        let author = fixture.root.join("author");
        native_author(&author);
        let staged = ready(
            prepare_with_trial(&runtime, &author, |pack| {
                assert_eq!(pack.manifest["entry"], NATIVE_ENTRY);
                assert!(pack.package.join(NATIVE_ENTRY).is_file());
                assert!(pack.package.join("resources/pixel.cso").is_file());
                assert!(pack.package.join("resources/texture.dds").is_file());
                Ok(())
            })
            .unwrap(),
        );
        assert!(!staged.publish().unwrap().updated);
        let installed = runtime.join("wallpapers-native/native-author");
        let revision = published_revision(&installed);
        let wrapper = parse_manifest(&fs::read(installed.join("manifest.json")).unwrap()).unwrap();
        let prefix = format!("{VERSIONS}/{revision}");
        assert_eq!(wrapper["entry"], format!("{prefix}/{NATIVE_ENTRY}"));
        assert_eq!(
            wrapper["settings"],
            format!("{prefix}/resources/options.json")
        );
        assert_eq!(
            fs::read(installed.join(format!("{prefix}/{NATIVE_ENTRY}"))).unwrap(),
            b"native test module"
        );
        let mut relative_wrapper = wrapper.clone();
        relative_wrapper["entry"] = json!(r"bin\sparks.dll");
        relative_wrapper["settings"] = json!("resources/options.json");
        assert!(native_catalog_available(&installed, &relative_wrapper));
        let other_revision = format!("v-{}", "0".repeat(40));
        let inactive = installed.join(VERSIONS).join(&other_revision).join("bin");
        fs::create_dir_all(&inactive).unwrap();
        fs::write(inactive.join("sparks.dll"), b"inactive").unwrap();
        relative_wrapper["entry"] = json!(format!("{VERSIONS}/{other_revision}/{NATIVE_ENTRY}"));
        assert!(!native_catalog_available(&installed, &relative_wrapper));

        let catalogue = crate::local_packs(&runtime);
        assert_eq!(catalogue.len(), 1);
        assert_eq!(catalogue[0]["supported"], true);
        assert_eq!(catalogue[0]["schemaAvailable"], true);
        assert!(matches!(
            prepare_with_trial(&runtime, &author, |_| panic!("identical DLL pack reached trial"))
                .unwrap(),
            Preparation::Installed(receipt) if !receipt.updated
        ));

        let copied = fixture.root.join("copied");
        copy_package(
            &plain_directory(&installed.join(&prefix)).unwrap(),
            &copied.join(&prefix),
            Some(Path::new(NATIVE_ENTRY)),
            None,
        )
        .unwrap();
        fs::copy(
            installed.join("manifest.json"),
            copied.join("manifest.json"),
        )
        .unwrap();
        let other = fixture.root.join("other-runtime");
        let recovered = ready(
            prepare_with_trial(&other, &copied, |pack| {
                assert_eq!(pack.manifest["entry"], NATIVE_ENTRY);
                assert_eq!(
                    fs::read(pack.package.join(NATIVE_ENTRY)).unwrap(),
                    b"native test module"
                );
                assert!(!pack.package.join(VERSIONS).exists());
                Ok(())
            })
            .unwrap(),
        );
        assert!(!recovered.publish().unwrap().updated);
    }

    #[test]
    #[ignore = "requires ACTION75_TEST_PLAYER pointing to a release player with its sibling worker"]
    fn native_nested_entry_real_trial_and_publish() {
        let fixture = Fixture::new();
        let exe =
            PathBuf::from(std::env::var_os("ACTION75_TEST_PLAYER").expect("ACTION75_TEST_PLAYER"));
        let runtime = fixture.root.join("runtime");
        let installed_player = crate::player(&runtime);
        fs::create_dir_all(installed_player.parent().unwrap()).unwrap();
        fs::copy(&exe, &installed_player).unwrap();
        let worker = "Action75WallpaperWorker.exe";
        fs::copy(
            exe.with_file_name(worker),
            installed_player.with_file_name(worker),
        )
        .unwrap();
        let author = fixture.root.join("author");
        native_author(&author);
        // A normal relative asset path can pass MAX_PATH after staging under
        // .action75-import or publishing it in .action75-versions.
        let texture = format!("resources/{}.json", "watercolor".repeat(17));
        fs::write(author.join(&texture), b"{}").unwrap();
        let material = "resources/paint.efkmatd";
        fs::write(author.join(material), b"compiled material").unwrap();
        fs::copy(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../tools/fixtures/key-game-abi1/pack/wallpaper.dll"),
            author.join(NATIVE_ENTRY),
        )
        .unwrap();
        let receipt = ready(prepare(&runtime, &author).unwrap())
            .publish()
            .unwrap();
        let installed = runtime.join("wallpapers-native").join(receipt.id);
        let wrapper = parse_manifest(&fs::read(installed.join("manifest.json")).unwrap()).unwrap();
        assert!(native_catalog_available(&installed, &wrapper));
        let revision = published_revision(&installed);
        assert_eq!(
            fs::read(
                installed
                    .join(".action75-versions")
                    .join(revision)
                    .join(material)
            )
            .unwrap(),
            b"compiled material"
        );
        let entry = native_asset_path(&wrapper, "entry", &["dll"]).unwrap();
        assert_eq!(
            fs::read(installed.join(entry)).unwrap(),
            fs::read(author.join(NATIVE_ENTRY)).unwrap()
        );

        // Exercise the normal delete job against an isolated running player.
        let mapping = Path::new("existing-resources/finalmouse/action75-ue-keymap.json");
        fs::create_dir_all(runtime.join(mapping).parent().unwrap()).unwrap();
        fs::copy(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../..")
                .join(mapping),
            runtime.join(mapping),
        )
        .unwrap();
        let pipe = format!("Action75DeleteTest-{}", std::process::id());
        fs::create_dir_all(runtime.join("data")).unwrap();
        fs::write(runtime.join("data/player-pipe.txt"), &pipe).unwrap();
        let mut child = std::process::Command::new(&installed_player)
            .arg("--root")
            .arg(&runtime)
            .arg("--state-dir")
            .arg(runtime.join("test-state"))
            .arg("--pipe")
            .arg(pipe)
            .arg("--headless-test")
            .spawn()
            .unwrap();
        let result = (|| -> Result<(), String> {
            let request = |value| crate::command(&runtime, value);
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(8);
            while request(json!({"action":"status"})).is_err() {
                if std::time::Instant::now() > deadline {
                    return Err("test player startup timed out".into());
                }
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            let status = request(json!({"action":"selectPack","id":"native-author"}))?;
            if status["packId"] != "native-author" {
                return Err("test pack did not activate".into());
            }
            delete_pack(&runtime, "native-author", &installed)?;
            if request(json!({"action":"status"}))?["packId"] != ""
                || !request(json!({"action":"listPacks"}))?
                    .as_array()
                    .unwrap()
                    .is_empty()
            {
                return Err("deleted wallpaper remained active or in the gallery".into());
            }
            Ok(())
        })();
        let _ = child.kill();
        child.wait().unwrap();
        result.unwrap();
        assert!(author.join(NATIVE_ENTRY).is_file());
    }

    #[test]
    fn native_import_keeps_previous_revision_on_trial_or_publication_failure() {
        let fixture = Fixture::new();
        let runtime = fixture.root.join("runtime");
        let author = fixture.root.join("author");
        let mut manifest = native_author(&author);
        ready(prepare_with_trial(&runtime, &author, |_| Ok(())).unwrap())
            .publish()
            .unwrap();
        let installed = runtime.join("wallpapers-native/native-author");
        let previous = published_revision(&installed);
        fs::write(author.join(NATIVE_ENTRY), b"updated test module").unwrap();
        manifest["name"] = json!("Native Author 2");
        write_json(&author.join("manifest.json"), &manifest).unwrap();

        let trial_failed = prepare_with_trial(&runtime, &author, |_| Err("trial failed".into()));
        assert_eq!(trial_failed.err().as_deref(), Some("trial failed"));
        assert_eq!(published_revision(&installed), previous);

        let pending = ready(prepare_with_trial(&runtime, &author, |_| Ok(())).unwrap());
        assert_eq!(
            pending.update_names(),
            Some(("Native Author", "Native Author 2"))
        );
        let old_manifest = installed.join("manifest.json");
        let locked = OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ.0)
            .open(&old_manifest)
            .unwrap();
        let rejected_revision = pending.stage.revision.clone();
        assert!(pending.publish().err().unwrap().contains("旧版本仍可使用"));
        drop(locked);
        assert_eq!(published_revision(&installed), previous);
        assert!(!installed.join(VERSIONS).join(rejected_revision).exists());

        let update = ready(prepare_with_trial(&runtime, &author, |_| Ok(())).unwrap());
        assert!(update.publish().unwrap().updated);
        let current = published_revision(&installed);
        assert_ne!(current, previous);
        for (revision, contents) in [
            (previous, b"native test module".as_slice()),
            (current, b"updated test module".as_slice()),
        ] {
            assert_eq!(
                fs::read(installed.join(VERSIONS).join(&revision).join(NATIVE_ENTRY)).unwrap(),
                contents
            );
        }
    }

    #[test]
    fn desktop_opacity_requires_a_dx12_percentage_slider() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        let mut manifest = native_author(&source);
        manifest["desktopOpacityControl"] = json!("opacity");
        let options = source.join("resources/options.json");
        let slider = json!({"id":"opacity", "type":"slider", "label":"Opacity",
                            "min":0, "max":100, "default":100, "step":1});
        let set_controls = |controls: Value| {
            write_json(
                &options,
                &json!({"version":1,"groups":[{"controls":controls}]}),
            )
            .unwrap()
        };
        set_controls(json!([slider]));
        assert!(validate_manifest(&manifest, &source).is_err());
        manifest["abi"] = json!(2);
        manifest["renderer"] = json!("d3d12");
        validate_manifest(&manifest, &source).unwrap();
        set_controls(json!([slider, slider]));
        assert!(validate_manifest(&manifest, &source).is_err());
        set_controls(json!([]));
        assert!(validate_manifest(&manifest, &source).is_err());
    }

    #[test]
    fn dx12_manifest_survives_import_and_catalogue_scan() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        let runtime = fixture.root.join("runtime");
        let mut manifest = native_author(&source);
        manifest["abi"] = json!(2);
        manifest["renderer"] = json!("d3d12");
        write_json(&source.join("manifest.json"), &manifest).unwrap();
        fs::write(source.join("lighting.dxil"), b"test shader asset").unwrap();
        ready(prepare_with_trial(&runtime, &source, |_| Ok(())).unwrap())
            .publish()
            .unwrap();
        let installed = runtime.join("wallpapers-native/native-author");
        let stored: Value =
            serde_json::from_slice(&fs::read(installed.join("manifest.json")).unwrap()).unwrap();
        assert_eq!(stored["renderer"], "d3d12");
        assert!(native_catalog_available(&installed, &stored));
        manifest["abi"] = json!(1);
        assert!(validate_manifest(&manifest, &source).is_err());
        manifest["abi"] = json!(2);
        manifest["renderer"] = json!("d3d11");
        assert!(validate_manifest(&manifest, &source).is_err());
    }

    #[test]
    fn native_import_rejects_wrong_abi_paths_and_unapproved_code_before_trial() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        let runtime = fixture.root.join("runtime");
        let manifest = native_author(&source);
        for (field, value, expected) in [
            ("abi", json!(2), "ABI 1"),
            ("entry", json!("../wallpaper.dll"), "entry"),
            ("entry", json!("alternate.dll"), "缺少资源"),
            ("entry", json!("bin/../bin/sparks.dll"), "entry"),
            ("entry", json!("bin/sparks.dll:other"), "entry"),
            ("settings", json!("../options.json"), "包内相对路径"),
            ("settings", json!("missing.json"), "缺少资源"),
            ("runtimeControls", json!(true), "runtimeControls"),
        ] {
            let mut candidate = manifest.clone();
            candidate[field] = value;
            assert!(
                validate_manifest(&candidate, &source)
                    .unwrap_err()
                    .contains(expected)
            );
        }
        fs::write(source.join("resources/options.json"), b"[1]").unwrap();
        assert!(
            validate_manifest(&manifest, &source)
                .unwrap_err()
                .contains("groups")
        );
        native_author(&source);
        let reject = || {
            prepare_with_trial(&runtime, &source, |_| {
                panic!("unsafe package reached trial")
            })
            .err()
            .expect("unsafe package was accepted")
        };
        for file in [
            "extra.dll",
            "resources/wallpaper.dll",
            "Action75WallpaperWorker.exe",
            "runtime.hlsl",
        ] {
            let path = source.join(file);
            fs::write(&path, b"extra file").unwrap();
            assert!(reject().contains(&path.file_name().unwrap().to_string_lossy().into_owned()));
            fs::remove_file(path).unwrap();
        }
        let outside = fixture.root.join("outside.dds");
        fs::write(&outside, b"external texture").unwrap();
        fs::hard_link(&outside, source.join("resources/link.dds")).unwrap();
        assert!(reject().contains("硬链接"));
        assert!(!runtime.join("wallpapers-native/native-author").exists());
    }

    #[test]
    fn native_import_requires_the_installed_worker_before_starting_trial() {
        let fixture = Fixture::new();
        let source = fixture.root.join("author");
        let runtime = fixture.root.join("runtime");
        native_author(&source);
        let rejected = prepare(&runtime, &source).err().expect("missing worker");
        assert!(
            rejected.contains("Action75WallpaperWorker.exe"),
            "{rejected}"
        );
        assert!(!runtime.join("wallpapers-native/native-author").exists());
    }

    fn fireworks_selection_default(settings: &mut Value) -> &mut Value {
        let group = settings["groups"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .find(|group| group["presentation"] == "variant-playback")
            .unwrap();
        let selection = group["playback"]["selection"].as_str().unwrap().to_owned();
        &mut group["controls"]
            .as_array_mut()
            .unwrap()
            .iter_mut()
            .find(|control| control["id"] == selection)
            .unwrap()["default"]
    }

    fn fireworks_author(directory: &Path) -> (Value, Value) {
        let mut manifest = parse_manifest(include_bytes!(
            "../../../tools/fixtures/builtins-abi1/night-fireworks/manifest.json"
        ))
        .unwrap();
        manifest["kind"] = json!("fireworks");
        manifest.as_object_mut().unwrap().remove("entry");
        manifest.as_object_mut().unwrap().remove("abi");
        manifest["id"] = json!("author-fireworks");
        manifest["name"] = json!("Author fireworks");
        manifest["settings"] = json!("variants/settings.json");
        manifest["preview"] = json!("art/cover.png");
        let mut settings: Value = serde_json::from_slice(include_bytes!(
            "../../../tools/fixtures/builtins-abi1/night-fireworks/settings.json"
        ))
        .unwrap();
        *fireworks_selection_default(&mut settings) = json!(8);
        fs::create_dir_all(directory.join("variants")).unwrap();
        fs::create_dir_all(directory.join("art")).unwrap();
        write_json(&directory.join("manifest.json"), &manifest).unwrap();
        write_json(&directory.join("variants/settings.json"), &settings).unwrap();
        fs::write(
            directory.join("art/cover.png"),
            include_bytes!("../../../tools/fixtures/builtins-abi1/night-fireworks/preview.png"),
        )
        .unwrap();
        (manifest, settings)
    }

    #[test]
    fn fireworks_import_preserves_settings_across_publication_updates_and_copies() {
        let fixture = Fixture::new();
        let runtime = fixture.root.join("runtime");
        let source = fixture.root.join("source");
        let (mut manifest, mut settings) = fireworks_author(&source);
        let staged = ready(
            prepare_with_trial(&runtime, &source, |pack| {
                assert_eq!(pack.manifest["kind"], "fireworks");
                assert_eq!(pack.manifest["settings"], "variants/settings.json");
                assert_eq!(
                    fs::read(pack.package.join("variants/settings.json")).unwrap(),
                    fs::read(source.join("variants/settings.json")).unwrap()
                );
                assert!(pack.package.join("art/cover.png").is_file());
                Ok(())
            })
            .unwrap(),
        );
        assert!(staged.update_names().is_none());
        let receipt = staged.publish().unwrap();
        assert_eq!(receipt.id, "author-fireworks");
        assert!(!receipt.updated);

        let destination = runtime.join("wallpapers-native/author-fireworks");
        let first = published_revision(&destination);
        let again =
            prepare_with_trial(&runtime, &source, |_| panic!("duplicate reached trial")).unwrap();
        assert!(matches!(again, Preparation::Installed(receipt) if !receipt.updated));

        manifest["name"] = json!("Updated fireworks");
        *fireworks_selection_default(&mut settings) = json!(4);
        write_json(&source.join("manifest.json"), &manifest).unwrap();
        write_json(&source.join("variants/settings.json"), &settings).unwrap();
        let failed_trial = prepare_with_trial(&runtime, &source, |_| Err("试播失败".into()));
        assert_eq!(failed_trial.err().as_deref(), Some("试播失败"));
        assert_eq!(published_revision(&destination), first);

        let update = ready(prepare_with_trial(&runtime, &source, |_| Ok(())).unwrap());
        assert_eq!(
            update.update_names(),
            Some(("Author fireworks", "Updated fireworks"))
        );
        assert_eq!(published_revision(&destination), first);
        assert!(update.publish().unwrap().updated);
        let latest = published_revision(&destination);
        assert_ne!(latest, first);
        let published =
            parse_manifest(&fs::read(destination.join("manifest.json")).unwrap()).unwrap();
        assert_eq!(
            published["settings"],
            format!("{VERSIONS}/{latest}/variants/settings.json")
        );
        assert_eq!(
            published["preview"],
            format!("{VERSIONS}/{latest}/art/cover.png")
        );
        let retained = destination.join(VERSIONS).join(first);
        let mut original_settings: Value =
            serde_json::from_slice(&fs::read(retained.join("variants/settings.json")).unwrap())
                .unwrap();
        assert_eq!(*fireworks_selection_default(&mut original_settings), 8);

        let other_runtime = fixture.root.join("other-runtime");
        let imported = ready(
            prepare_with_trial(&other_runtime, &destination, |pack| {
                assert_eq!(pack.manifest["settings"], "variants/settings.json");
                let staged_settings: Value = serde_json::from_slice(
                    &fs::read(pack.package.join("variants/settings.json")).unwrap(),
                )
                .unwrap();
                assert_eq!(staged_settings, settings);
                assert!(!pack.package.join(VERSIONS).exists());
                Ok(())
            })
            .unwrap(),
        );
        assert!(!imported.publish().unwrap().updated);
    }

    #[test]
    fn fireworks_import_rejects_other_renderer_fields_and_invalid_resource_paths() {
        let fixture = Fixture::new();
        let source = fixture.root.join("source");
        let (manifest, _) = fireworks_author(&source);
        for (field, value) in [
            ("effect", json!("effect.efkefc")),
            ("effects", json!(["effect.efkefc"])),
            ("background", json!("art/cover.png")),
            ("canvasEffect", json!("canvas.efkefc")),
            ("runtimeControls", json!(false)),
            ("layout", json!("layout.json")),
            ("script", json!("author.js")),
        ] {
            let mut candidate = manifest.clone();
            candidate[field] = value;
            assert!(
                validate_manifest(&candidate, &source)
                    .unwrap_err()
                    .contains(field)
            );
        }

        for (field, path, expected) in [
            ("settings", "../outside.json", "包内相对路径"),
            ("settings", "art/cover.png", "资源格式"),
            ("settings", "variants/missing.json", "缺少资源"),
            ("preview", "art/../cover.png", "包内相对路径"),
            ("preview", "variants/settings.json", "资源格式"),
        ] {
            let mut candidate = manifest.clone();
            candidate[field] = json!(path);
            assert!(
                validate_manifest(&candidate, &source)
                    .unwrap_err()
                    .contains(expected)
            );
        }
    }

    #[test]
    fn copies_only_the_published_snapshot_and_recognizes_repeated_imports() {
        let fixture = Fixture::new();
        let first = fixture.root.join("first-installation");
        let second = fixture.root.join("second-installation");
        let material = fixture.root.join("author-source");
        let shared = fixture.root.join("copied-wallpaper");
        author(&material, "author-transport", "Author package");
        ready(prepare_with_trial(&first, &material, |_| Ok(())).unwrap())
            .publish()
            .unwrap();

        let installed = first.join("wallpapers-native/author-transport");
        let revision = published_revision(&installed);
        copy_package(&plain_directory(&installed).unwrap(), &shared, None, None).unwrap();
        let retired = shared.join(VERSIONS).join(format!("v-{}", "9".repeat(40)));
        fs::create_dir(&retired).unwrap();
        fs::write(
            retired.join("retired-private.png"),
            b"private older revision",
        )
        .unwrap();
        fs::write(shared.join("loose-private.png"), b"private loose resource").unwrap();
        let wrapper = shared.join("manifest.json");
        let mut outer = parse_manifest(&fs::read(&wrapper).unwrap()).unwrap();
        outer["id"] = json!("AUTHOR-TRANSPORT");
        outer["effect"] = json!("loose-private.png");
        write_json(&wrapper, &outer).unwrap();

        let pending = ready(
            prepare_with_trial(&second, &shared, |pack| {
                assert_eq!(pack.id, "AUTHOR-TRANSPORT");
                let staged = pack.package.join("manifest.json");
                assert_eq!(
                    fs::read(&staged).unwrap(),
                    fs::read(material.join("manifest.json")).unwrap()
                );
                assert_eq!(
                    fs::read(pack.package.join("images/atlas.png")).unwrap(),
                    b"author atlas"
                );
                assert!(!pack.package.join("loose-private.png").exists());
                assert!(!pack.package.join(VERSIONS).exists());
                Ok(())
            })
            .unwrap(),
        );
        pending.publish().unwrap();
        let destination = second.join("wallpapers-native/AUTHOR-TRANSPORT");
        let new_revision = published_revision(&destination);
        assert_ne!(new_revision, revision);
        assert!(
            !destination
                .join(VERSIONS)
                .join(&new_revision)
                .join("loose-private.png")
                .exists()
        );
        let again =
            prepare_with_trial(&second, &shared, |_| panic!("duplicate reached trial")).unwrap();
        assert!(
            matches!(again, Preparation::Installed(receipt) if receipt.id == "AUTHOR-TRANSPORT")
        );
        assert!(retired.join("retired-private.png").is_file());
        assert!(shared.join("loose-private.png").is_file());
    }

    #[test]
    fn copied_versions_require_a_matching_validated_author_manifest() {
        let fixture = Fixture::new();
        let installed = fixture.root.join("installation");
        let shared = fixture.root.join("copy");
        fs::create_dir(&shared).unwrap();
        let wrapper = shared.join("manifest.json");
        let revision = format!("v-{}", "1".repeat(40));
        let snapshot = shared.join(VERSIONS).join(&revision);
        author(&snapshot, "different-id", "Different package");
        let mut outer = json!({"version": 1, "id": "copied-author", REVISION: "../outside"});
        write_json(&wrapper, &outer).unwrap();
        let reject = || {
            prepare_with_trial(&installed, &shared, |_| {
                panic!("invalid copy reached trial")
            })
            .err()
            .expect("copied package should be refused")
        };
        assert!(reject().contains("无效的发布版本"));
        outer[REVISION] = json!(revision);
        write_json(&wrapper, &outer).unwrap();
        assert!(reject().contains("ID 不一致"));

        let original = snapshot.join("manifest.json");
        let mut inner = parse_manifest(&fs::read(&original).unwrap()).unwrap();
        inner["id"] = json!("copied-author");
        inner["effect"] = json!("../private.png");
        write_json(&original, &inner).unwrap();
        assert!(reject().contains("包内相对路径"));
        assert!(!installed.join("wallpapers-native/copied-author").exists());
    }

    #[test]
    fn changed_copied_author_still_requires_update_confirmation() {
        let fixture = Fixture::new();
        let first = fixture.root.join("origin");
        let second = fixture.root.join("destination");
        let original = fixture.root.join("original");
        let update = fixture.root.join("update");
        author(&original, "author-update", "Old author package");
        author(&update, "author-update", "New author package");
        ready(prepare_with_trial(&second, &original, |_| Ok(())).unwrap())
            .publish()
            .unwrap();
        ready(prepare_with_trial(&first, &update, |_| Ok(())).unwrap())
            .publish()
            .unwrap();

        let before = published_revision(&second.join("wallpapers-native/author-update"));
        let version = ready(
            prepare_with_trial(
                &second,
                &first.join("wallpapers-native/author-update"),
                |_| Ok(()),
            )
            .unwrap(),
        );
        assert_eq!(
            version.update_names(),
            Some(("Old author package", "New author package"))
        );
        drop(version);
        assert_eq!(
            published_revision(&second.join("wallpapers-native/author-update")),
            before
        );
    }

    #[test]
    fn bundled_id_conflicts_resolve_to_stock_or_the_managed_author() {
        let fixture = Fixture::new();
        let library = fixture.root.join("custom");
        let shipped = fixture.root.join("wallpapers-native");
        author(&library.join("afterimage"), "afterimage", "Migrated stock");
        author(&shipped.join("afterimage"), "afterimage", "Upgraded stock");
        author(
            &shipped.join("personal-only"),
            "personal-only",
            "Hidden from the custom library",
        );
        let library = fs::canonicalize(library).unwrap();
        let bundled = find_installed(&fixture.root, &library, "afterimage")
            .unwrap()
            .unwrap();
        assert_eq!(
            bundled.directory,
            fs::canonicalize(shipped.join("afterimage")).unwrap()
        );
        assert!(
            find_installed(&fixture.root, &library, "personal-only")
                .unwrap()
                .is_none()
        );

        let custom = library.join("afterimage/manifest.json");
        let mut managed = parse_manifest(&fs::read(&custom).unwrap()).unwrap();
        managed[REVISION] = json!(format!("v-{}", "2".repeat(40)));
        write_json(&custom, &managed).unwrap();
        let author = find_installed(&fixture.root, &library, "afterimage")
            .unwrap()
            .unwrap();
        assert_eq!(
            author.directory,
            fs::canonicalize(library.join("afterimage")).unwrap()
        );
        assert!(author.revision.is_some());
    }
}
