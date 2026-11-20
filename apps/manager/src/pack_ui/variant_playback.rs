use super::Outcome;
use eframe::egui::{self, Align, DragValue, Layout, RichText};
use serde_json::{Map, Value, json};

pub(super) fn is_group(group: &Value) -> bool {
    matches!(
        group["presentation"].as_str(),
        Some("variant-playback" | "fireworks-playback")
    )
}

pub(super) fn regular_controls(group: &Value) -> impl Iterator<Item = &Value> + Clone {
    let controls = group["controls"].as_array().into_iter().flatten();
    let legacy = group["presentation"] == "fireworks-playback";
    let roles = (group["presentation"] == "variant-playback").then(|| {
        let playback = &group["playback"];
        [
            &playback["selection"],
            &playback["randomize"],
            &playback["interval"],
        ]
    });
    let options = roles
        .and_then(|roles| controls.clone().find(|control| &control["id"] == roles[0]))
        .and_then(|selection| selection["options"].as_array());

    controls.filter(move |control| {
        !legacy
            && control["id"].as_str().is_none_or(|id| {
                !roles.is_some_and(|roles| roles.iter().any(|role| role.as_str() == Some(id)))
                    && !options.is_some_and(|options| {
                        options.iter().any(|option| option["toggleId"] == id)
                    })
            })
    })
}

struct Variant<'a> {
    id: &'a str,
    label: &'a str,
    value: u32,
    checked: bool,
}

fn whole_number(value: &Value) -> Option<u32> {
    let number = value.as_f64()?;
    let integer = number as u32;
    (number == f64::from(integer)).then_some(integer)
}

pub(super) fn show(
    ui: &mut egui::Ui,
    group: &Value,
    values: &mut Map<String, Value>,
    out: &mut Outcome,
) -> Result<(), &'static str> {
    let controls = group["controls"].as_array().ok_or("播放组缺少控件")?;
    let legacy = group["presentation"] == "fireworks-playback";
    let playback = &group["playback"];
    let role = |name: &str, legacy_id: &'static str| {
        if legacy {
            Some(legacy_id)
        } else {
            playback[name].as_str()
        }
    };
    let selection_id = role("selection", "shape").ok_or("播放组缺少选择控件引用")?;
    let randomize_id = role("randomize", "shuffle").ok_or("播放组缺少随机控件引用")?;
    let interval_id = role("interval", "keyInterval").ok_or("播放组缺少间隔控件引用")?;
    let control = |id: &str, kind: &str| {
        controls
            .iter()
            .find(|control| control["id"] == id && control["type"] == kind)
    };
    let selection = control(selection_id, "select").ok_or("播放组缺少选择控件")?;
    let randomize = control(randomize_id, "toggle").ok_or("播放组缺少随机控件")?;
    let interval = control(interval_id, "slider").ok_or("播放组缺少间隔控件")?;
    let options = selection["options"].as_array().ok_or("缺少播放选项")?;
    let fixed =
        whole_number(values.get(selection_id).unwrap_or(&selection["default"])).unwrap_or_default();
    let mut variants: Vec<Variant<'_>> = Vec::with_capacity(options.len());
    for option in options
        .iter()
        .filter(|option| !option["toggleId"].is_null())
    {
        let id = option["toggleId"]
            .as_str()
            .ok_or("播放选项的勾选控件无效")?;
        let toggle = control(id, "toggle").ok_or("播放选项缺少勾选控件")?;
        let value = whole_number(&option["value"])
            .filter(|value| *value > 0)
            .ok_or("播放选项的值必须为正整数")?;
        if id == randomize_id
            || variants
                .iter()
                .any(|other| other.id == id || other.value == value)
        {
            return Err("播放选项的值或勾选控件重复");
        }
        let checked = if fixed == 0 {
            values.get(id).unwrap_or(&toggle["default"]) == true
        } else {
            fixed == value
        };
        variants.push(Variant {
            id,
            label: option["label"].as_str().ok_or("播放选项缺少名称")?,
            value,
            checked,
        });
    }
    if variants.is_empty() {
        return Err("缺少可勾选的播放选项");
    }
    if !legacy {
        let order = playback["order"].as_array().ok_or("播放组缺少播放顺序")?;
        if order.len() != variants.len() {
            return Err("播放顺序必须包含全部可勾选选项");
        }
        let mut ordered = Vec::with_capacity(variants.len());
        for value in order {
            let value = whole_number(value);
            let index = variants
                .iter()
                .position(|variant| Some(variant.value) == value)
                .ok_or("播放顺序包含无效或重复的选项")?;
            ordered.push(variants.swap_remove(index));
        }
        variants = ordered;
    }
    let (Some(min), Some(max)) = (
        whole_number(&interval["min"]),
        whole_number(&interval["max"]),
    ) else {
        return Err("缺少击键触发间隔范围");
    };
    if min == 0 || min > max {
        return Err("击键触发间隔范围无效");
    }

    let group_id = group["id"].as_str().unwrap_or(selection_id);
    ui.push_id(("variant-playback", group_id, selection_id), |ui| {
        ui.label(RichText::new(group["label"].as_str().unwrap_or("播放选项")).strong());
        let warning_id = ui.id().with("last-variant");
        let mut blocked = ui.data(|data| data.get_temp::<bool>(warning_id).unwrap_or(false));
        let was_blocked = blocked;
        let mut count_checked = variants.iter().filter(|variant| variant.checked).count();
        let mut selection_changed = false;
        ui.with_layout(Layout::top_down_justified(Align::Min), |ui| {
            ui.spacing_mut().item_spacing.y = 2.;
            for variant in &mut variants {
                let mut checked = variant.checked;
                let response = ui
                    .push_id(variant.id, |ui| ui.checkbox(&mut checked, variant.label))
                    .inner;
                #[cfg(test)]
                out.rendered.insert(variant.id.into(), response.rect);
                if response.changed() {
                    blocked = !checked && count_checked == 1;
                    if blocked {
                        ui.ctx().request_repaint();
                    } else {
                        variant.checked = checked;
                        if checked {
                            count_checked += 1;
                        } else {
                            count_checked -= 1;
                        }
                        selection_changed = true;
                    }
                }
            }
        });
        if blocked != was_blocked {
            ui.data_mut(|data| data.insert_temp(warning_id, blocked));
        }
        if count_checked == 0 || (blocked && count_checked == 1) {
            let message = if legacy {
                "请至少保留一种花型"
            } else {
                "请至少保留一个选项"
            };
            ui.colored_label(ui.visuals().error_fg_color, message);
        }
        if selection_changed {
            let selection = if count_checked == 1 {
                variants
                    .iter()
                    .find(|variant| variant.checked)
                    .unwrap()
                    .value
            } else {
                0
            };
            // A legacy fixed profile can hold stale combination switches.
            // Publish the fixed value and the entire effective set together.
            out.set_value(values, selection_id, json!(selection));
            for variant in variants {
                out.set_value(values, variant.id, json!(variant.checked));
            }
        }
        if count_checked > 1 {
            let mut enabled = values.get(randomize_id).unwrap_or(&randomize["default"]) == true;
            let label = randomize["label"].as_str().unwrap_or("随机播放");
            let response = ui
                .push_id(randomize_id, |ui| ui.checkbox(&mut enabled, label))
                .inner;
            #[cfg(test)]
            out.rendered.insert(randomize_id.into(), response.rect);
            if response.changed() {
                out.set_value(values, randomize_id, json!(enabled));
            }
        }

        let mut count =
            whole_number(values.get(interval_id).unwrap_or(&interval["default"])).unwrap_or(min);
        let hint = if legacy {
            "达到次数时，从当次按键的位置放一发烟火"
        } else {
            interval["label"]
                .as_str()
                .unwrap_or("达到设定次数时触发效果")
        };
        let response = ui
            .push_id(interval_id, |ui| {
                let row_height = ui.text_style_height(&ui.style().drag_value_text_style)
                    + 2. * ui.spacing().button_padding.y;
                ui.spacing_mut().item_spacing.x = 4.;
                ui.spacing_mut().interact_size = egui::vec2(52., row_height);
                ui.spacing_mut().button_padding.x = 5.;
                ui.horizontal_wrapped(|ui| {
                    let prefix = ui.label(RichText::new("每按").size(13.));
                    let field = ui.add(
                        DragValue::new(&mut count)
                            .range(min..=max)
                            .custom_parser(|text| text.trim().parse::<u32>().ok().map(f64::from))
                            .update_while_editing(false),
                    );
                    let suffix = ui.label(RichText::new("次键触发").size(13.));
                    field
                        .labelled_by(prefix.id)
                        .labelled_by(suffix.id)
                        .on_hover_text(hint)
                })
                .inner
            })
            .inner;
        #[cfg(test)]
        out.rendered.insert(interval_id.into(), response.rect);
        if response.changed() {
            out.set_value(values, interval_id, json!(count));
        }
        ui.add_space(8.);
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use action75_manager::pack_model::{Bundle, Draft, defaults};
    use egui::{Context, Event, Key, Modifiers, PointerButton, Pos2, RawInput, Rect, vec2};
    use std::path::Path;

    fn frame(context: &Context, draft: &mut Draft, events: Vec<Event>) -> (Outcome, Vec<String>) {
        context.memory_mut(|m| m.set_everything_is_visible(true));
        let mut outcome = Outcome::default();
        let rendered = context.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(204., 680.))),
                events,
                ..Default::default()
            },
            |context| {
                egui::CentralPanel::default().show(context, |ui| {
                    outcome = ui
                        .push_id(("pack-settings", &draft.selected), |ui| {
                            super::super::show(
                                ui,
                                &draft.bundle.as_ref().unwrap().schema,
                                &mut draft.values,
                                &[],
                            )
                        })
                        .inner;
                });
            },
        );
        assert!(outcome.errors.is_empty(), "{:?}", outcome.errors);
        if !outcome.changed_fields.is_empty() {
            draft.changed(outcome.changed_fields.iter().cloned());
        }
        let text = rendered
            .shapes
            .iter()
            .filter_map(|shape| match &shape.shape {
                egui::Shape::Text(text) => Some(text.galley.text().to_owned()),
                _ => None,
            })
            .collect();
        (outcome, text)
    }

    fn click(context: &Context, draft: &mut Draft, pos: Pos2) -> (Outcome, Vec<String>) {
        let send = |draft: &mut Draft, pressed| {
            frame(
                context,
                draft,
                vec![
                    Event::PointerMoved(pos),
                    Event::PointerButton {
                        pos,
                        button: PointerButton::Primary,
                        pressed,
                        modifiers: Modifiers::NONE,
                    },
                ],
            )
        };
        send(draft, true);
        send(draft, false)
    }

    fn edit_interval(context: &Context, draft: &mut Draft, id: &str, value: &str) {
        let interval = frame(context, draft, vec![]).0.rendered[id].center();
        click(context, draft, interval);
        frame(context, draft, vec![]);
        frame(context, draft, vec![Event::Text(value.into())]);
        frame(
            context,
            draft,
            vec![Event::Key {
                key: Key::Enter,
                physical_key: None,
                pressed: true,
                repeat: false,
                modifiers: Modifiers::NONE,
            }],
        );
    }

    #[test]
    fn legacy_shapes_use_the_same_full_width_checkboxes_and_atomic_patch() {
        let schema = json!({
            "version": 1,
            "presentation": "layered",
            "groups": [{
                "id": "playback",
                "label": "播放花型",
                "presentation": "fireworks-playback",
                "controls": [
                    {"id":"shape","type":"select","label":"花型","default":0,
                     "binding":{"target":"fireworks.shape"},"options":[
                        {"value":0,"label":"组合轮换"},
                        {"value":1,"label":"锦冠垂柳","toggleId":"useWillow"},
                        {"value":6,"label":"千轮菊","toggleId":"useBouquet"},
                        {"value":8,"label":"心形","toggleId":"useHeart"}
                     ]},
                    {"id":"shuffle","type":"toggle","label":"随机抽取花型","default":false,
                     "binding":{"target":"fireworks.shuffle"}},
                    {"id":"useWillow","type":"toggle","label":"锦冠垂柳","default":true,
                     "binding":{"target":"fireworks.willow"}},
                    {"id":"useBouquet","type":"toggle","label":"千轮菊","default":false,
                     "binding":{"target":"fireworks.bouquet"}},
                    {"id":"useHeart","type":"toggle","label":"心形","default":true,
                     "binding":{"target":"fireworks.heart"}},
                    {"id":"keyInterval","type":"slider","label":"击键触发间隔","default":1,
                     "min":1,"max":1000000,"step":1,"binding":{"target":"fireworks.keyInterval"}}
                ]
            }, {
                "id": "appearance",
                "label": "外观",
                "controls": [
                    {"id":"size","type":"slider","label":"绽放范围","default":1,
                     "min":0.5,"max":6,"step":0.05,"binding":{"target":"fireworks.spread"}}
                ]
            }]
        });
        let mut values = defaults(&schema);
        let context = Context::default();
        crate::ui_style::install_fonts(&context);
        let mut draft = Draft::default();
        let selection = draft.select("night-fireworks".into());
        values.insert("shape".into(), json!(6));
        values.insert("size".into(), json!(2.6));
        values.insert("shuffle".into(), json!(true));
        let mut legacy = Bundle {
            pack_id: "night-fireworks".into(),
            schema,
            values,
            revision: json!(7),
        };
        assert!(draft.load("night-fireworks", selection, legacy.clone()));

        let (initial, _) = frame(&context, &mut draft, vec![]);
        assert!(!initial.rendered.contains_key("shape"));
        assert!(!initial.rendered.contains_key("shuffle"));
        assert!(draft.patch().is_empty());
        let row_end = |id: &str| {
            let rect = initial.rendered[id];
            assert!(rect.width() >= 170., "{rect:?}");
            Pos2::new(rect.right() - 5., rect.center().y)
        };
        let (blocked, feedback) = click(&context, &mut draft, row_end("useBouquet"));
        assert!(blocked.changed_fields.is_empty() && draft.patch().is_empty());
        assert!(feedback.iter().any(|label| label == "请至少保留一种花型"));

        let (multi, _) = click(&context, &mut draft, row_end("useHeart"));
        assert!(!multi.changed_fields.is_empty() && multi.rendered.contains_key("shuffle"));
        assert_eq!(draft.values["shape"], 0);
        assert_eq!(draft.values["shuffle"], true);
        assert_eq!(draft.values["size"], 2.6);
        assert_eq!(draft.values["useWillow"], false);
        assert_eq!(draft.values["useBouquet"], true);
        assert_eq!(draft.values["useHeart"], true);
        assert_eq!(draft.patch()["shape"], 0);
        assert_eq!(draft.patch()["useBouquet"], true);
        assert_eq!(draft.edit, 1);

        legacy.values = draft.values.clone();
        let selection = draft.select("night-fireworks".into());
        assert!(draft.load("night-fireworks", selection, legacy));
        let (restored, _) = frame(&context, &mut draft, vec![]);
        assert!(restored.rendered.contains_key("shuffle") && draft.patch().is_empty());
        let (one, _) = click(&context, &mut draft, row_end("useBouquet"));
        assert!(!one.changed_fields.is_empty() && !one.rendered.contains_key("shuffle"));
        assert_eq!(draft.values["shape"], 8);

        edit_interval(&context, &mut draft, "keyInterval", "3");
        assert_eq!(draft.patch()["keyInterval"], 3);
    }

    #[test]
    fn swapping_folders_changes_the_rendered_order_labels_and_interval_bindings() {
        let context = Context::default();
        crate::ui_style::install_fonts(&context);
        let mut draft = Draft::default();
        let load = |draft: &mut Draft, folder: &str| {
            let file = Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("tests/fixtures/variant-playback")
                .join(folder)
                .join("settings.json");
            let schema = serde_json::from_str(&std::fs::read_to_string(file).unwrap()).unwrap();
            let values = defaults(&schema);
            let id = "night-fireworks";
            let selection = draft.select(id.into());
            assert!(draft.load(
                id,
                selection,
                Bundle {
                    pack_id: id.into(),
                    schema,
                    values,
                    revision: json!(selection),
                },
            ));
        };
        let check_rows = |out: &Outcome, text: &[String], rows: &[(&str, &str)]| {
            assert!(
                rows.windows(2)
                    .all(|pair| { out.rendered[pair[0].0].top() < out.rendered[pair[1].0].top() })
            );
            let expected: Vec<_> = rows.iter().map(|(_, label)| *label).collect();
            let rendered: Vec<_> = text
                .iter()
                .map(String::as_str)
                .filter(|label| expected.contains(label))
                .collect();
            assert_eq!(rendered, expected);
            println!("渲染的复选框顺序 {}", rendered.join(" → "));
        };

        load(&mut draft, "original");
        let (original, text) = frame(&context, &mut draft, vec![]);
        assert!(text.iter().any(|label| label == "原版花型"));
        assert!(text.iter().any(|label| label == "2"));
        check_rows(
            &original,
            &text,
            &[
                ("useHeart", "心形"),
                ("useWillow", "垂柳"),
                ("usePalm", "棕榈"),
            ],
        );
        assert!(!original.rendered.contains_key("shape"));
        assert!(draft.patch().is_empty());
        click(&context, &mut draft, original.rendered["shuffle"].center());
        assert_eq!(draft.patch()["shuffle"], true);
        edit_interval(&context, &mut draft, "keyInterval", "5");
        assert_eq!(draft.patch()["keyInterval"], 5);
        println!("原版间隔显示 2，编辑后 keyInterval = 5");

        load(&mut draft, "replacement");
        let (replacement, text) = frame(&context, &mut draft, vec![]);
        assert!(text.iter().any(|label| label == "新包样式"));
        assert!(text.iter().any(|label| label == "4"));
        assert!(text.iter().any(|label| label == "光斑亮度"));
        assert!(replacement.rendered.contains_key("intensity"));
        check_rows(
            &replacement,
            &text,
            &[("enableRing", "银色星环"), ("enablePetal", "花芯")],
        );
        for obsolete in ["shape", "shuffle", "keyInterval", "useWillow", "pattern"] {
            assert!(!replacement.rendered.contains_key(obsolete), "{obsolete}");
        }
        assert!(!replacement.rendered.contains_key("randomOrder"));
        assert!(draft.patch().is_empty());

        let (multi, text) = click(
            &context,
            &mut draft,
            replacement.rendered["enablePetal"].center(),
        );
        assert!(text.iter().any(|label| label == "打乱样式"));
        assert_eq!(
            multi.changed_fields,
            ["pattern", "enableRing", "enablePetal"]
        );
        assert_eq!(draft.patch()["pattern"], 0);
        assert_eq!(draft.values["enableRing"], true);
        assert_eq!(draft.values["enablePetal"], true);
        let random = multi.rendered["randomOrder"].center();
        click(&context, &mut draft, random);
        assert_eq!(draft.patch()["randomOrder"], true);
        click(&context, &mut draft, random);
        assert_eq!(draft.values["randomOrder"], false);

        let (fixed, _) = click(&context, &mut draft, multi.rendered["enableRing"].center());
        assert_eq!(draft.patch()["pattern"], 3);
        assert!(!fixed.rendered.contains_key("randomOrder"));
        edit_interval(&context, &mut draft, "keysPerBeat", "99");
        assert_eq!(draft.patch()["keysPerBeat"], 7);
        assert!(!draft.values.contains_key("keyInterval"));
        println!("新包间隔显示 4，输入 99 后 keysPerBeat 限制为 7，已实点随机、顺序和固定项");
    }
}
