use action75_manager::Settings;
use eframe::egui::{self, Align2, Color32, FontId, Id, Pos2, Rect, Sense, vec2};
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Default)]
pub struct Trace {
    pub changed: bool,
    pub combo: Option<Rect>,
    pub rows: BTreeMap<String, Rect>,
    pub row_colors: BTreeMap<String, Vec<Rect>>,
    pub editors: Vec<Rect>,
    pub popup_ids: Vec<Id>,
    pub start_custom: Option<Rect>,
}

fn colors(value: &Value) -> Vec<[u8; 3]> {
    value
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|v| serde_json::from_value::<[u8; 3]>(v.clone()).ok())
        .take(9)
        .collect()
}

/// Actual palette widgets used in both the application and RawInput tests.
#[cfg(test)]
pub fn show(ui: &mut egui::Ui, settings: &mut Settings, palettes: &[Value]) -> Trace {
    show_with_limit(ui, settings, &palettes.iter().collect::<Vec<_>>(), 9)
}
pub fn show_with_limit(
    ui: &mut egui::Ui,
    settings: &mut Settings,
    palettes: &[&Value],
    max_colors: usize,
) -> Trace {
    let max_colors = max_colors.clamp(1, 9);
    let panel_width = ui.available_width();
    let mut trace = Trace::default();
    let selected_preset = |id: &str| palettes.iter().copied().find(|p| p["id"] == id);
    let name = selected_preset(&settings.palette)
        .and_then(|p| p["name"].as_str())
        .unwrap_or("选择配色");
    let mut selected = None;
    ui.spacing_mut().button_padding.y = 3.;
    let combo = egui::ComboBox::from_id_salt("effect-palette")
        .width(panel_width.min(218.))
        .wrap_mode(egui::TextWrapMode::Truncate)
        .selected_text(name)
        .close_behavior(egui::PopupCloseBehavior::CloseOnClickOutside)
        .show_ui(ui, |ui| {
            ui.set_min_width(218.);
            for p in palettes {
                let Some(id) = p["id"].as_str() else { continue };
                let name = p["name"].as_str().unwrap_or(id);
                let cs = if id == "custom" {
                    settings.palette_colors.clone()
                } else {
                    colors(&p["colors"])
                };
                ui.push_id(("preset-row", id), |ui| {
                    let (rect, response) = ui.allocate_exact_size(
                        vec2(ui.available_width().max(218.), 28.),
                        Sense::click(),
                    );
                    trace.rows.insert(id.into(), rect);
                    if response.hovered() || settings.palette == id {
                        ui.painter()
                            .rect_filled(rect, 4, ui.visuals().selection.bg_fill);
                    }
                    ui.painter().text(
                        rect.left_center() + vec2(8., 0.),
                        Align2::LEFT_CENTER,
                        name,
                        FontId::proportional(13.),
                        ui.visuals().text_color(),
                    );
                    let count = cs.len();
                    let mut circles = Vec::new();
                    for (i, c) in cs.iter().enumerate() {
                        let center = Pos2::new(
                            rect.right() - 10. - (count - 1 - i) as f32 * 12.,
                            rect.center().y,
                        );
                        ui.painter().circle_filled(
                            center,
                            4.5,
                            Color32::from_rgb(c[0], c[1], c[2]),
                        );
                        circles.push(Rect::from_center_size(center, vec2(10., 10.)));
                    }
                    trace.row_colors.insert(id.into(), circles);
                    if response.clicked() {
                        selected = Some(id.to_owned());
                        ui.close();
                    }
                });
            }
        });
    trace.combo = Some(combo.response.rect);
    if let Some(id) = selected {
        settings.palette = id;
        trace.changed = true;
    }

    let mut editable = if settings.palette == "custom" {
        settings.palette_colors.clone()
    } else {
        selected_preset(&settings.palette)
            .map(|p| colors(&p["colors"]))
            .unwrap_or_default()
    };
    if settings.palette != "custom" && editable.is_empty() {
        let r = ui
            .button("自选颜色…")
            .on_hover_text("打开颜色编辑器，自由搭配喷溅颜色");
        trace.start_custom = Some(r.rect);
        if r.clicked() {
            settings.palette = "custom".into();
            editable = settings.palette_colors.clone();
            trace.changed = true;
        }
    }
    if !editable.is_empty() {
        let mut edited = false;
        // Same parent and per-index IDs for presets and custom colors: the picker
        // stays open when the first click converts a preset into a custom palette.
        ui.push_id("palette-editors", |ui| {
            let custom = settings.palette == "custom";
            let (width, gap, max_columns) = if custom {
                (34., 8., 5)
            } else if panel_width < 193. {
                (15., 3., 9)
            } else {
                (17., 5., 9)
            };
            let columns =
                (((panel_width + gap) / (width + gap)).floor() as usize).clamp(1, max_columns);
            for (row, chunk) in editable.chunks_mut(columns).enumerate() {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = gap;
                    ui.spacing_mut().interact_size = vec2(width, if custom { 26. } else { 18. });
                    for (column, color) in chunk.iter_mut().enumerate() {
                        let index = row * columns + column;
                        ui.push_id(index, |ui| {
                            let popup_id = ui.auto_id_with("popup");
                            let response = ui.color_edit_button_srgb(color);
                            trace.editors.push(response.rect);
                            trace.popup_ids.push(popup_id);
                            if response.clicked() || response.changed() {
                                edited = true;
                            }
                            response.on_hover_text(format!("颜色 {} · 点击编辑", index + 1));
                        });
                    }
                });
            }
        });
        if settings.palette == "custom" {
            if ui
                .add_enabled(
                    editable.len() < max_colors,
                    egui::Button::new("＋ 添加颜色").small(),
                )
                .clicked()
            {
                editable.push([255, 255, 255]);
                edited = true;
            }
            if ui
                .add_enabled(editable.len() > 1, egui::Button::new("− 移除末色").small())
                .clicked()
            {
                editable.pop();
                edited = true;
            }
        }
        if edited {
            settings.palette = "custom".into();
            settings.palette_colors = editable;
            trace.changed = true;
        }
    }
    if trace.changed {
        settings.hue = 0.;
    }
    trace
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Event, Modifiers, PointerButton, RawInput};
    struct Harness {
        ctx: egui::Context,
        settings: Settings,
        palettes: Vec<Value>,
        time: f64,
        last: Trace,
        output: egui::FullOutput,
        width: f32,
    }
    impl Harness {
        fn new(palette: &str) -> Self {
            let mut settings = Settings::default();
            settings.palette = palette.into();
            settings.brightness = 4.;
            settings.scale = 2.75;
            settings.intensity = 2.4;
            settings.saturation = 2.5;
            settings.hue = 75.;
            Self {
                ctx: egui::Context::default(),
                settings,
                palettes: vec![
                    serde_json::json!({"id":"original","name":"Original","colors":[]}),
                    serde_json::json!({"id":"ocean","name":"Ocean","colors":[[40,170,220],[90,210,180],[180,220,255]]}),
                    serde_json::json!({"id":"candy","name":"Candy","colors":[[230,80,150],[80,210,240]]}),
                    serde_json::json!({"id":"custom","name":"Custom","colors":[]}),
                ],
                time: 0.,
                last: Trace::default(),
                output: Default::default(),
                width: 250.,
            }
        }
        fn frame(&mut self, events: Vec<Event>) {
            self.time += 0.05;
            let raw = RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(900., 850.))),
                time: Some(self.time),
                events,
                ..Default::default()
            };
            let mut trace = Trace::default();
            self.output = self.ctx.run(raw, |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    ui.set_width(self.width);
                    ui.set_max_width(self.width);
                    trace = show(ui, &mut self.settings, &self.palettes);
                });
            });
            self.last = trace;
        }
        fn click(&mut self, p: Pos2) -> bool {
            self.frame(vec![Event::PointerMoved(p)]);
            let mut changed = self.last.changed;
            self.frame(vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            }]);
            changed |= self.last.changed;
            self.frame(vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }]);
            changed | self.last.changed
        }
        fn open_combo(&mut self) {
            self.frame(vec![]);
            self.click(self.last.combo.unwrap().center());
            self.frame(vec![]);
        }
    }
    #[test]
    fn clicking_dropdown_color_selects_entire_preset_row() {
        let mut h = Harness::new("original");
        h.open_combo();
        let hit = h.last.row_colors["ocean"][1].center();
        let changed = h.click(hit);
        assert_eq!(h.settings.palette, "ocean");
        assert!(changed);
        assert_eq!(h.settings.brightness, 4.);
        assert_eq!(h.settings.saturation, 2.5);
    }
    #[test]
    fn clicking_row_right_space_selects_custom() {
        let mut h = Harness::new("ocean");
        h.open_combo();
        let rect = h.last.rows["custom"];
        h.click(Pos2::new(rect.right() - 2., rect.center().y));
        assert_eq!(h.settings.palette, "custom");
        assert!(!h.last.editors.is_empty());
    }
    #[test]
    fn original_has_obvious_custom_action() {
        let mut h = Harness::new("original");
        h.frame(vec![]);
        h.click(h.last.start_custom.unwrap().center());
        assert_eq!(h.settings.palette, "custom");
        assert!(!h.last.editors.is_empty());
        assert_eq!(h.settings.hue, 0.);
    }
    #[test]
    fn preset_color_opens_stable_picker_then_edit_emits_settings_payload() {
        let mut h = Harness::new("ocean");
        h.frame(vec![]);
        let hit = h.last.editors[0].center();
        let popup = h.last.popup_ids[0];
        h.click(hit);
        assert_eq!(h.settings.palette, "custom");
        assert!(egui::Popup::is_id_open(&h.ctx, popup));
        h.frame(vec![]);
        assert_eq!(h.last.popup_ids[0], popup);
        assert!(egui::Popup::is_id_open(&h.ctx, popup));
        let before = h.settings.palette_colors.clone();
        let square = h
            .output
            .shapes
            .iter()
            .find_map(|s| {
                if let egui::Shape::Mesh(m) = &s.shape {
                    let r = m.calc_bounds();
                    if r.width() > 250. && r.height() > 250. {
                        Some(r)
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .expect("actual built-in HSV color picker must be rendered");
        let changed = h.click(Pos2::new(
            square.left() + square.width() * 0.85,
            square.top() + square.height() * 0.15,
        ));
        assert_ne!(h.settings.palette_colors, before);
        assert!(changed);
        let request = h.settings.request();
        assert_eq!(request["settings"]["effectPalette"], "custom");
        assert_eq!(
            request["settings"]["effectPaletteColors"],
            serde_json::to_value(&h.settings.palette_colors).unwrap()
        );
        assert_eq!(h.settings.scale, 2.75);
        assert_eq!(h.settings.intensity, 2.4);
    }
    #[test]
    fn nine_editable_colors_wrap_inside_panel_and_last_picker_stays_open() {
        for width in [232., 280.] {
            let mut h = Harness::new("custom");
            h.width = width;
            h.settings.palette_colors = vec![
                [255, 40, 60],
                [255, 150, 30],
                [250, 215, 20],
                [80, 150, 80],
                [50, 185, 170],
                [80, 155, 240],
                [145, 100, 190],
                [230, 120, 180],
                [220, 220, 220],
            ];
            h.frame(vec![]);
            assert_eq!(h.last.editors.len(), 9);
            let left = h.last.editors[0].left();
            for rect in &h.last.editors {
                assert!(
                    rect.left() >= left - 0.1 && rect.right() <= left + width + 0.1,
                    "{width}: {rect:?}"
                );
            }
            assert!(h.last.editors[8].top() > h.last.editors[0].top());
            let popup = h.last.popup_ids[8];
            let p = h.last.editors[8].center();
            h.click(p);
            assert!(egui::Popup::is_id_open(&h.ctx, popup));
            h.frame(vec![]);
            assert_eq!(h.last.popup_ids[8], popup);
            assert!(egui::Popup::is_id_open(&h.ctx, popup));
        }
    }
}
