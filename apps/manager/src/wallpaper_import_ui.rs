use action75_manager::wallpaper_import::{self, Preparation, PreparedImport, Receipt};
use eframe::egui;
use std::{
    path::{Path, PathBuf},
    sync::mpsc,
    thread,
};

use crate::folder_picker;

enum Outcome {
    Published(Receipt),
    AlreadyInstalled(Receipt),
    Confirm(Box<PreparedImport>),
}

#[derive(Default)]
enum Phase {
    #[default]
    Idle,
    Choose,
    Choosing(mpsc::Receiver<folder_picker::Reply>),
    Working(mpsc::Receiver<Result<Outcome, String>>),
    Confirm(Box<PreparedImport>),
}

#[derive(Default)]
pub struct ImportFlow {
    phase: Phase,
    notice: Option<Result<String, String>>,
    pub reveal: Option<String>,
}

impl ImportFlow {
    pub fn busy(&self) -> bool {
        !matches!(self.phase, Phase::Idle)
    }

    pub fn button(&mut self, ui: &mut egui::Ui, enabled: bool) {
        let button = egui::Button::new("导入壁纸").min_size(egui::vec2(0., 27.));
        if ui
            .add_enabled(enabled && !self.busy(), button)
            .on_hover_text("选择包含 manifest.json 的壁纸文件夹")
            .clicked()
        {
            self.phase = Phase::Choose;
            self.notice = None;
        }
    }

    pub fn open_picker(&mut self, frame: &eframe::Frame, ctx: &egui::Context) {
        if matches!(self.phase, Phase::Choose) {
            match folder_picker::open(frame, ctx, PathBuf::new(), "选择要导入的壁纸文件夹")
            {
                Ok(receiver) => self.phase = Phase::Choosing(receiver),
                Err(error) => self.failed(format!("无法打开文件夹选择窗口，{error}")),
            }
        }
    }

    pub fn poll(&mut self, root: &Path, ctx: &egui::Context) -> Option<Receipt> {
        match &self.phase {
            Phase::Choosing(receiver) => {
                let result = match receiver.try_recv() {
                    Ok(result) => result,
                    Err(mpsc::TryRecvError::Empty) => return None,
                    Err(mpsc::TryRecvError::Disconnected) => Err("文件夹选择已中断".into()),
                };
                self.phase = Phase::Idle;
                match result {
                    Ok(Some(path)) => {
                        let root = root.to_owned();
                        self.run(ctx, move || {
                            match wallpaper_import::prepare(&root, Path::new(&path))? {
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
                        Err("导入任务已中断，请重新选择文件夹".into())
                    }
                };
                self.phase = Phase::Idle;
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
        self.notice = Some(Err(error));
    }

    pub fn status(&mut self, ui: &mut egui::Ui) {
        if matches!(self.phase, Phase::Working(_)) {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label("正在检查并试播壁纸…");
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
