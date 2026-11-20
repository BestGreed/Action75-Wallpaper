use eframe::egui;
use serde_json::{Value, json};

pub fn selection_key(monitor: &Value) -> Option<&str> {
    if monitor["primary"] == true {
        return None;
    }
    let device = monitor["device"].as_str().filter(|name| !name.is_empty())?;
    match monitor.get("monitorIdentity") {
        Some(identity) => identity.as_str().filter(|path| !path.is_empty()),
        None => Some(device),
    }
}

pub fn keyboard_resolution(m: &Value) -> bool {
    selection_key(m).is_some() && m["width"] == 1920 && m["height"] == 720
}

pub fn same_choice(a: &Value, b: &Value) -> bool {
    selection_key(a).is_some()
        && selection_key(a) == selection_key(b)
        && a["width"] == b["width"]
        && a["height"] == b["height"]
}

pub fn show(ui: &mut egui::Ui, monitor: &Value) -> egui::Response {
    let primary = monitor["primary"] == true;
    let key = selection_key(monitor);
    let name = monitor["label"].as_str().unwrap_or("显示器");
    let label = if primary {
        format!("{name} · 不可作为键盘屏")
    } else if key.is_none() {
        format!("{name} · 无法确认显示身份")
    } else {
        name.to_owned()
    };
    ui.push_id(monitor["device"].as_str().unwrap_or(name), |ui| {
        ui.add_enabled_ui(key.is_some(), |ui| {
            ui.selectable_label(monitor["selected"] == true, label)
        })
        .inner
    })
    .inner
}

pub fn selection_request(monitor: &Value) -> Option<Value> {
    selection_key(monitor)?;
    let mut settings = json!({"monitorDevice":monitor["device"]});
    if let Some(identity) = monitor.get("monitorIdentity") {
        settings["monitorIdentity"] = identity.clone();
    }
    Some(json!({"action":"setSettings","settings":settings}))
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Event, Modifiers, PointerButton, Pos2, RawInput, Rect, vec2};

    fn frame(ctx: &egui::Context, monitor: &Value, events: Vec<Event>) -> (Rect, Option<Value>) {
        let mut rect = Rect::NOTHING;
        let mut request = None;
        let _ = ctx.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(600., 160.))),
                events,
                ..Default::default()
            },
            |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    let response = show(ui, monitor);
                    rect = response.rect;
                    if response.clicked() {
                        request = selection_request(monitor);
                    }
                });
            },
        );
        (rect, request)
    }

    fn click_monitor(monitor: &Value) -> Option<Value> {
        let ctx = egui::Context::default();
        let p = frame(&ctx, monitor, vec![]).0.center();
        frame(&ctx, monitor, vec![Event::PointerMoved(p)]);
        let down = frame(
            &ctx,
            monitor,
            vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            }],
        )
        .1;
        let up = frame(
            &ctx,
            monitor,
            vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }],
        )
        .1;
        down.or(up)
    }

    #[test]
    fn actual_primary_row_click_never_emits_monitor_request() {
        for selected in [false, true] {
            assert!(
                click_monitor(&json!({"device":"DISPLAY1","label":"Primary",
                "primary":true,"selected":selected}))
                .is_none()
            );
        }
    }

    #[test]
    fn actual_secondary_row_click_emits_its_device_only() {
        assert_eq!(
            click_monitor(&json!({"device":"DISPLAY2","label":"Action75",
                "primary":false,"selected":false})),
            Some(json!({"action":"setSettings","settings":{"monitorDevice":"DISPLAY2"}}))
        );
    }

    #[test]
    fn identified_monitor_click_carries_the_physical_identity() {
        assert_eq!(
            click_monitor(
                &json!({"device":"DISPLAY2","label":"Action75","primary":false,
                "monitorIdentity":"target-a"})
            )
            .unwrap()["settings"],
            json!({"monitorDevice":"DISPLAY2","monitorIdentity":"target-a"})
        );
        assert!(
            click_monitor(
                &json!({"device":"DISPLAY2","label":"Action75","primary":false,
            "monitorIdentity":""})
            )
            .is_none()
        );
    }
}
