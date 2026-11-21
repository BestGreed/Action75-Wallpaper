use action75_manager::wallpaper_import::{self, Preparation, PreparedImport, Receipt};
use eframe::egui;
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc,
    },
    thread,
};

use crate::folder_picker;

enum Outcome {
    Published(Receipt),
    AlreadyInstalled(Receipt),
    Cancelled,
    Confirm(Box<PreparedImport>),
}

#[derive(Default)]
enum Phase {
    #[default]
    Idle,
    Choose(folder_picker::Selection),
    Choosing(
        folder_picker::Selection,
        mpsc::Receiver<folder_picker::Reply>,
    ),
    Working(mpsc::Receiver<Result<Outcome, String>>),
    Confirm(Box<PreparedImport>),
}

#[derive(Default)]
struct MediaProgress {
    cancelled: AtomicBool,
    copied: AtomicU64,
    total: AtomicU64,
}

#[derive(Default)]
pub struct ImportFlow {
    phase: Phase,
    media: Option<Arc<MediaProgress>>,
    notice: Option<Result<String, String>>,
    pub reveal: Option<String>,
}

impl Drop for ImportFlow {
    fn drop(&mut self) {
        if let Some(progress) = &self.media {
            progress.cancelled.store(true, Ordering::Relaxed);
        }
    }
}

impl ImportFlow {
    pub fn busy(&self) -> bool {
        !matches!(self.phase, Phase::Idle)
    }

    pub fn button(&mut self, ui: &mut egui::Ui, enabled: bool) {
        let available = enabled && !self.busy();
        let interactive = ui
            .add_enabled(
                available,
                egui::Button::new(egui::RichText::new("导入").size(13.))
                    .frame_when_inactive(false)
                    .min_size(egui::vec2(0., 30.))
                    .right_text(egui::Atom::custom(
                        ui.id().with("import-chevron"),
                        egui::vec2(8., 8.),
                    )),
            )
            .on_hover_cursor(egui::CursorIcon::PointingHand);
        let c = egui::pos2(
            interactive.rect.right() - ui.spacing().button_padding.x - 4.,
            interactive.rect.center().y,
        );
        let stroke = ui.style().interact(&interactive).fg_stroke;
        for dx in [-3., 3.] {
            ui.painter()
                .line_segment([c + egui::vec2(dx, -1.5), c + egui::vec2(0., 1.5)], stroke);
        }
        if available {
            let frame = egui::Frame::popup(ui.style())
                .inner_margin(6)
                .corner_radius(8);
            egui::Popup::menu(&interactive).frame(frame).show(|ui| {
                ui.set_width(164.);
                ui.spacing_mut().item_spacing.y = 2.;
                ui.spacing_mut().button_padding.x = 10.;
                for (label, kind) in [
                    ("视频或图片", folder_picker::Selection::Media),
                    ("ZIP 压缩包", folder_picker::Selection::Zip),
                    ("壁纸文件夹", folder_picker::Selection::Folder),
                ] {
                    if matches!(kind, folder_picker::Selection::Zip) {
                        ui.add_space(2.);
                        ui.separator();
                        ui.add_space(2.);
                        ui.horizontal(|ui| {
                            ui.add_space(10.);
                            ui.label(egui::RichText::new("互动壁纸").small().weak());
                        });
                    }
                    let row = egui::Button::new(label)
                        .frame_when_inactive(false)
                        .min_size(egui::vec2(ui.available_width(), 30.));
                    if ui
                        .add(row)
                        .on_hover_cursor(egui::CursorIcon::PointingHand)
                        .clicked()
                    {
                        self.phase = Phase::Choose(kind);
                        self.notice = None;
                        ui.close();
                    }
                }
            });
        }
    }

    pub fn open_picker(&mut self, frame: &eframe::Frame, ctx: &egui::Context) {
        if let Phase::Choose(kind) = self.phase {
            let title = match kind {
                folder_picker::Selection::Folder => "选择要导入的壁纸文件夹",
                folder_picker::Selection::Zip => "选择互动壁纸 ZIP",
                folder_picker::Selection::Media => "选择视频或图片",
            };
            match folder_picker::open_dialog(frame, ctx, PathBuf::new(), title, kind) {
                Ok(receiver) => self.phase = Phase::Choosing(kind, receiver),
                Err(error) => self.failed(format!("无法打开选择窗口，{error}")),
            }
        }
    }

    pub fn poll(&mut self, root: &Path, ctx: &egui::Context) -> Option<Receipt> {
        match &self.phase {
            Phase::Choosing(kind, receiver) => {
                let kind = *kind;
                let result = match receiver.try_recv() {
                    Ok(result) => result,
                    Err(mpsc::TryRecvError::Empty) => return None,
                    Err(mpsc::TryRecvError::Disconnected) => Err("文件选择已中断".into()),
                };
                self.phase = Phase::Idle;
                match result {
                    Ok(Some(path)) => {
                        let root = root.to_owned();
                        let zip = matches!(kind, folder_picker::Selection::Zip);
                        let media = matches!(kind, folder_picker::Selection::Media)
                            .then(|| Arc::new(MediaProgress::default()));
                        self.media = media.clone();
                        self.run(ctx, move || {
                            let preparation = match &media {
                                Some(progress) => wallpaper_import::prepare_media(
                                    &root,
                                    Path::new(&path),
                                    &progress.cancelled,
                                    |copied, total| {
                                        progress.total.store(total, Ordering::Relaxed);
                                        progress.copied.store(copied, Ordering::Relaxed);
                                    },
                                ),
                                None if zip => {
                                    wallpaper_import::prepare_zip(&root, Path::new(&path))
                                }
                                None => wallpaper_import::prepare(&root, Path::new(&path)),
                            };
                            if media
                                .as_ref()
                                .is_some_and(|p| p.cancelled.load(Ordering::Relaxed))
                            {
                                return Ok(Outcome::Cancelled);
                            }
                            match preparation? {
                                Preparation::Installed(receipt) => {
                                    Ok(Outcome::AlreadyInstalled(receipt))
                                }
                                Preparation::Ready(pack) if pack.update_names().is_some() => {
                                    Ok(Outcome::Confirm(pack))
                                }
                                Preparation::Ready(pack) => pack.publish().map(Outcome::Published),
                            }
                        });
                    }
                    Ok(None) => {}
                    Err(error) => self.failed(error),
                }
            }
            Phase::Working(receiver) => {
                let result = match receiver.try_recv() {
                    Ok(result) => result,
                    Err(mpsc::TryRecvError::Empty) => return None,
                    Err(mpsc::TryRecvError::Disconnected) => {
                        Err("导入任务已中断，请重新选择".into())
                    }
                };
                self.phase = Phase::Idle;
                self.media = None;
                match result {
                    Ok(Outcome::Published(receipt)) => {
                        self.notice = Some(Ok(if receipt.updated {
                            format!("已更新「{}」，重新选择后使用新版本", receipt.name)
                        } else {
                            format!("已导入「{}」", receipt.name)
                        }));
                        return Some(receipt);
                    }
                    Ok(Outcome::AlreadyInstalled(receipt)) => {
                        self.notice = Some(Ok(format!("「{}」已在壁纸库中", receipt.name)));
                        return Some(receipt);
                    }
                    Ok(Outcome::Cancelled) => self.notice = Some(Ok("已取消导入".into())),
                    Ok(Outcome::Confirm(pack)) => self.phase = Phase::Confirm(pack),
                    Err(error) => self.failed(error),
                }
            }
            _ => {}
        }
        None
    }

    fn run(
        &mut self,
        ctx: &egui::Context,
        work: impl FnOnce() -> Result<Outcome, String> + Send + 'static,
    ) {
        let (send, receive) = mpsc::channel();
        let notify = ctx.clone();
        match thread::Builder::new()
            .name("wallpaper-import".into())
            .spawn(move || {
                let _ = send.send(work());
                notify.request_repaint();
            }) {
            Ok(_) => self.phase = Phase::Working(receive),
            Err(error) => self.failed(format!("无法开始导入，{error}")),
        }
    }

    fn failed(&mut self, error: String) {
        self.phase = Phase::Idle;
        self.media = None;
        self.notice = Some(Err(error));
    }

    pub fn status(&mut self, ui: &mut egui::Ui) {
        if matches!(self.phase, Phase::Working(_)) {
            ui.horizontal(|ui| {
                ui.spinner();
                if let Some(progress) = &self.media {
                    let total = progress.total.load(Ordering::Relaxed);
                    let copied = progress.copied.load(Ordering::Relaxed);
                    if copied < total {
                        ui.label(format!(
                            "正在复制媒体文件 · {:.0}%",
                            100. * copied as f64 / total as f64
                        ));
                    } else {
                        ui.label("正在检查媒体并生成封面…");
                    }
                    if ui
                        .add_enabled(
                            !progress.cancelled.load(Ordering::Relaxed),
                            egui::Button::new("取消"),
                        )
                        .clicked()
                    {
                        progress.cancelled.store(true, Ordering::Relaxed);
                    }
                    ui.ctx()
                        .request_repaint_after(std::time::Duration::from_millis(150));
                } else {
                    ui.label("正在检查并试播壁纸…");
                }
            });
        } else if let Some(result) = &self.notice {
            let dismissed = ui
                .horizontal_top(|ui| {
                    let width = ui.available_width() - 60.;
                    let text = match result {
                        Ok(message) => egui::RichText::new(message).weak(),
                        Err(error) => egui::RichText::new(format!("导入失败，{error}"))
                            .color(ui.visuals().error_fg_color),
                    };
                    ui.allocate_ui_with_layout(
                        egui::vec2(width.max(0.), 20.),
                        egui::Layout::top_down(egui::Align::Min),
                        |ui| {
                            ui.add(egui::Label::new(text).wrap());
                        },
                    );
                    ui.small_button("关闭").clicked()
                })
                .inner;
            if dismissed {
                self.notice = None;
            }
        }
    }

    pub fn confirm(&mut self, ctx: &egui::Context) {
        let Phase::Confirm(pack) = &self.phase else {
            return;
        };
        let Some((installed, proposed)) = pack.update_names() else {
            return;
        };
        let modal = egui::Modal::new(egui::Id::new("wallpaper-import-update")).show(ctx, |ui| {
            ui.set_width(356.);
            ui.heading("更新壁纸？");
            ui.add_space(8.);
            ui.label(if proposed == installed {
                format!("「{installed}」已在壁纸库中。是否导入新版本？")
            } else {
                format!("「{proposed}」与库中的「{installed}」使用相同的 ID。是否更新？")
            });
            ui.label("兼容的参数会保留，新版本会在重新选择或重启后使用。");
            ui.add_space(12.);
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let update = ui.button("更新壁纸").clicked();
                let cancel = ui.button("取消").clicked();
                (update, cancel)
            })
            .inner
        });
        let (update, cancel) = modal.inner;
        if update {
            if let Phase::Confirm(pack) = std::mem::take(&mut self.phase) {
                self.run(ctx, move || pack.publish().map(Outcome::Published));
            }
        } else if cancel || modal.should_close() {
            self.phase = Phase::Idle;
        }
    }
}
