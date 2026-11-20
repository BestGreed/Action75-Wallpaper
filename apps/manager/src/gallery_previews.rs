use eframe::egui;
use serde_json::Value;
use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, mpsc},
    thread,
    time::SystemTime,
};

// Visible cards are always retained, even when they exceed this scroll-back budget.
const CACHE_LIMIT: usize = 6;
const DECODE_BATCH: usize = 6;

pub struct Preview {
    pub image: egui::TextureHandle,
    pub frost: Arc<egui::ColorImage>,
}

#[derive(PartialEq, Clone)]
struct Stamp {
    file: PathBuf,
    bytes: u64,
    modified: SystemTime,
}

struct Entry {
    directory: String,
    checked: u64,
    used: u64,
    stamp: Option<Stamp>,
    preview: Option<Preview>,
}

struct Request {
    id: String,
    directory: String,
    stamp: Option<Stamp>,
}

enum Decoded {
    Unchanged,
    Missing,
    Image {
        stamp: Option<Stamp>,
        rgba: image::RgbaImage,
        frost: egui::ColorImage,
    },
}

type Batch<T> = (u64, Vec<T>);

struct Loader {
    jobs: mpsc::SyncSender<Batch<Request>>,
    results: mpsc::Receiver<Batch<(Request, Decoded)>>,
    pending: bool,
}

impl Loader {
    fn new(repaint: egui::Context) -> Self {
        let (jobs, requests) = mpsc::sync_channel::<Batch<Request>>(1);
        let (reply, results) = mpsc::channel();
        thread::spawn(move || {
            while let Ok((generation, requests)) = requests.recv() {
                let results = requests
                    .into_iter()
                    .map(|request| {
                        let decoded = decode(&request);
                        (request, decoded)
                    })
                    .collect();
                if reply.send((generation, results)).is_err() {
                    break;
                }
                repaint.request_repaint();
            }
        });
        Self {
            jobs,
            results,
            pending: false,
        }
    }
}

#[derive(Default)]
pub struct Cache {
    entries: HashMap<String, Entry>,
    loader: Option<Loader>,
    generation: u64,
    frame: u64,
}

impl Cache {
    pub fn refresh(&mut self, packs: &[Value]) {
        self.generation = self.generation.wrapping_add(1);
        if !self.entries.is_empty() {
            let current: HashMap<_, _> = packs.iter().filter_map(source).collect();
            self.entries.retain(|id, entry| {
                current.get(id.as_str()).copied() == Some(entry.directory.as_str())
            });
        }
    }

    pub fn forget(&mut self, id: &str) {
        self.entries.remove(id);
        self.generation = self.generation.wrapping_add(1);
    }

    pub fn get(&self, id: &str) -> Option<&Preview> {
        self.entries.get(id)?.preview.as_ref()
    }

    pub fn prepare_visible<'a>(
        &mut self,
        ctx: &egui::Context,
        visible: impl Iterator<Item = &'a Value>,
    ) {
        self.receive(ctx);
        self.frame = self.frame.wrapping_add(1);
        let can_request = !self.loader.as_ref().is_some_and(|loader| loader.pending);
        let mut requests = Vec::new();
        for (id, directory) in visible.filter_map(source) {
            let mut entry = self.entries.get_mut(id);
            if let Some(entry) = entry.as_deref_mut() {
                entry.used = self.frame;
                if entry.checked == self.generation {
                    continue;
                }
            }
            if can_request && requests.len() < DECODE_BATCH {
                requests.push(Request {
                    id: id.to_owned(),
                    directory: directory.to_owned(),
                    stamp: entry.and_then(|entry| entry.stamp.clone()),
                });
            }
        }

        // Keep all visible entries, even if a tall window shows more than the
        // usual cache size. Scroll history never competes with visible rows.
        while self.entries.len() > CACHE_LIMIT {
            let Some(stale) = self
                .entries
                .iter()
                .filter(|(_, entry)| entry.used != self.frame)
                .min_by_key(|(_, entry)| entry.used)
                .map(|(id, _)| id.clone())
            else {
                break;
            };
            self.entries.remove(&stale);
        }

        if !requests.is_empty() {
            let loader = self.loader.get_or_insert_with(|| Loader::new(ctx.clone()));
            loader.pending = loader.jobs.try_send((self.generation, requests)).is_ok();
        }
    }

    fn receive(&mut self, ctx: &egui::Context) {
        let Some(loader) = &mut self.loader else {
            return;
        };
        let Ok((generation, results)) = loader.results.try_recv() else {
            return;
        };
        loader.pending = false;
        if generation != self.generation {
            return;
        }
        for (request, decoded) in results {
            let id = request.id;
            let (stamp, preview) = match decoded {
                Decoded::Unchanged => {
                    if let Some(entry) = self.entries.get_mut(&id) {
                        entry.checked = generation;
                    }
                    continue;
                }
                Decoded::Missing => (None, None),
                Decoded::Image { stamp, rgba, frost } => {
                    let image = ctx.load_texture(
                        &id,
                        egui::ColorImage::from_rgba_unmultiplied(
                            [rgba.width() as usize, rgba.height() as usize],
                            rgba.as_raw(),
                        ),
                        egui::TextureOptions::LINEAR,
                    );
                    (
                        stamp,
                        Some(Preview {
                            image,
                            frost: Arc::new(frost),
                        }),
                    )
                }
            };
            self.entries.insert(
                id,
                Entry {
                    directory: request.directory,
                    checked: generation,
                    used: self.frame,
                    stamp,
                    preview,
                },
            );
        }
    }
}

fn source(pack: &Value) -> Option<(&str, &str)> {
    Some((pack["id"].as_str()?, pack["path"].as_str()?))
}

fn decode(request: &Request) -> Decoded {
    let dir = Path::new(&request.directory);
    let manifest = fs::read(dir.join("manifest.json"))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
    let name = manifest
        .as_ref()
        .and_then(|value| {
            value["preview"]
                .as_str()
                .or_else(|| value["background"].as_str())
        })
        .unwrap_or("preview.png");
    let file = dir.join(name);
    let Ok(metadata) = fs::metadata(&file) else {
        return Decoded::Missing;
    };
    if !metadata.is_file() || metadata.len() > 32 * 1024 * 1024 {
        return Decoded::Missing;
    }
    let stamp = metadata.modified().ok().map(|modified| Stamp {
        file: file.clone(),
        bytes: metadata.len(),
        modified,
    });
    if stamp.is_some() && stamp == request.stamp {
        return Decoded::Unchanged;
    }
    let Ok(mut reader) = image::ImageReader::open(&file).and_then(|r| r.with_guessed_format())
    else {
        return Decoded::Missing;
    };
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(64 * 1024 * 1024);
    limits.max_image_width = Some(8192);
    limits.max_image_height = Some(8192);
    reader.limits(limits);
    let Ok(image) = reader.decode() else {
        return Decoded::Missing;
    };
    let rgba = if image.width() == 480 && image.height() == 180 {
        image.into_rgba8()
    } else {
        image.thumbnail(480, 180).into_rgba8()
    };
    let small = image::imageops::thumbnail(&rgba, 24, 9);
    let mut frost = image::imageops::blur(&image::imageops::grayscale(&small), 1.25);
    let bottom = frost.height() - 1;
    for (_, row, pixel) in frost.enumerate_pixels_mut() {
        pixel.0[0] = 8 + pixel.0[0] / 8 + (bottom - row) as u8 * 2;
    }
    let frost = egui::ColorImage::new(
        [frost.width() as usize, frost.height() as usize],
        frost
            .iter()
            .copied()
            .map(egui::Color32::from_white_alpha)
            .collect(),
    );
    Decoded::Image { stamp, rgba, frost }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::time::{Duration, Instant, UNIX_EPOCH};

    struct Fixture(PathBuf);

    impl Fixture {
        fn new() -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            Self(
                std::env::temp_dir()
                    .join(format!("action75-previews-{}-{nonce}", std::process::id())),
            )
        }

        fn pack(&self, id: &str, subdir: &str) -> Value {
            let directory = self.0.join(subdir);
            fs::create_dir_all(&directory).unwrap();
            image::RgbaImage::from_pixel(16, 6, image::Rgba([32, 64, 96, 255]))
                .save(directory.join("preview.png"))
                .unwrap();
            json!({"id":id,"path":directory})
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn settle(cache: &mut Cache, ctx: &egui::Context, visible: &[Value]) {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let _ = ctx.run(egui::RawInput::default(), |ctx| {
                cache.prepare_visible(ctx, visible.iter());
            });
            if !cache.loader.as_ref().is_some_and(|loader| loader.pending) {
                break;
            }
            assert!(Instant::now() < deadline, "Visible previews did not settle");
            thread::sleep(Duration::from_millis(1));
        }
    }

    #[test]
    fn only_visible_covers_load_and_scrolling_releases_old_textures() {
        let ctx = egui::Context::default();
        let fixture = Fixture::new();
        let packs = (0..CACHE_LIMIT + 12)
            .map(|n| fixture.pack(&n.to_string(), &n.to_string()))
            .collect::<Vec<_>>();
        let mut cache = Cache::default();
        cache.refresh(&packs);
        settle(&mut cache, &ctx, &packs[..2]);
        assert!(cache.get("0").is_some());
        assert!(cache.get("1").is_some());
        assert!(cache.get("2").is_none());
        let first = cache.get("0").unwrap();
        let first_id = first.image.id();
        for visible in packs[2..].chunks(3) {
            settle(&mut cache, &ctx, visible);
        }
        assert!(cache.get("0").is_none());
        assert!(cache.get(&(CACHE_LIMIT + 11).to_string()).is_some());
        let textures = ctx.tex_manager();
        assert!(textures.read().meta(first_id).is_none());
        let old_count = textures.read().num_allocated();
        cache.refresh(&[]);
        assert_eq!(old_count - textures.read().num_allocated(), CACHE_LIMIT);
        let frame = ctx.run(egui::RawInput::default(), |_| {});
        assert_eq!(frame.textures_delta.free.len(), CACHE_LIMIT);
        settle(&mut cache, &ctx, &packs[..1]);
        assert!(cache.get("0").is_some()); // No process-global stamp can suppress a reload.
    }

    #[test]
    fn refresh_reuses_current_files_and_evicts_replaced_missing_and_stale_results() {
        let ctx = egui::Context::default();
        let fixture = Fixture::new();
        let pack = fixture.pack("wallpaper", "original");
        let mut cache = Cache::default();
        cache.refresh(std::slice::from_ref(&pack));
        settle(&mut cache, &ctx, std::slice::from_ref(&pack));
        let old = cache.get("wallpaper").unwrap().image.id();
        cache.refresh(std::slice::from_ref(&pack));
        settle(&mut cache, &ctx, std::slice::from_ref(&pack));
        assert_eq!(cache.get("wallpaper").unwrap().image.id(), old);

        let preview = fixture.0.join("original/preview.png");
        let modified = fs::metadata(&preview).unwrap().modified().unwrap();
        fs::OpenOptions::new()
            .write(true)
            .open(&preview)
            .unwrap()
            .set_times(fs::FileTimes::new().set_modified(modified + Duration::from_secs(10)))
            .unwrap();
        cache.refresh(std::slice::from_ref(&pack));
        settle(&mut cache, &ctx, std::slice::from_ref(&pack));
        let new = cache.get("wallpaper").unwrap().image.id();
        assert_ne!(old, new);
        assert!(ctx.tex_manager().read().meta(old).is_none());

        fs::remove_file(preview).unwrap();
        cache.refresh(std::slice::from_ref(&pack));
        settle(&mut cache, &ctx, std::slice::from_ref(&pack));
        assert!(cache.get("wallpaper").is_none());
        assert!(ctx.tex_manager().read().meta(new).is_none());

        let alternate = fixture.pack("wallpaper", "alternate");
        cache.refresh(std::slice::from_ref(&alternate));
        cache.prepare_visible(&ctx, std::iter::once(&alternate));
        let latest = fixture.pack("wallpaper", "latest");
        cache.refresh(std::slice::from_ref(&latest));
        settle(&mut cache, &ctx, &[]);
        assert!(cache.get("wallpaper").is_none());
        settle(&mut cache, &ctx, std::slice::from_ref(&latest));
        let last = cache.get("wallpaper").unwrap().image.id();
        cache.refresh(&[alternate]);
        assert!(cache.get("wallpaper").is_none());
        assert!(ctx.tex_manager().read().meta(last).is_none());
    }
}
