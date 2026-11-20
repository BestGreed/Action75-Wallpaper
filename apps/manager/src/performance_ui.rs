use crate::{
    performance::{PlayerProcesses, Reading, SAMPLE_INTERVAL, Sample, Session},
    ui_style,
};
use action75_manager::graphics_adapters::AdapterModel;
use eframe::egui::{
    self, Align, Align2, Color32, FontId, Layout, Pos2, Rect, RichText, Sense, Shape, Stroke, Ui,
    pos2, vec2,
};
use std::{
    collections::VecDeque,
    time::{Duration, Instant},
};

const HISTORY: Duration = Duration::from_secs(60);
const GIB: f32 = 1024. * 1024. * 1024.;
const MIB: f32 = 1024. * 1024.;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum Scope {
    System,
    #[default]
    Player,
}

#[derive(Default)]
pub struct PerformancePage {
    session: Option<Session>,
    samples: VecDeque<Sample>,
    processors: u32,
    adapters: Vec<AdapterModel>,
    scope: Scope,
    player_pid: u32,
    failed: bool,
}

impl PerformancePage {
    pub fn update(&mut self, ctx: &egui::Context, visible: bool, processes: PlayerProcesses) {
        if !visible {
            self.session = None;
            self.failed = false;
            return;
        }
        self.player_pid = processes.player_pid();
        if self.session.is_none() && !self.failed {
            match Session::start(ctx, processes) {
                Ok(worker) => self.session = Some(worker),
                Err(_) => self.failed = true,
            }
        }
        if let Some(session) = &self.session {
            session.set_player(processes);
            for reading in session.readings.try_iter() {
                match reading {
                    Reading::Hardware {
                        processors,
                        adapters,
                    } => {
                        self.processors = processors;
                        self.adapters = adapters;
                    }
                    Reading::Sample(sample) => self.samples.push_back(sample),
                }
            }
        }
        let now = Instant::now();
        while self
            .samples
            .front()
            .is_some_and(|sample| now.duration_since(sample.at) > HISTORY)
        {
            self.samples.pop_front();
        }
    }

    pub fn show(&mut self, ui: &mut Ui, adapter_name: Option<&str>, adapter_luid: Option<u64>) {
        ui.set_max_width(ui.available_width().min(1160.));
        ui.horizontal_top(|ui| {
            ui.label(RichText::new("性能监控").font(ui_style::display_font(32.)));
            ui.allocate_ui_with_layout(
                vec2(ui.available_width(), 36.),
                Layout::right_to_left(Align::Center),
                |ui| {
                    ui.selectable_value(&mut self.scope, Scope::Player, "壁纸播放器");
                    ui.selectable_value(&mut self.scope, Scope::System, "整机");
                },
            );
        });
        ui.add_space(4.);
        let player = self.scope == Scope::Player;
        let offline = player && self.player_pid == 0;
        ui.label(
            RichText::new(if self.failed {
                "无法启动性能采样"
            } else if offline {
                "壁纸播放器未连接"
            } else {
                "最近 60 秒  ·  每秒采样"
            })
            .weak(),
        );
        ui.add_space(18.);

        let now = Instant::now();
        let latest = self
            .samples
            .back()
            .filter(|sample| now.duration_since(sample.at) < SAMPLE_INTERVAL * 3);
        let current = latest.filter(|_| !offline);
        let timeline = Timeline {
            samples: &self.samples,
            end: latest.map_or(now, |sample| sample.at),
            height: if ui.ctx().content_rect().height() < 760. {
                108.
            } else {
                126.
            },
        };
        let cpu = |sample: &Sample| {
            if player {
                sample.player?.cpu
            } else {
                sample.cpu
            }
        };
        let memory = |sample: &Sample| {
            if player {
                Some(sample.player?.working_set? as f32 / MIB)
            } else {
                Some(sample.memory?.used as f32 / GIB)
            }
        };
        let adapter_name = adapter_name.filter(|name| self.player_pid != 0 && !name.is_empty());
        let luid = adapter_name.and_then(|name| {
            adapter_luid.or_else(|| {
                // Older players only report the model name. A unique match is safe.
                let model = self.adapters.iter().find(|model| model.name == name)?;
                match model.luids.as_slice() {
                    [id] => Some(*id),
                    _ => None,
                }
            })
        });
        let gpu = |sample: &Sample| {
            if player && sample.player.is_none() {
                return None;
            }
            let load = sample.gpu.adapters.get(&luid?)?;
            Some(if player { load.player } else { load.system })
        };

        let [cpu_ink, memory_ink, gpu_ink] = series_colors(ui.visuals().dark_mode);
        let system_capacity = latest.and_then(|sample| sample.memory.map(|v| v.total as f32 / GIB));
        let system_memory =
            if let Some((total, used)) = system_capacity.zip(current.and_then(memory)) {
                format!("共 {total:.1} GiB · {:.1}%", used * 100. / total)
            } else {
                "物理内存".into()
            };
        timeline.row(
            ui,
            Metric {
                title: "CPU",
                value: current.and_then(cpu),
                unit: Unit::Percent,
                ceiling: timeline.percent_ceiling(player, cpu),
                color: cpu_ink,
            },
            cpu,
            |ui| {
                let label = if player {
                    "整机 CPU 时间占比".to_owned()
                } else if self.processors > 0 {
                    format!("{} 个逻辑处理器", self.processors)
                } else {
                    "总利用率".to_owned()
                };
                ui.label(RichText::new(label).size(13.).weak());
            },
        );
        timeline.separator(ui);
        timeline.row(
            ui,
            Metric {
                title: "内存",
                value: current.and_then(memory),
                unit: if player { Unit::MiB } else { Unit::GiB },
                ceiling: if player {
                    (timeline.peak(memory) / 128.).ceil().max(1.) * 128.
                } else {
                    system_capacity.unwrap_or(1.)
                },
                color: memory_ink,
            },
            memory,
            |ui| {
                let detail = if player {
                    "进程工作集"
                } else {
                    &system_memory
                };
                ui.label(RichText::new(detail).size(13.).weak());
            },
        );
        timeline.separator(ui);
        let last_chart = timeline.row(
            ui,
            Metric {
                title: "显卡",
                value: current.and_then(gpu),
                unit: Unit::Percent,
                ceiling: timeline.percent_ceiling(player, gpu),
                color: gpu_ink,
            },
            gpu,
            |ui| {
                let name = adapter_name.unwrap_or("等待播放器连接");
                ui.add(egui::Label::new(RichText::new(name).size(13.).weak()).truncate())
                    .on_hover_text(name);
            },
        );
        timeline.time_axis(ui, last_chart);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::performance::{GpuReadings, PhysicalMemory};

    #[test]
    fn player_is_default_and_system_can_be_selected_in_the_minimum_window() {
        let ctx = egui::Context::default();
        ui_style::install_fonts(&ctx);
        let screen = Rect::from_min_size(Pos2::ZERO, vec2(1080., 680.));
        let now = Instant::now();
        let mut page = PerformancePage {
            samples: (0..3)
                .rev()
                .map(|second| Sample {
                    at: now - Duration::from_secs(second),
                    cpu: Some(18.2),
                    memory: Some(PhysicalMemory {
                        used: 12 << 30,
                        total: 32 << 30,
                    }),
                    player: None,
                    gpu: GpuReadings::default(),
                })
                .collect(),
            ..Default::default()
        };
        let draw = |page: &mut PerformancePage, events| {
            ctx.run(
                egui::RawInput {
                    screen_rect: Some(screen),
                    events,
                    ..Default::default()
                },
                |ctx| {
                    egui::TopBottomPanel::top("chrome")
                        .exact_height(50.)
                        .show(ctx, |_| {});
                    egui::SidePanel::left("rail")
                        .exact_width(122.)
                        .show(ctx, |_| {});
                    egui::CentralPanel::default()
                        .frame(egui::Frame::new().inner_margin(26))
                        .show(ctx, |ui| {
                            egui::ScrollArea::vertical().show(ui, |ui| page.show(ui, None, None));
                        });
                },
            )
        };
        let visible_text = |frame: &egui::FullOutput, label: &str| {
            frame.shapes.iter().find_map(|shape| {
                let Shape::Text(text) = &shape.shape else {
                    return None;
                };
                let bounds = text.galley.rect.translate(text.pos.to_vec2());
                (text.galley.job.text == label && shape.clip_rect.contains_rect(bounds))
                    .then_some(bounds)
            })
        };
        let frame = draw(&mut page, vec![]);
        assert!(page.scope == Scope::Player);
        for label in [
            "整机",
            "壁纸播放器",
            "壁纸播放器未连接",
            "CPU",
            "内存",
            "显卡",
            "现在",
        ] {
            assert!(
                visible_text(&frame, label).is_some(),
                "{label} must be visible without scrolling"
            );
        }

        let pos = visible_text(&frame, "整机").unwrap().center();
        draw(&mut page, vec![egui::Event::PointerMoved(pos)]);
        for pressed in [true, false] {
            draw(
                &mut page,
                vec![egui::Event::PointerButton {
                    pos,
                    button: egui::PointerButton::Primary,
                    pressed,
                    modifiers: egui::Modifiers::NONE,
                }],
            );
        }
        assert!(page.scope == Scope::System);
        assert!(visible_text(&draw(&mut page, vec![]), "32.0 GiB").is_some());
    }
}

fn series_colors(dark: bool) -> [Color32; 3] {
    let [cpu, memory, gpu] = if dark {
        [[132, 208, 211], [232, 188, 120], [193, 171, 236]]
    } else {
        [[39, 108, 118], [138, 93, 36], [108, 79, 153]]
    };
    [cpu, memory, gpu].map(|[r, g, b]| Color32::from_rgb(r, g, b))
}

#[derive(Clone, Copy)]
enum Unit {
    Percent,
    GiB,
    MiB,
}

impl Unit {
    fn text(self) -> &'static str {
        match self {
            Self::Percent => "%",
            Self::GiB => "GiB",
            Self::MiB => "MiB",
        }
    }

    fn number(self, value: f32) -> String {
        match self {
            Self::Percent if value > 0. && value < 1. => format!("{value:.2}"),
            Self::MiB => format!("{value:.0}"),
            _ => format!("{value:.1}"),
        }
    }

    fn tick(self, value: f32) -> String {
        match self {
            Self::GiB => format!("{value:.1} GiB"),
            _ => format!("{value:.0} {}", self.text()),
        }
    }
}

struct Metric {
    title: &'static str,
    value: Option<f32>,
    unit: Unit,
    ceiling: f32,
    color: Color32,
}

struct Timeline<'a> {
    samples: &'a VecDeque<Sample>,
    end: Instant,
    height: f32,
}

impl Timeline<'_> {
    fn peak(&self, read: impl Fn(&Sample) -> Option<f32>) -> f32 {
        self.samples.iter().filter_map(read).fold(0., f32::max)
    }

    fn percent_ceiling(&self, for_player: bool, read: impl Fn(&Sample) -> Option<f32>) -> f32 {
        if for_player {
            let peak = self.peak(read);
            [2., 5., 10., 25., 50., 100.]
                .into_iter()
                .find(|&tick| tick >= peak * 1.08)
                .unwrap_or(100.)
        } else {
            100.
        }
    }

    fn row(
        &self,
        ui: &mut Ui,
        metric: Metric,
        read: impl Fn(&Sample) -> Option<f32>,
        details: impl FnOnce(&mut Ui),
    ) -> Rect {
        ui.horizontal_top(|ui| {
            let label_width = 184.;
            ui.allocate_ui_with_layout(
                vec2(label_width, self.height),
                Layout::top_down(Align::Min),
                |ui| {
                    ui.set_width(label_width);
                    ui.spacing_mut().item_spacing.y = 3.;
                    ui.label(RichText::new(metric.title).size(16.).strong());
                    ui.horizontal(|ui| {
                        let number = metric
                            .value
                            .map_or_else(|| "—".to_owned(), |value| metric.unit.number(value));
                        ui.label(RichText::new(number).monospace().size(28.));
                        if metric.value.is_some() {
                            ui.label(RichText::new(metric.unit.text()).size(13.).weak());
                        }
                    });
                    details(ui);
                },
            );
            ui.add_space(13.);
            self.plot(ui, &metric, read)
        })
        .inner
    }

    fn separator(&self, ui: &mut Ui) {
        ui.add_space(6.);
        ui.separator();
        ui.add_space(6.);
    }

    fn plot(&self, ui: &mut Ui, metric: &Metric, read: impl Fn(&Sample) -> Option<f32>) -> Rect {
        let (bounds, response) =
            ui.allocate_exact_size(vec2(ui.available_width(), self.height), Sense::hover());
        let rect = Rect::from_min_max(bounds.min + vec2(0., 6.), bounds.max - vec2(64., 5.));
        let painter = ui.painter();
        let ink = ui.visuals().weak_text_color();
        let grid = Stroke::new(1_f32, ink.gamma_multiply(0.17));
        painter.rect_filled(rect, 2, ui.visuals().extreme_bg_color.gamma_multiply(0.20));
        for index in 0..=6 {
            let x = egui::lerp(rect.x_range(), index as f32 / 6.);
            painter.vline(x, rect.y_range(), grid);
        }
        for index in 0..=4 {
            let y = egui::lerp(rect.y_range(), index as f32 / 4.);
            painter.hline(rect.x_range(), y, grid);
        }
        for (y, value) in [
            (rect.top(), metric.unit.tick(metric.ceiling)),
            (rect.bottom(), "0".to_owned()),
        ] {
            painter.text(
                pos2(rect.right() + 8., y),
                Align2::LEFT_CENTER,
                value,
                FontId::proportional(11.5),
                ink,
            );
        }

        let painter = painter.with_clip_rect(painter.clip_rect().intersect(rect.expand(2.)));
        let stroke = Stroke::new(1.7_f32, metric.color);
        let draw = |points: &mut Vec<Pos2>| {
            if points.len() > 1 {
                painter.add(Shape::line(std::mem::take(points), stroke));
            } else if let Some(point) = points.pop() {
                painter.circle_filled(point, 2., metric.color);
            }
        };
        let hover = response.hover_pos().filter(|&point| rect.contains(point));
        let mut closest = None;
        let mut points = Vec::new();
        let mut previous = None;
        let mut endpoint = None;
        for sample in self.samples {
            let age = self.end.saturating_duration_since(sample.at);
            let Some(value) = read(sample).filter(|_| age <= HISTORY) else {
                draw(&mut points);
                previous = None;
                continue;
            };
            if previous.is_some_and(|at| sample.at.duration_since(at) > SAMPLE_INTERVAL * 2) {
                draw(&mut points);
            }
            let x = rect.right() - rect.width() * (age.as_secs_f32() / HISTORY.as_secs_f32());
            let y = rect.bottom() - rect.height() * (value / metric.ceiling).clamp(0., 1.);
            let point = pos2(x, y);
            if let Some(cursor) = hover {
                let distance = (cursor.x - x).abs();
                if distance < rect.width() / 45.
                    && closest.is_none_or(|(best, _, _, _)| distance < best)
                {
                    closest = Some((distance, point, age, value));
                }
            }
            points.push(point);
            previous = Some(sample.at);
            endpoint = Some(point);
        }
        draw(&mut points);
        if let Some(point) = endpoint.filter(|_| metric.value.is_some()) {
            painter.circle_filled(point, 2.2, metric.color);
        }
        if let Some((_, point, age, value)) = closest {
            painter.vline(
                point.x,
                rect.y_range(),
                Stroke::new(1_f32, ink.gamma_multiply(0.58)),
            );
            painter.circle_filled(point, 3.4, metric.color);
            response.on_hover_text(format!(
                "{}  {} {}\n{} 秒前",
                metric.title,
                metric.unit.number(value),
                metric.unit.text(),
                age.as_secs_f32().round() as u32
            ));
        }
        rect
    }

    fn time_axis(&self, ui: &mut Ui, chart: Rect) {
        let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 22.), Sense::hover());
        let y = rect.top() + 3.;
        for (x, align, label) in [
            (chart.left(), Align2::LEFT_TOP, "60 秒前"),
            (chart.center().x, Align2::CENTER_TOP, "30 秒前"),
            (chart.right(), Align2::RIGHT_TOP, "现在"),
        ] {
            ui.painter().text(
                pos2(x, y),
                align,
                label,
                FontId::proportional(12.),
                ui.visuals().weak_text_color(),
            );
        }
    }
}
