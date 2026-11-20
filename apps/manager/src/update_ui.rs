use action75_manager::{
    software_update::{self, Release},
    update_install,
};
use eframe::egui;
use std::{
    path::{Path, PathBuf},
    sync::mpsc::{self, Receiver},
    time::{Duration, Instant},
};

enum Notice {
    Checked(Result<Option<Release>, String>),
    Downloaded(Result<PathBuf, String>),
    Started(Result<(), String>),
}

#[derive(Default)]
pub struct Updates {
    installed: bool,
    checked: Option<Instant>,
    release: Option<Release>,
    job: Option<Receiver<Notice>>,
    installer: Option<PathBuf>,
    handing_off: bool,
    finished: bool,
    text: String,
}

impl Updates {
    pub fn enable(&mut self, root: &Path) {
        self.installed = update_install::installed(root);
    }

    fn run(&mut self, ctx: &egui::Context, task: impl FnOnce() -> Notice + Send + 'static) {
        let (tx, rx) = mpsc::channel();
        let ctx = ctx.clone();
        self.job = Some(rx);
        std::thread::spawn(move || {
            let _ = tx.send(task());
            ctx.request_repaint();
        });
    }

    fn check(&mut self, ctx: &egui::Context) {
        self.checked = Some(Instant::now());
        self.text = "正在检查更新…".into();
        self.run(ctx, || Notice::Checked(software_update::check()));
    }

    pub fn poll(&mut self, root: &Path, ctx: &egui::Context, can_install: bool) -> bool {
        if self.finished {
            return true;
        }
        if !self.installed {
            return false;
        }
        if let Some(rx) = &self.job {
            let notice = match rx.try_recv() {
                Ok(result) => result,
                Err(mpsc::TryRecvError::Empty) => return false,
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.handing_off = false;
                    self.text = "更新任务未完成，请重试".into();
                    self.job = None;
                    return false;
                }
            };
            self.job = None;
            match notice {
                Notice::Checked(Ok(release)) => {
                    if let Some(found) = &release {
                        action75_manager::storage::log(
                            root,
                            "INFO",
                            &format!("发现新版本 {}", found.version),
                        );
                    }
                    self.release = release;
                    self.text = if self.release.is_some() {
                        String::new()
                    } else {
                        "已是最新版本".into()
                    };
                }
                Notice::Downloaded(Ok(path)) => {
                    self.installer = Some(path);
                    self.text = "下载完成，等待当前操作结束…".into();
                }
                Notice::Started(Ok(())) => {
                    self.finished = true;
                    return true;
                }
                Notice::Checked(Err(e)) | Notice::Downloaded(Err(e)) | Notice::Started(Err(e)) => {
                    action75_manager::storage::log(root, "WARN", &format!("软件更新：{e}"));
                    self.text = e;
                    self.handing_off = false;
                }
            }
        }
        if self.handing_off {
            return false;
        }
        if can_install
            && let Some(release) = self.release.clone()
            && let Some(path) = self.installer.take()
        {
            let root = root.to_owned();
            self.text = "正在启动安装程序…".into();
            self.handing_off = true;
            self.run(ctx, move || {
                let started = software_update::lock_verified(&path, &release)
                    .and_then(|_locked_file| update_install::start(&root, &path));
                if started.is_err() {
                    let _ = std::fs::remove_file(path);
                }
                Notice::Started(started)
            });
        } else if self.installer.is_none()
            && self
                .checked
                .is_none_or(|t| t.elapsed() >= Duration::from_secs(24 * 3600))
        {
            self.check(ctx);
        }
        false
    }

    pub fn has_update(&self) -> bool {
        self.release.is_some()
    }

    pub fn row(&mut self, ui: &mut egui::Ui, can_install: bool) {
        if !self.installed {
            ui.weak("通过安装版接收更新");
            return;
        }
        let busy = self.job.is_some() || self.handing_off || self.installer.is_some();
        if let Some(release) = self.release.clone() {
            ui.label(format!("发现新版本 {}", release.version));
            if ui
                .add_enabled(!busy && can_install, egui::Button::new("立即更新"))
                .clicked()
            {
                self.text = "正在下载并验证安装包…".into();
                self.run(ui.ctx(), move || {
                    Notice::Downloaded(software_update::download(&release))
                });
            }
        } else if ui
            .add_enabled(!busy, egui::Button::new("检查更新"))
            .clicked()
        {
            self.check(ui.ctx());
        }
        if !self.text.is_empty() {
            ui.weak(&self.text);
        }
    }

    pub fn protect_handoff(&self, ctx: &egui::Context) {
        if self.handing_off {
            if ctx.input(|i| i.viewport().close_requested()) {
                ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            }
            egui::Modal::new("update_handoff".into()).show(ctx, |ui| {
                ui.label("正在交接安装程序…");
            });
        }
    }
}
