use action75_manager::Settings;
#[cfg(test)]
use eframe::egui::Rect;
use eframe::egui::{self, RichText};
use serde_json::{Map, Value, json};
#[cfg(test)]
use std::collections::BTreeMap;

mod variant_playback;

#[derive(Default)]
pub struct Outcome {
    pub changed_fields: Vec<String>,
    #[cfg(test)]
    pub rendered: BTreeMap<String, Rect>,
    pub errors: Vec<String>,
    #[cfg(test)]
    pub palette_editors: BTreeMap<String, Vec<Rect>>,
}

impl Outcome {
    fn set_value(&mut self, values: &mut Map<String, Value>, id: &str, value: Value) {
        values.insert(id.into(), value);
        self.changed_fields.push(id.into());
    }
}

pub fn show(
    ui: &mut egui::Ui,
    schema: &Value,
    values: &mut Map<String, Value>,
    palettes: &[Value],
) -> Outcome {
    let mut out = Outcome::default();
    if schema.is_null() {
        return out;
    }
    if schema["version"] != 1 {
        out.errors.push("不支持的壁纸配置版本".into());
        return out;
    }
    let Some(groups) = schema["groups"].as_array() else {
        out.errors.push("壁纸配置缺少 groups".into());
        return out;
    };

    if schema["presentation"] == "layered" {
        fn section<'a>(group: &'a Value, item: &Value) -> &'a str {
            group["entity"]
                .as_str()
                .filter(|s| !s.trim().is_empty())
                .unwrap_or(if item["surface"] == "background" {
                    "背景"
                } else {
                    "前景"
                })
        }
        let regular = || {
            groups.iter().flat_map(|g| {
                variant_playback::regular_controls(g).map(move |c| (section(g, c), c))
            })
        };
        let mut titles = vec!["前景", "背景"];
        for group in groups {
            if let Some(title) = group["entity"].as_str().filter(|s| !s.trim().is_empty()) {
                if !titles.contains(&title) {
                    titles.push(title);
                }
            }
        }
        let mut first_layer = true;
        for title in titles {
            let layer = || regular().filter_map(|(owner, c)| (owner == title).then_some(c));
            let mut basic: Vec<_> = layer().filter(|c| appearance_rank(c).is_some()).collect();
            basic.sort_by_key(|c| appearance_rank(c));
            let has_basic = basic.iter().any(|c| control_visible(c, values));
            let effects = layer().filter(|c| appearance_rank(c).is_none());
            let playback = groups
                .iter()
                .filter(|g| variant_playback::is_group(g) && section(g, g) == title);
            let has_advanced = playback.clone().next().is_some()
                || effects.clone().any(|c| control_visible(c, values));
            if !has_basic && !has_advanced {
                continue;
            }
            if !first_layer {
                ui.add_space(8.);
                ui.separator();
            }
            first_layer = false;
            ui.add_space(4.);
            ui.label(RichText::new(title).size(16.).strong());
            if has_basic {
                ui.weak("基础设置");
                show_controls(ui, basic.into_iter(), values, palettes, &mut out);
            }
            if has_advanced {
                egui::CollapsingHeader::new("高级设置")
                    .id_salt(("layer_advanced", title))
                    .show(ui, |ui| {
                        for group in playback {
                            if let Err(error) = variant_playback::show(ui, group, values, &mut out)
                            {
                                out.errors.push(error.into());
                            }
                        }
                        show_controls(ui, effects, values, palettes, &mut out);
                    });
            }
        }
    } else {
        for group in groups {
            if variant_playback::is_group(group) {
                if let Err(error) = variant_playback::show(ui, group, values, &mut out) {
                    out.errors.push(error.into());
                }
            } else if let Some(label) = group["label"].as_str().filter(|label| !label.is_empty()) {
                ui.label(RichText::new(label).size(15.).strong());
            }
            if group["controls"].is_array() {
                show_controls(
                    ui,
                    variant_playback::regular_controls(group),
                    values,
                    palettes,
                    &mut out,
                );
                ui.add_space(10.);
            } else {
                out.errors.push("配置组缺少 controls".into());
            }
        }
    }
    for error in &out.errors {
        ui.colored_label(ui.visuals().error_fg_color, error);
    }
    out
}

fn appearance_rank(control: &Value) -> Option<usize> {
    ["style", "brightness", "hue", "saturation", "color"]
        .iter()
        .position(|role| Some(*role) == control["appearance"].as_str())
}

fn control_visible(control: &Value, values: &Map<String, Value>) -> bool {
    control.get("visibleWhen").is_none_or(|condition| {
        values.get(condition["id"].as_str().unwrap_or("")) == condition.get("equals")
    })
}

fn palettes_for<'a>(control: &'a Value, legacy: &'a [Value]) -> Result<Vec<&'a Value>, String> {
    static CUSTOM: std::sync::LazyLock<Value> =
        std::sync::LazyLock::new(|| json!({"id":"custom","name":"自定义","colors":[]}));
    let mut options: Vec<_> = if let Some(inline) = control.get("palettes") {
        inline
            .as_array()
            .ok_or("palettes 必须是数组")?
            .iter()
            .collect()
    } else {
        let presets = control["presets"].as_array();
        legacy
            .iter()
            .filter(|p| presets.is_none_or(|ids| ids.contains(&p["id"])))
            .collect()
    };
    if !options.iter().any(|p| p["id"] == "custom") {
        options.push(&CUSTOM);
    }
    Ok(options)
}
fn show_controls<'a>(
    ui: &mut egui::Ui,
    controls: impl Iterator<Item = &'a Value>,
    values: &mut Map<String, Value>,
    palettes: &[Value],
    out: &mut Outcome,
) {
    for control in controls {
        let Some(id) = control["id"].as_str() else {
            out.errors.push("配置控件缺少 id".into());
            continue;
        };
        if !control_visible(control, values) {
            continue;
        }
        if control.get("default").is_none() {
            out.errors.push(format!("控件 {id} 缺少 default"));
            continue;
        }
        let label = control["label"].as_str().unwrap_or(id);
        let mut value = values.get(id).unwrap_or(&control["default"]).clone();
        let result = ui
            .push_id(("schema-control", id), |ui| -> Result<bool, String> {
                let right = ui.max_rect().right().min(ui.clip_rect().right());
                ui.set_width((right - ui.cursor().left()).max(1.));
                let mut changed = false;
                match control["type"].as_str().unwrap_or("") {
                    "slider" => {
                        let (min, max, step) = (
                            control["min"].as_f64().ok_or("slider 缺少 min")?,
                            control["max"].as_f64().ok_or("slider 缺少 max")?,
                            control["step"].as_f64().unwrap_or(1.),
                        );
                        if !min.is_finite()
                            || !max.is_finite()
                            || min >= max
                            || !step.is_finite()
                            || step <= 0.
                        {
                            return Err("slider 范围无效".into());
                        }
                        let mut number = value.as_f64().ok_or("slider 当前值不是数字")?;
                        let unit = control["unit"].as_str().unwrap_or("");
                        let formatted = if step >= 1. {
                            format!("{number:.0}{unit}")
                        } else {
                            format!("{number:.2}{unit}")
                        };
                        let response = super::percent_ui::slider_row(ui, label, formatted, |ui| {
                            ui.spacing_mut().interact_size.y = 12.;
                            ui.style_mut()
                                .text_styles
                                .insert(egui::TextStyle::Body, egui::FontId::proportional(10.));
                            let v = ui.visuals_mut();
                            v.selection.bg_fill = v.selection.stroke.color.gamma_multiply(0.6);
                            v.widgets.inactive.bg_fill = v.widgets.inactive.bg_stroke.color;
                            v.widgets.inactive.corner_radius = 2.into();
                            v.widgets.hovered.corner_radius = 2.into();
                            v.widgets.active.corner_radius = 2.into();
                            ui.add(
                                egui::Slider::new(&mut number, min..=max)
                                    .show_value(false)
                                    .handle_shape(egui::style::HandleShape::Rect {
                                        aspect_ratio: 0.9,
                                    })
                                    .step_by(step),
                            )
                        });
                        #[cfg(test)]
                        out.rendered.insert(id.into(), response.rect);
                        // Slider can mark itself changed just by rounding an
                        // off-step migrated value during layout. Only user
                        // manipulation may replace the canonical JSON value.
                        let user_edit = response.dragged()
                            || response.is_pointer_button_down_on()
                            || response.clicked()
                            || (response.has_focus()
                                && ui.input(|i| {
                                    i.key_pressed(egui::Key::ArrowLeft)
                                        || i.key_pressed(egui::Key::ArrowRight)
                                }));
                        if response.changed() && user_edit {
                            value = if number.fract() == 0. && number.abs() < 9e15 {
                                json!(number as i64)
                            } else {
                                json!(number)
                            };
                            changed = true;
                        }
                    }
                    "toggle" => {
                        let mut enabled = value.as_bool().ok_or("toggle 当前值不是布尔值")?;
                        let r = ui.checkbox(&mut enabled, label);
                        #[cfg(test)]
                        out.rendered.insert(id.into(), r.rect);
                        if r.changed() {
                            value = json!(enabled);
                            changed = true;
                        }
                    }
                    "select" => {
                        ui.label(label);
                        let options = control["options"].as_array().ok_or("select 缺少 options")?;
                        let caption = options
                            .iter()
                            .find(|o| o.get("value").unwrap_or(o) == &value)
                            .map(|o| {
                                o["label"]
                                    .as_str()
                                    .map(str::to_owned)
                                    .unwrap_or_else(|| o.to_string())
                            })
                            .unwrap_or_else(|| value.to_string());
                        let _response = egui::ComboBox::from_id_salt("select")
                            .width(ui.available_width().min(210.))
                            .wrap_mode(egui::TextWrapMode::Truncate)
                            .selected_text(caption)
                            .show_ui(ui, |ui| {
                                for option in options {
                                    let v = option.get("value").unwrap_or(option);
                                    let name = option["label"]
                                        .as_str()
                                        .map(str::to_owned)
                                        .unwrap_or_else(|| {
                                            v.as_str()
                                                .map(str::to_owned)
                                                .unwrap_or_else(|| v.to_string())
                                        });
                                    if ui.selectable_value(&mut value, v.clone(), name).changed() {
                                        changed = true;
                                    }
                                }
                            });
                        #[cfg(test)]
                        out.rendered.insert(id.into(), _response.response.rect);
                    }
                    "color" => {
                        ui.label(label);
                        let mut rgb: [u8; 3] = serde_json::from_value(value.clone())
                            .map_err(|_| "color 必须是 RGB 数组")?;
                        let r = ui.color_edit_button_srgb(&mut rgb);
                        #[cfg(test)]
                        out.rendered.insert(id.into(), r.rect);
                        if r.changed() {
                            value = json!(rgb);
                            changed = true;
                        }
                    }
                    "palette" => {
                        ui.label(label);
                        let mut bridge = Settings::default();
                        bridge.palette = value["preset"]
                            .as_str()
                            .ok_or("palette 缺少 preset")?
                            .into();
                        bridge.palette_colors = serde_json::from_value(value["colors"].clone())
                            .map_err(|_| "palette 缺少 RGB colors")?;
                        let max = control["maxColors"].as_u64().unwrap_or(9).clamp(1, 9) as usize;
                        if bridge.palette_colors.is_empty() || bridge.palette_colors.len() > max {
                            return Err("palette 颜色数量超出配置".into());
                        }
                        let filtered = palettes_for(control, palettes)?;
                        let trace =
                            super::palette_ui::show_with_limit(ui, &mut bridge, &filtered, max);
                        #[cfg(test)]
                        out.palette_editors.insert(id.into(), trace.editors.clone());
                        #[cfg(test)]
                        if let Some(rect) = trace.combo {
                            out.rendered.insert(id.into(), rect);
                        }
                        changed |= trace.changed;
                        if control["allowSingleHue"] == true {
                            if let Some(r) = super::percent_ui::single_color_hue(ui, &mut bridge) {
                                changed |= r.changed();
                            }
                        }
                        if changed {
                            value["preset"] = json!(bridge.palette);
                            value["colors"] = json!(bridge.palette_colors);
                        }
                    }
                    unknown => return Err(format!("不支持的控件类型：{unknown}")),
                }
                Ok(changed)
            })
            .inner;
        match result {
            Ok(true) => {
                out.set_value(values, id, value);
            }
            Ok(false) => {}
            Err(e) => {
                out.errors.push(format!("{label}：{e}"));
            }
        }
        ui.add_space(4.);
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Pos2, RawInput, vec2};

    #[test]
    fn bundled_controls_fit_a_narrow_inspector_with_advanced_sections_open() {
        let packs =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../wallpapers-native");
        for entry in std::fs::read_dir(packs).unwrap() {
            let path = entry.unwrap().path().join("settings.json");
            if !path.is_file() {
                continue;
            }
            let schema: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
            let mut values: Map<_, _> = schema["groups"]
                .as_array()
                .unwrap()
                .iter()
                .flat_map(|group| group["controls"].as_array().unwrap())
                .map(|c| (c["id"].as_str().unwrap().to_owned(), c["default"].clone()))
                .collect();
            let ctx = egui::Context::default();
            ctx.memory_mut(|mem| mem.set_everything_is_visible(true));
            let _ = ctx.run(
                RawInput {
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(220., 9000.))),
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        ui.set_width(159.);
                        let right = ui.max_rect().right();
                        let out = show(ui, &schema, &mut values, &[]);
                        assert!(out.errors.is_empty(), "{path:?}: {:?}", out.errors);
                        for (id, rect) in out.rendered {
                            assert!(
                                rect.right() <= right + 1.,
                                "{path:?} {id}: {rect:?} > {right}"
                            );
                        }
                    });
                },
            );
        }
    }

    #[test]
    fn advanced_sliders_keep_values_and_rails_inside_the_same_right_edge() {
        use egui::{Event, Modifiers, PointerButton, ViewportId};

        let schema = json!({"version":1,"presentation":"layered","groups":[{"controls":[
            {"id":"duration","type":"slider","label":"消散时长","appearance":"brightness","default":0.9,"min":0.2,"max":3,"step":0.01,"unit":"秒","binding":{"target":"duration"}},
            {"id":"size","type":"slider","label":"轨道大小与键盘相对比例","default":2.75,"min":0.5,"max":3,"step":0.05,"unit":"×","advanced":true,"binding":{"target":"size"}},
            {"id":"quality","type":"select","label":"细节档位","default":1,"options":[{"value":1,"label":"标准档位的完整说明文字"}]},
            {"id":"saturation","type":"slider","label":"饱和度","default":78,"min":0,"max":100,"unit":"%","advanced":true,"binding":{"target":"saturation"}},
            {"id":"hue","type":"slider","label":"色相","default":0,"min":0,"max":360,"unit":"°","surface":"background","appearance":"hue","binding":{"target":"hue"}}
        ]}]});
        for (width, scale) in [(197., 1.25), (159., 1.25), (159., 1.5)] {
            let ctx = egui::Context::default();
            crate::ui_style::install_fonts(&ctx);
            ctx.style_mut(|style| style.animation_time = 0.);
            let mut values = Map::new();
            let mut frame = |events| {
                let mut input = RawInput {
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 500.))),
                    events,
                    ..Default::default()
                };
                input
                    .viewports
                    .get_mut(&ViewportId::ROOT)
                    .unwrap()
                    .native_pixels_per_point = Some(scale);
                let mut right = 0.;
                let mut out = Outcome::default();
                let output = ctx.run(input, |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        ui.set_width(width);
                        right = ui.max_rect().right();
                        let clip = ui.clip_rect();
                        ui.set_clip_rect(clip.with_max_x(right + 8.));
                        out = show(ui, &schema, &mut values, &[]);
                    });
                });
                (output, out, right)
            };
            let initial = frame(vec![]).0;
            let pos = initial
                .shapes
                .iter()
                .find_map(|clipped| match &clipped.shape {
                    egui::Shape::Text(text) if text.galley.job.text == "高级设置" => {
                        Some(text.visual_bounding_rect().center())
                    }
                    _ => None,
                })
                .unwrap();
            frame(vec![Event::PointerMoved(pos)]);
            for pressed in [true, false] {
                frame(vec![Event::PointerButton {
                    pos,
                    button: PointerButton::Primary,
                    pressed,
                    modifiers: Modifiers::NONE,
                }]);
            }
            let (output, out, right) = frame(vec![]);
            assert!(out.errors.is_empty());
            assert!(out.changed_fields.is_empty());
            assert!(out.rendered["quality"].right() <= right + 1. / scale);
            assert!(out.rendered["size"].left() > out.rendered["duration"].left());
            for (id, value) in [
                ("duration", "0.90秒"),
                ("size", "2.75×"),
                ("saturation", "78%"),
                ("hue", "0°"),
            ] {
                assert!(
                    (out.rendered[id].right() - right).abs() <= 1. / scale,
                    "{id}"
                );
                let (edge, clip) = output
                    .shapes
                    .iter()
                    .find_map(|clipped| match &clipped.shape {
                        egui::Shape::Text(text) if text.galley.job.text == value => {
                            assert!(!text.galley.elided);
                            Some((
                                text.pos.x + text.galley.rect.right(),
                                clipped.clip_rect.right(),
                            ))
                        }
                        _ => None,
                    })
                    .unwrap();
                assert!((edge - right).abs() <= 1. / scale && edge < clip, "{value}");
            }
        }
    }

    #[test]
    fn basic_appearance_is_ordered_by_layer_and_role_before_effects() {
        let schema = json!({"version":1,"presentation":"layered","groups":[{"controls":[
            {"id":"hidden","type":"toggle","label":"Hidden","default":false,"advanced":true},
            {"id":"size","type":"toggle","label":"Size","default":false},
            {"id":"bg","type":"toggle","label":"BG","default":false,"surface":"background","appearance":"style"},
            {"id":"sat","type":"toggle","label":"Sat","default":false,"appearance":"saturation","advanced":true},
            {"id":"light","type":"toggle","label":"Light","default":false,"appearance":"brightness"}
        ]}]});
        let ctx = egui::Context::default();
        let mut out = Outcome::default();
        let _ = ctx.run(RawInput::default(), |ctx| {
            egui::CentralPanel::default().show(ctx, |ui| {
                out = show(ui, &schema, &mut Map::new(), &[]);
            });
        });
        assert!(out.errors.is_empty());
        assert!(!out.rendered.contains_key("hidden"));
        assert!(!out.rendered.contains_key("size"));
        for pair in ["light", "sat", "bg"].windows(2) {
            assert!(out.rendered[pair[0]].bottom() <= out.rendered[pair[1]].top());
        }
    }

    #[test]
    fn black_hole_uses_pack_entities_and_separate_appearance_controls() {
        let schema: Value = serde_json::from_str(include_str!(
            "../../../wallpapers-native/black-hole/settings.json"
        ))
        .unwrap();
        let ctx = egui::Context::default();
        let result = ctx.run(RawInput::default(), |ctx| {
            egui::CentralPanel::default().show(ctx, |ui| {
                let out = show(ui, &schema, &mut Map::new(), &[]);
                assert!(
                    out.rendered["diskPalette"].bottom() < out.rendered["starBrightness"].top()
                );
                assert!(out.rendered["starColor"].bottom() < out.rendered["skyMode"].top());
                assert!(!out.rendered.contains_key("lensStrength"));
            });
        });
        let headings: Vec<_> = result
            .shapes
            .iter()
            .filter_map(|s| match &s.shape {
                egui::Shape::Text(t) => Some(t.galley.text()),
                _ => None,
            })
            .filter(|s| ["黑洞与吸积盘", "恒星", "远景天空", "前景", "背景"].contains(s))
            .collect();
        assert_eq!(headings, ["黑洞与吸积盘", "恒星", "远景天空"]);
    }

    #[test]
    fn native_schema_fields_render_without_renderer_bindings() {
        let schema = json!({"version":1,"presentation":"layered","groups":[{"id":"new","label":"New Group","controls":[
            {"id":"excitation","type":"slider","label":"Excitation","default":17,"min":0,"max":70,"step":1},
            {"id":"stars","type":"toggle","label":"Stars","default":true},
            {"id":"gradient","type":"select","label":"Gradient","default":"cool","options":[
                {"value":"cool","label":"Cool"},{"value":"warm","label":"Warm"}
            ]},
            {"id":"tint","type":"color","label":"Tint","default":[255,250,240],"surface":"background"}
        ]}]});
        let ctx = egui::Context::default();
        ctx.memory_mut(|m| m.set_everything_is_visible(true));
        let mut values = Map::new();
        let mut out = Outcome::default();
        let _ = ctx.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 400.))),
                ..Default::default()
            },
            |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    out = show(ui, &schema, &mut values, &[]);
                });
            },
        );
        for id in ["excitation", "stars", "gradient", "tint"] {
            assert!(out.rendered.contains_key(id), "{id}");
        }
        assert!(out.changed_fields.is_empty());
        assert!(out.errors.is_empty());
    }
    #[test]
    fn staged_background_schema_controls_render_and_edit_from_files() {
        use egui::{Event, Modifiers, PointerButton};
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/background-schemas");
        let mut tested = 0;
        for entry in std::fs::read_dir(root).unwrap() {
            let path = entry.unwrap().path().join("settings.json");
            if !path.is_file() {
                continue;
            }
            let complete: Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
            let background = complete["groups"]
                .as_array()
                .unwrap()
                .iter()
                .find(|g| g["id"] == "background")
                .unwrap()
                .clone();
            let controls = background["controls"].as_array().unwrap().clone();
            assert_eq!(controls.len(), 4);
            assert_eq!(controls.iter().filter(|c| c["type"] == "slider").count(), 3);
            assert_eq!(controls.iter().filter(|c| c["type"] == "color").count(), 1);
            let schema = json!({"version":complete["version"],"groups":[background]});
            let baseline = action75_manager::pack_model::defaults(&complete);
            let mut values = baseline.clone();
            let ctx = egui::Context::default();
            let mut out = Outcome::default();
            let mut frame = |events| {
                let _ = ctx.run(
                    RawInput {
                        screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 500.))),
                        events,
                        ..Default::default()
                    },
                    |ctx| {
                        egui::CentralPanel::default().show(ctx, |ui| {
                            out = show(ui, &schema, &mut values, &[]);
                        });
                    },
                );
                assert!(out.errors.is_empty());
                out.rendered.clone()
            };
            let rects = frame(vec![]);
            assert_eq!(rects.len(), controls.len());
            for control in controls.iter().filter(|c| c["type"] == "slider") {
                let id = control["id"].as_str().unwrap();
                let rect = rects[id];
                let p = Pos2::new(rect.right() - 1., rect.center().y);
                frame(vec![Event::PointerMoved(p)]);
                for pressed in [true, false] {
                    frame(vec![Event::PointerButton {
                        pos: p,
                        button: PointerButton::Primary,
                        pressed,
                        modifiers: Modifiers::NONE,
                    }]);
                }
            }
            for control in controls.iter().filter(|c| c["type"] == "slider") {
                let id = control["id"].as_str().unwrap();
                assert_eq!(values[id], control["max"]);
            }
            for (id, value) in &baseline {
                if !controls
                    .iter()
                    .any(|c| c["type"] == "slider" && c["id"] == id.as_str())
                {
                    assert_eq!(&values[id], value);
                }
            }
            tested += 1;
        }
        assert_eq!(tested, 7);
    }
    #[test]
    fn visible_when_and_unknown_widget_are_safe() {
        let schema = json!({"version":1,"groups":[{"id":"g","label":"Group","controls":[{"id":"shown","type":"toggle","label":"Show","default":false,"binding":{"target":"x"}},{"id":"conditional","type":"slider","label":"Conditional","default":2,"min":0,"max":4,"binding":{"target":"y"},"visibleWhen":{"id":"shown","equals":true}},{"id":"future","type":"future-widget","label":"Future","default":0,"binding":{"target":"z"}}]}]});
        let ctx = egui::Context::default();
        let mut values = Map::from_iter([("shown".into(), json!(false))]);
        let mut out = Outcome::default();
        let _ = ctx.run(RawInput::default(), |ctx| {
            egui::CentralPanel::default().show(ctx, |ui| {
                out = show(ui, &schema, &mut values, &[]);
            });
        });
        assert!(!out.rendered.contains_key("conditional"));
        assert!(out.errors.iter().any(|e| e.contains("future-widget")));
    }
    #[test]
    fn arbitrary_slider_changes_only_its_canonical_value_on_real_input() {
        use egui::{Event, Modifiers, PointerButton};
        let schema = json!({"version":1,"groups":[{"id":"extension","label":"Extension","controls":[{"id":"brand_new_parameter","type":"slider","label":"New parameter","default":17,"min":0,"max":70,"step":1,"binding":{"target":"plugin.custom"}}]}]});
        let ctx = egui::Context::default();
        let mut values = Map::from_iter([("unrelated".into(), json!(44))]);
        let mut out = Outcome::default();
        let mut frame = |events: Vec<Event>| {
            let _ = ctx.run(
                RawInput {
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 400.))),
                    events,
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        out = show(ui, &schema, &mut values, &[]);
                    });
                },
            );
            out.rendered["brand_new_parameter"]
        };
        let rect = frame(vec![]);
        let p = Pos2::new(rect.right() - 1., rect.center().y);
        frame(vec![Event::PointerMoved(p)]);
        frame(vec![Event::PointerButton {
            pos: p,
            button: PointerButton::Primary,
            pressed: true,
            modifiers: Modifiers::NONE,
        }]);
        frame(vec![Event::PointerButton {
            pos: p,
            button: PointerButton::Primary,
            pressed: false,
            modifiers: Modifiers::NONE,
        }]);
        assert_eq!(values["brand_new_parameter"], 70);
        assert_eq!(values["unrelated"], 44);
    }
    #[test]
    fn empty_frames_preserve_non_step_migrated_json_and_emit_no_patch() {
        let schema = json!({"version":1,"groups":[{"id":"g","label":"Controls","controls":[
            {"id":"brightness","type":"slider","label":"Brightness","default":50,"min":0,"max":100,"step":1,"binding":{"target":"x"}},
            {"id":"size","type":"slider","label":"Size","default":1,"min":0.5,"max":3,"step":0.05,"binding":{"target":"y"}},
            {"id":"duration","type":"slider","label":"Duration","default":0.63,"min":0.2,"max":2.5,"step":0.01,"binding":{"target":"z"}}
        ]}]});
        let original = serde_json::from_value::<Map<String, Value>>(
            json!({"brightness":89.44271909999159,"size":1.200000047,"duration":0.63}),
        )
        .unwrap();
        let exact = serde_json::to_vec(&original).unwrap();
        let mut draft = action75_manager::pack_model::Draft::default();
        let selection = draft.select("migrated".into());
        draft.load(
            "migrated",
            selection,
            action75_manager::pack_model::Bundle {
                pack_id: "migrated".into(),
                schema: schema.clone(),
                values: original,
                revision: json!(2),
            },
        );
        let ctx = egui::Context::default();
        for _ in 0..5 {
            let mut out = Outcome::default();
            let _ = ctx.run(
                RawInput {
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(400., 600.))),
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        out = show(ui, &schema, &mut draft.values, &[]);
                    });
                },
            );
            assert!(out.changed_fields.is_empty());
            assert!(out.errors.is_empty());
            assert!(draft.patch().is_empty());
            assert_eq!(serde_json::to_vec(&draft.values).unwrap(), exact);
        }
    }
    #[test]
    fn inline_same_id_palettes_use_each_files_name_and_rgb_not_global() {
        let legacy = vec![json!({"id":"same","name":"GLOBAL MUST NOT APPEAR","colors":[[1,2,3]]})];
        let a = json!({"palettes":[{"id":"same","name":"File Red","colors":[[240,50,80],[255,180,70]]}]});
        let b = json!({"palettes":[{"id":"same","name":"File Blue","colors":[[40,150,240],[90,220,200]]}]});
        let pa = palettes_for(&a, &legacy).unwrap();
        let pb = palettes_for(&b, &legacy).unwrap();
        assert_eq!(pa[0]["id"], pb[0]["id"]);
        assert_ne!(pa[0]["name"], pb[0]["name"]);
        assert_ne!(pa[0]["colors"], pb[0]["colors"]);
        assert_eq!(pa.len(), 2);
        assert_eq!(pa[1]["id"], "custom");
        assert!(palettes_for(&json!({"palettes":null}), &legacy).is_err());
    }
    #[test]
    fn actual_inline_preset_color_click_edits_that_files_colors() {
        use egui::{Event, Modifiers, PointerButton};
        for (name, rgb) in [
            ("File Red", json!([[240, 50, 80], [255, 180, 70]])),
            ("File Blue", json!([[40, 150, 240], [90, 220, 200]])),
        ] {
            let schema = json!({"version":1,"groups":[{"id":"g","label":"Colors","controls":[{"id":"combination","type":"palette","label":"Color set","default":{"preset":"same","colors":[[120,120,120]]},"maxColors":9,"allowSingleHue":true,"binding":{"target":"palette"},"palettes":[{"id":"same","name":name,"colors":rgb}]}]}]});
            let legacy =
                vec![json!({"id":"same","name":"GLOBAL MUST NOT APPEAR","colors":[[1,2,3]]})];
            let mut values = Map::from_iter([(
                "combination".into(),
                json!({"preset":"same","colors":[[120,120,120]]}),
            )]);
            let ctx = egui::Context::default();
            let mut out = Outcome::default();
            let mut output = egui::FullOutput::default();
            let mut frame = |events: Vec<Event>| {
                output = ctx.run(
                    RawInput {
                        screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(550., 650.))),
                        events,
                        ..Default::default()
                    },
                    |ctx| {
                        egui::CentralPanel::default().show(ctx, |ui| {
                            out = show(ui, &schema, &mut values, &legacy);
                        });
                    },
                );
                out.palette_editors["combination"][0]
            };
            let rect = frame(vec![]);
            let p = rect.center();
            frame(vec![Event::PointerMoved(p)]);
            frame(vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            }]);
            frame(vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }]);
            assert_eq!(values["combination"]["preset"], "custom");
            assert_eq!(values["combination"]["colors"], rgb);
            assert!(egui::Popup::is_any_open(&ctx));
        }
    }
}
