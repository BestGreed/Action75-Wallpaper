// Single-file imports are ordinary revisioned packages once published.
use super::*;
use sha2::{Digest, Sha256};
use std::io::BufReader;
use std::sync::atomic::{AtomicBool, Ordering};

const IMAGES: &[&str] = &["png", "jpg", "jpeg", "bmp", "tif", "tiff", "webp"];
const VIDEOS: &[&str] = &["mp4", "m4v", "mov", "wmv", "mkv", "webm"];
const BUFFER_SIZE: usize = 256 * 1024;

pub fn media_filter_pattern() -> String {
    IMAGES
        .iter()
        .chain(VIDEOS)
        .map(|ext| format!("*.{ext}"))
        .collect::<Vec<_>>()
        .join(";")
}

pub(super) fn extensions(kind: &str) -> Option<&'static [&'static str]> {
    match kind {
        "image" => Some(IMAGES),
        "video" => Some(VIDEOS),
        _ => None,
    }
}

pub(super) fn ensure_space(directory: &Path, size: u64) -> Result<(), String> {
    use windows::{Win32::Storage::FileSystem::GetDiskFreeSpaceExW, core::HSTRING};
    let mut available = 0;
    unsafe {
        GetDiskFreeSpaceExW(
            &HSTRING::from(directory.as_os_str()),
            Some(&mut available),
            None,
            None,
        )
    }
    .map_err(|e| format!("无法查询壁纸库可用空间，{e}"))?;
    // Keep room for the trial frame, thumbnail and manifests on the destination volume.
    if available < size.saturating_add(8 * 1024 * 1024) {
        return Err(format!(
            "壁纸库空间不足，导入需要至少 {} MiB",
            size.div_ceil(1024 * 1024).saturating_add(8)
        ));
    }
    Ok(())
}

fn check_cancel(cancelled: &AtomicBool) -> Result<(), String> {
    if cancelled.load(Ordering::Relaxed) {
        Err("已取消导入".into())
    } else {
        Ok(())
    }
}

fn settings() -> Value {
    let slider =
        |id: &str, label: &str, min: f64, max: f64, default: f64, step: f64, unit: &str| {
            json!({"id":id,"type":"slider","label":label,"min":min,"max":max,"default":default,
               "step":step,"unit":unit,"binding":{"target":format!("background.{id}")}})
        };
    json!({"version":1,"groups":[
        {"id":"layout","label":"画面布局","controls":[
            {"id":"fit","type":"select","label":"适应方式","default":0,
             "options":[{"value":0,"label":"填满屏幕"},{"value":1,"label":"完整显示"}],
             "binding":{"target":"background.fit"}},
            slider("zoom","缩放",0.25,4.,1.,0.01,"×"),
            slider("panX","水平位置",-1.,1.,0.,0.01,""),
            slider("panY","垂直位置",-1.,1.,0.,0.01,"")
        ]},
        {"id":"color","label":"色彩","controls":[
            slider("brightness","亮度",0.,4.,1.,0.01,"×"),
            slider("saturation","饱和度",0.,2.5,1.,0.01,"×"),
            slider("hue","色相",0.,360.,0.,1.,"°")
        ]}
    ]})
}

fn thumbnail(stage: &Path, dest: &Path) -> Result<(), String> {
    let frame = open_input(stage, &stage.join("trial-frame.png"))?;
    let reader = image::ImageReader::with_format(BufReader::new(frame), image::ImageFormat::Png);
    crate::thumbnail::decode(reader, 4096)
        .map_err(|e| format!("无法读取试播画面，{e}"))?
        .save(dest)
        .map_err(|e| format!("无法保存壁纸封面，{e}"))
}

fn stream_media(
    input: &mut File,
    output: &mut File,
    total: u64,
    cancelled: &AtomicBool,
    mut progress: impl FnMut(u64, u64),
) -> Result<String, String> {
    let mut buf = vec![0; BUFFER_SIZE];
    let mut hash = Sha256::new();
    let mut copied = 0;
    progress(0, total);
    loop {
        check_cancel(cancelled)?;
        let n = input
            .read(&mut buf)
            .map_err(|e| format!("读取媒体失败，{e}"))?;
        if n == 0 {
            break;
        }
        copied += n as u64;
        if copied > total {
            return Err("媒体文件在导入时发生变化".into());
        }
        output
            .write_all(&buf[..n])
            .map_err(|e| format!("复制媒体失败，{e}"))?;
        hash.update(&buf[..n]);
        progress(copied, total);
    }
    if copied != total {
        return Err("媒体文件在导入时发生变化".into());
    }
    output
        .sync_all()
        .map_err(|e| format!("无法保存媒体，{e}"))?;
    Ok(format!("media-{:.48}", format!("{:x}", hash.finalize())))
}

pub fn prepare_media(
    root: &Path,
    source: &Path,
    cancelled: &AtomicBool,
    progress: impl FnMut(u64, u64),
) -> Result<Preparation, String> {
    prepare(root, source, cancelled, progress, |prepared| {
        prepared.verify_trial_cancellable(Some(cancelled))
    })
}

fn prepare(
    root: &Path,
    source: &Path,
    cancelled: &AtomicBool,
    progress: impl FnMut(u64, u64),
    verify: impl FnOnce(&PreparedImport) -> Result<(), String>,
) -> Result<Preparation, String> {
    check_cancel(cancelled)?;
    let ext = source
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase();
    let kind = if IMAGES.contains(&ext.as_str()) {
        "image"
    } else if VIDEOS.contains(&ext.as_str()) {
        "video"
    } else {
        return Err("不支持此媒体文件格式".into());
    };
    let name: String = source
        .file_stem()
        .ok_or("媒体文件缺少名称")?
        .to_string_lossy()
        .chars()
        .filter(|ch| !ch.is_control())
        .scan(0, |size, ch| {
            *size += ch.len_utf8();
            (*size <= 256).then_some(ch)
        })
        .collect();
    let name = name.trim();
    let name = if name.is_empty() {
        "媒体壁纸"
    } else {
        name
    }
    .to_owned();
    let parent = plain_directory(source.parent().ok_or("无法读取媒体目录")?)?;
    let mut input = open_input(&parent, source)?;
    let total = input.metadata().map_err(|e| e.to_string())?.len();
    if total == 0 {
        return Err("媒体文件为空".into());
    }
    let library = library_at(root)?;
    if parent.starts_with(library.join(STAGING)) {
        return Err("请选择导入暂存目录以外的文件".into());
    }
    ensure_space(&library, total)?;
    let stage = Stage::create(&library)?;
    let asset = format!("media.{ext}");
    let pending = stage.directory.join("media.tmp");
    let mut output =
        File::create_new(&pending).map_err(|e| path_error("无法暂存媒体", &pending, e))?;
    let id = stream_media(&mut input, &mut output, total, cancelled, progress)?;
    drop(output);
    drop(input);
    check_cancel(cancelled)?;
    let existing = find_installed(root, &library, &id)?;
    if let Some(installed) = existing {
        let original = plain_directory(&installed.original())?;
        let manifest = parse_manifest(&read_small(&original, &original.join("manifest.json"))?)?;
        let media = resource_path(
            &manifest["background"],
            "background",
            extensions(kind).unwrap(),
        )?;
        let mut prior = open_input(&original, &original.join(media))?;
        // The ID alone cannot vouch for an imported third-party package or a modified file.
        let mut hash = Sha256::new();
        let mut buffer = vec![0; BUFFER_SIZE];
        loop {
            check_cancel(cancelled)?;
            let n = prior.read(&mut buffer).map_err(|e| e.to_string())?;
            if n == 0 {
                break;
            }
            hash.update(&buffer[..n]);
        }
        if manifest["kind"] != kind
            || id != format!("media-{:.48}", format!("{:x}", hash.finalize()))
        {
            return Err("同一媒体 ID 已被其他壁纸使用，请检查壁纸库".into());
        }
        return Ok(Preparation::Installed(Receipt {
            id: installed.id,
            name: installed.name,
            updated: false,
        }));
    }
    let package = stage.directory.join("wallpapers-native").join(&id);
    fs::create_dir_all(&package).map_err(|e| path_error("无法准备媒体目录", &package, e))?;
    fs::rename(&pending, package.join(&asset))
        .map_err(|e| path_error("无法暂存媒体", &pending, e))?;
    let manifest = json!({"version":1,"kind":kind,"id":id,"name":name,"background":asset,"settings":"settings.json"});
    write_json(&package.join("manifest.json"), &manifest)?;
    write_json(&package.join("settings.json"), &settings())?;
    let mut prepared = Box::new(PreparedImport {
        stage,
        root: root.to_owned(),
        library,
        package,
        manifest,
        id,
        name,
        existing: None,
    });
    verify(&prepared)?;
    check_cancel(cancelled)?;
    thumbnail(
        &prepared.stage.directory,
        &prepared.package.join("preview.png"),
    )?;
    prepared.manifest["preview"] = json!("preview.png");
    write_json(&prepared.package.join("manifest.json"), &prepared.manifest)?;
    check_cancel(cancelled)?;
    Ok(Preparation::Ready(prepared))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_png(path: &Path) {
        image::RgbImage::from_pixel(96, 36, image::Rgb([32, 148, 222]))
            .save(path)
            .unwrap();
    }

    #[test]
    fn single_file_publishes_a_portable_package_and_deduplicates_content() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        let source = root.join("蓝色山谷.PNG");
        make_png(&source);
        let cancel = AtomicBool::new(false);
        let Preparation::Ready(prepared) = prepare(
            root,
            &source,
            &cancel,
            |_, _| {},
            |pack| {
                assert_eq!(pack.manifest["kind"], "image");
                assert!(pack.manifest.get("preview").is_none());
                make_png(&pack.stage.directory.join("trial-frame.png"));
                Ok(())
            },
        )
        .unwrap() else {
            panic!()
        };
        let receipt = prepared.publish().unwrap();
        assert_eq!(receipt.name, "蓝色山谷");
        let published = root.join("wallpapers-native").join(&receipt.id);
        let manifest: Value =
            serde_json::from_slice(&fs::read(published.join("manifest.json")).unwrap()).unwrap();
        for key in ["background", "settings", "preview"] {
            assert!(published.join(manifest[key].as_str().unwrap()).is_file());
        }
        let schema: Value = serde_json::from_slice(
            &fs::read(published.join(manifest["settings"].as_str().unwrap())).unwrap(),
        )
        .unwrap();
        assert_eq!(schema, settings());
        let source2 = root.join("其他名字.png");
        fs::rename(&source, &source2).unwrap();
        let Preparation::Installed(duplicate) =
            prepare(root, &source2, &cancel, |_, _| {}, |_| panic!()).unwrap()
        else {
            panic!()
        };
        assert_eq!(duplicate.id, receipt.id);
        let another = tempfile::tempdir().unwrap();
        let Preparation::Ready(copy) =
            prepare_with_trial(another.path(), &published, |_| Ok(())).unwrap()
        else {
            panic!()
        };
        assert_eq!(copy.publish().unwrap().id, receipt.id);
    }

    #[test]
    fn cancel_and_failed_trial_drop_only_staging_and_keep_source() {
        let root = tempfile::tempdir().unwrap();
        let source = root.path().join("样片.mp4");
        File::create(&source)
            .unwrap()
            .set_len((BUFFER_SIZE * 4) as u64)
            .unwrap();
        let cancel = AtomicBool::new(false);
        let result = prepare(
            root.path(),
            &source,
            &cancel,
            |done, _| {
                if done > 0 {
                    cancel.store(true, Ordering::Relaxed);
                }
            },
            |_| panic!(),
        );
        assert!(result.err().unwrap().contains("取消"));
        cancel.store(false, Ordering::Relaxed);
        assert_eq!(
            prepare(
                root.path(),
                &source,
                &cancel,
                |_, _| {},
                |_| Err("无法解码视频".into())
            )
            .err()
            .unwrap(),
            "无法解码视频"
        );
        assert_eq!(
            fs::metadata(&source).unwrap().len(),
            (BUFFER_SIZE * 4) as u64
        );
        let library = root.path().join("wallpapers-native");
        assert_eq!(fs::read_dir(&library).unwrap().count(), 1);
        assert_eq!(fs::read_dir(library.join(STAGING)).unwrap().count(), 1);
    }

    #[test]
    fn only_declared_media_can_exceed_folder_import_quota() {
        let dir = tempfile::tempdir().unwrap();
        let base = plain_directory(dir.path()).unwrap();
        let asset = Path::new("clip.mp4");
        File::create(base.join(asset))
            .unwrap()
            .set_len(PACKAGE_BYTES + 1)
            .unwrap();
        assert_eq!(
            collect_files(&base, None, Some(asset)).unwrap()[asset],
            PACKAGE_BYTES + 1
        );
        assert!(collect_files(&base, None, None).is_err());
        fs::write(base.join("unreferenced.webm"), b"another").unwrap();
        assert!(collect_files(&base, None, Some(asset)).is_err());
        assert!(ensure_space(&base, u64::MAX).is_err());
    }

    #[test]
    #[ignore = "requires ACTION75_QA_PLAYER, ACTION75_QA_IMAGE and ACTION75_QA_VIDEO on Windows"]
    fn isolated_player_decodes_media_and_rejects_corruption() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        crate::storage::Preferences {
            gpu_preference: crate::storage::GpuPreference::Integrated,
            ..Default::default()
        }
        .save(root)
        .unwrap();
        let player = crate::player(root);
        fs::create_dir_all(player.parent().unwrap()).unwrap();
        fs::copy(std::env::var_os("ACTION75_QA_PLAYER").unwrap(), player).unwrap();
        let cancel = AtomicBool::new(false);
        for (env, kind) in [
            ("ACTION75_QA_IMAGE", "image"),
            ("ACTION75_QA_VIDEO", "video"),
        ] {
            let file = PathBuf::from(std::env::var_os(env).unwrap());
            let Preparation::Ready(pack) = prepare_media(root, &file, &cancel, |_, _| {}).unwrap()
            else {
                panic!()
            };
            assert_eq!(pack.manifest["kind"], kind);
            let preview = image::open(pack.package.join("preview.png"))
                .unwrap()
                .into_rgb8();
            assert!(preview.width() <= 480 && preview.height() <= 180);
            assert!(
                preview
                    .pixels()
                    .any(|pixel| pixel != preview.get_pixel(0, 0))
            );
            let trial: Value = serde_json::from_slice(
                &fs::read(pack.stage.directory.join("trial-result.json")).unwrap(),
            )
            .unwrap();
            println!("{kind}: {trial}");
            assert!(trial["ok"] == true && trial["framesRendered"].as_u64().unwrap() > 0);
            pack.publish().unwrap();
        }
        let corrupt = root.join("损坏.mp4");
        fs::write(&corrupt, b"invalid video").unwrap();
        assert!(prepare_media(root, &corrupt, &cancel, |_, _| {}).is_err());
        assert_eq!(
            fs::read_dir(root.join("wallpapers-native"))
                .unwrap()
                .count(),
            3
        );
    }
}
