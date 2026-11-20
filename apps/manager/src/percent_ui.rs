#[cfg(test)]
use action75_manager::PercentScale;
use action75_manager::{Settings, single_color_hue_visible};
use eframe::egui::{self, Align, Color32, RichText};

pub fn slider_row(
    ui: &mut egui::Ui,
    label: &str,
    value: String,
    add_slider: impl FnOnce(&mut egui::Ui) -> egui::Response,
) -> egui::Response {
    // Advanced controls inherit the panel style, but their indent reduces the row width.
    let width = ui.available_width();
    ui.horizontal(|ui| {
        ui.with_layout(egui::Layout::right_to_left(Align::Center), |ui| {
            ui.add(egui::Label::new(RichText::new(value).weak().size(12.)).wrap());
            ui.with_layout(egui::Layout::left_to_right(Align::Center), |ui| {
                ui.add(egui::Label::new(label).wrap());
            });
        });
    });
    ui.scope(|ui| {
        ui.spacing_mut().slider_width = width;
        add_slider(ui)
    })
    .inner
}
#[cfg(test)]
pub fn slider(
    ui: &mut egui::Ui,
    label: &str,
    factor: &mut f32,
    scale: PercentScale,
) -> egui::Response {
    let mut percent = scale.percent(*factor);
    let response = slider_row(ui, label, format!("{percent:.0}"), |ui| {
        ui.add(
            egui::Slider::new(&mut percent, 0.0..=100.0)
                .show_value(false)
                .step_by(1.0),
        )
    });
    if response.changed() {
        *factor = scale.factor(percent);
    }
    ui.add_space(8.);
    response
}
pub fn single_color_hue(ui: &mut egui::Ui, settings: &mut Settings) -> Option<egui::Response> {
    if !single_color_hue_visible(settings) {
        return None;
    }
    let c = settings.palette_colors[0];
    let mut hsv = egui::ecolor::Hsva::from(Color32::from_rgb(c[0], c[1], c[2]));
    let mut degrees = hsv.h * 360.;
    let response = slider_row(ui, "色相", format!("{degrees:.0}°"), |ui| {
        ui.add(
            egui::Slider::new(&mut degrees, 0.0..=360.0)
                .show_value(false)
                .step_by(1.),
        )
    });
    if response.changed() {
        hsv.h = (degrees / 360.).rem_euclid(1.);
        let c = Color32::from(hsv).to_array();
        settings.palette_colors[0] = [c[0], c[1], c[2]];
        settings.hue = 0.;
    }
    Some(response)
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{Event, Modifiers, PointerButton, Pos2, RawInput, Rect, vec2};
    fn frame(
        ctx: &egui::Context,
        factor: &mut f32,
        scale: PercentScale,
        events: Vec<Event>,
    ) -> Rect {
        let mut rect = Rect::NOTHING;
        let _ = ctx.run(
            RawInput {
                screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(500., 350.))),
                events,
                ..Default::default()
            },
            |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    ui.set_width(260.);
                    rect = slider(ui, "Brightness", factor, scale).rect;
                });
            },
        );
        rect
    }
    #[test]
    fn real_percent_sliders_reach_zero_and_maximum() {
        for (scale, max) in [
            (PercentScale::Brightness, 4.),
            (PercentScale::Saturation, 2.5),
            (PercentScale::Intensity, 3.),
        ] {
            let ctx = egui::Context::default();
            let mut factor = 1.;
            let rect = frame(&ctx, &mut factor, scale, vec![]);
            for (x, expected) in [(rect.left() + 1., 0.), (rect.right() - 1., max)] {
                let pos = Pos2::new(x, rect.center().y);
                frame(&ctx, &mut factor, scale, vec![Event::PointerMoved(pos)]);
                frame(
                    &ctx,
                    &mut factor,
                    scale,
                    vec![Event::PointerButton {
                        pos,
                        button: PointerButton::Primary,
                        pressed: true,
                        modifiers: Modifiers::NONE,
                    }],
                );
                frame(
                    &ctx,
                    &mut factor,
                    scale,
                    vec![Event::PointerButton {
                        pos,
                        button: PointerButton::Primary,
                        pressed: false,
                        modifiers: Modifiers::NONE,
                    }],
                );
                assert!(
                    (factor - expected).abs() < 1e-5,
                    "{scale:?}: {factor} != {expected}"
                );
            }
        }
    }
    #[test]
    fn hue_widget_hidden_for_original_and_multiple_colors() {
        let ctx = egui::Context::default();
        let mut settings = Settings::default();
        for (palette, colors, visible) in [
            ("original", vec![[255, 0, 0]], false),
            ("custom", vec![[255, 0, 0], [0, 255, 0]], false),
            ("candy", vec![[255, 0, 0]], false),
            ("custom", vec![[255, 0, 0]], true),
        ] {
            settings.palette = palette.into();
            settings.palette_colors = colors;
            let mut shown = false;
            let output = ctx.run(RawInput::default(), |ctx| {
                egui::CentralPanel::default().show(ctx, |ui| {
                    shown = single_color_hue(ui, &mut settings).is_some();
                });
            });
            assert_eq!(shown, visible);
            if !visible {
                assert!(!output.shapes.iter().any(
                    |s| matches!(&s.shape,egui::Shape::Text(t)if t.galley.job.text.contains("色相"))
                ));
            }
        }
    }
    #[test]
    fn single_hue_slider_changes_absolute_rgb_and_clears_legacy_hue() {
        fn draw(ctx: &egui::Context, settings: &mut Settings, events: Vec<Event>) -> Rect {
            let mut rect = Rect::NOTHING;
            let _ = ctx.run(
                RawInput {
                    screen_rect: Some(Rect::from_min_size(Pos2::ZERO, vec2(500., 350.))),
                    events,
                    ..Default::default()
                },
                |ctx| {
                    egui::CentralPanel::default().show(ctx, |ui| {
                        rect = single_color_hue(ui, settings).unwrap().rect;
                    });
                },
            );
            rect
        }
        let ctx = egui::Context::default();
        let mut settings = Settings::default();
        settings.palette = "custom".into();
        settings.palette_colors = vec![[255, 0, 0]];
        settings.hue = 75.;
        settings.brightness = 4.;
        settings.intensity = 0.8;
        settings.scale = 1.2;
        let rect = draw(&ctx, &mut settings, vec![]);
        let pos = rect.center();
        draw(&ctx, &mut settings, vec![Event::PointerMoved(pos)]);
        draw(
            &ctx,
            &mut settings,
            vec![Event::PointerButton {
                pos,
                button: PointerButton::Primary,
                pressed: true,
                modifiers: Modifiers::NONE,
            }],
        );
        draw(
            &ctx,
            &mut settings,
            vec![Event::PointerButton {
                pos,
                button: PointerButton::Primary,
                pressed: false,
                modifiers: Modifiers::NONE,
            }],
        );
        let c = settings.palette_colors[0];
        assert!(c[0] < 5 && c[1] > 250 && c[2] > 250);
        assert_eq!(settings.hue, 0.);
        assert_eq!(
            (settings.brightness, settings.intensity, settings.scale),
            (4., 0.8, 1.2)
        );
    }
}
