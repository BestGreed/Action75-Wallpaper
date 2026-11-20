use eframe::egui;
use serde_json::{Value, json};

#[derive(Default)]
pub struct State {
    pub acknowledged: bool,
    pub supported: bool,
    pub pending: Option<u64>,
    generation: u64,
    pub error: String,
}
#[derive(Clone, Copy, Debug)]
pub struct Request {
    pub generation: u64,
    pub value: bool,
}
impl Request {
    pub fn json(self) -> Value {
        json!({"action":"setSettings","settings":{"exclusiveDisplay":self.value}})
    }
}
impl State {
    pub fn observe(&mut self, status: &Value) {
        if let Some(value) = status["exclusiveDisplay"].as_bool() {
            self.supported = true;
            if self.pending.is_none() {
                self.acknowledged = value;
            }
        }
    }
    fn begin(&mut self, value: bool) -> Option<Request> {
        if !self.supported || self.pending.is_some() || self.acknowledged == value {
            return None;
        }
        self.generation = self.generation.wrapping_add(1);
        self.pending = Some(self.generation);
        self.error.clear();
        Some(Request {
            generation: self.generation,
            value,
        })
    }
    pub fn ack(&mut self, generation: u64, status: &Value) -> bool {
        if self.pending != Some(generation) {
            return false;
        }
        self.pending = None;
        if let Some(value) = status["exclusiveDisplay"].as_bool() {
            self.supported = true;
            self.acknowledged = value;
            self.error.clear();
        } else {
            self.error = "主机未确认副屏专用设置".into();
        }
        true
    }
    pub fn fail(&mut self, generation: u64, error: String) {
        if self.pending == Some(generation) {
            self.pending = None;
            self.error = error;
        }
    }
}
/// The checkbox displays the last server-acknowledged requested state, not guard activity.
pub fn show(ui: &mut egui::Ui, state: &mut State) -> (egui::Response, Option<Request>) {
    let mut value = state.acknowledged;
    let response = ui.add_enabled(
        state.supported && state.pending.is_none(),
        egui::Checkbox::new(&mut value, "键盘副屏专用"),
    );
    let request = if response.changed() {
        state.begin(value)
    } else {
        None
    };
    (response, request)
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Event, Modifiers, PointerButton, Pos2, RawInput, Rect, vec2};
    fn frame(
        ctx: &egui::Context,
        state: &mut State,
        events: Vec<Event>,
    ) -> (Rect, Option<Request>) {
        let mut rect = Rect::NOTHING;
        let mut request = None;
        let _ = ctx.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(500., 200.))),
                events,
                ..Default::default()
            },
            |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    let (r, sent) = show(ui, state);
                    rect = r.rect;
                    if sent.is_some() {
                        request = sent;
                    }
                });
            },
        );
        (rect, request)
    }
    fn click(ctx: &egui::Context, state: &mut State, p: Pos2) -> Option<Request> {
        frame(ctx, state, vec![Event::PointerMoved(p)]);
        let a = frame(
            ctx,
            state,
            vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            }],
        )
        .1;
        let b = frame(
            ctx,
            state,
            vec![Event::PointerButton {
                pos: p,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }],
        )
        .1;
        a.or(b)
    }
    #[test]
    fn actual_checkbox_emits_global_only_json_and_waits_for_ack() {
        let ctx = egui::Context::default();
        let mut state = State::default();
        state.observe(&json!({"exclusiveDisplay":false}));
        let p = frame(&ctx, &mut state, vec![]).0.center();
        let request = click(&ctx, &mut state, p).unwrap();
        assert_eq!(
            request.json(),
            json!({"action":"setSettings","settings":{"exclusiveDisplay":true}})
        );
        assert!(!state.acknowledged);
        assert!(click(&ctx, &mut state, p).is_none());
        assert_eq!(state.pending, Some(request.generation));
        state.ack(
            request.generation,
            &json!({"exclusiveDisplay":true,"displayGuardMode":"pending","displayGuardError":null}),
        );
        assert!(state.acknowledged);
        assert!(state.error.is_empty());
    }
    #[test]
    fn stale_status_and_ack_do_not_overwrite_pending_or_new_selection() {
        let mut state = State::default();
        state.observe(&json!({"exclusiveDisplay":false}));
        let first = state.begin(true).unwrap();
        state.observe(&json!({"exclusiveDisplay":false}));
        assert!(!state.acknowledged);
        assert!(!state.ack(first.generation + 1, &json!({"exclusiveDisplay":true})));
        state.ack(first.generation, &json!({"exclusiveDisplay":true}));
        let second = state.begin(false).unwrap();
        assert!(!state.ack(first.generation, &json!({"exclusiveDisplay":true})));
        assert_eq!(state.pending, Some(second.generation));
        state.ack(second.generation, &json!({"exclusiveDisplay":false}));
        assert!(!state.acknowledged);
    }
    #[test]
    fn real_error_releases_pending_without_optimistic_toggle() {
        let mut state = State::default();
        state.observe(&json!({"exclusiveDisplay":false}));
        let r = state.begin(true).unwrap();
        state.fail(r.generation, "Win32 failed".into());
        assert!(!state.acknowledged);
        assert!(state.pending.is_none());
        assert_eq!(state.error, "Win32 failed");
    }
}
