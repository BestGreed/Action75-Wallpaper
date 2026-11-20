use crate::ui_style;
use action75_manager::storage::InterfaceTheme;
use eframe::egui::{
    self, Align2, Color32, FontId, Pos2, Rect, Response, Sense, Stroke, StrokeKind, Ui, vec2,
};

#[derive(Clone, Copy)]
pub enum Icon {
    Library,
    Calibration,
    Devices,
    Performance,
    Settings,
    Search,
    ClosePanel,
    OpenPanel,
    Refresh,
    MinimizeWindow,
    MaximizeWindow,
    RestoreWindow,
    CloseWindow,
}

pub fn navigation(
    ui: &mut Ui,
    name: &str,
    glyph: Icon,
    selected: bool,
    enabled: bool,
    height: f32,
) -> Response {
    ui.add_enabled_ui(enabled, |ui| {
        let (rect, response) =
            ui.allocate_exact_size(vec2(ui.available_width(), height), Sense::click());
        response.widget_info(|| {
            egui::WidgetInfo::selected(egui::WidgetType::SelectableLabel, enabled, selected, name)
        });
        let amount = ui_style::animate(
            ui.ctx(),
            response.id.with("selected"),
            selected,
            ui_style::PAGE,
        );
        let hover = ui_style::animate(
            ui.ctx(),
            response.id.with("hover"),
            response.hovered() && enabled,
            ui_style::FEEDBACK,
        );
        ui_style::nav_wash(ui, rect, amount.max(hover * 0.25));
        let color = if selected || response.hovered() {
            ui.visuals().text_color()
        } else {
            ui.visuals().weak_text_color()
        };
        paint_icon(ui, glyph, rect.center() - vec2(0., 12.), 24., color);
        ui.painter().text(
            rect.center() + vec2(0., 21.),
            Align2::CENTER_CENTER,
            name,
            FontId::proportional(13.),
            color,
        );
        focus_outline(ui, &response);
        response.on_hover_cursor(egui::CursorIcon::PointingHand)
    })
    .inner
}

pub fn icon_button(ui: &mut Ui, glyph: Icon, name: &str, size: f32) -> Response {
    let (rect, response) = ui.allocate_exact_size(vec2(size, size), Sense::click());
    response
        .widget_info(|| egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), name));
    let hover = ui_style::animate(
        ui.ctx(),
        response.id.with("hover"),
        response.hovered(),
        ui_style::FEEDBACK,
    );
    if hover > 0. {
        ui.painter().rect_filled(
            rect,
            5,
            ui.visuals().selection.bg_fill.gamma_multiply(hover * 0.65),
        );
    }
    paint_icon(
        ui,
        glyph,
        rect.center(),
        size * 0.59,
        ui.visuals().text_color(),
    );
    focus_outline(ui, &response);
    response
        .on_hover_text(name)
        .on_hover_cursor(egui::CursorIcon::PointingHand)
}

pub fn theme_key(ui: &mut Ui, theme: InterfaceTheme, selected: bool) -> Response {
    let (rect, response) = ui.allocate_exact_size(vec2(28., 29.), Sense::click());
    let label = theme_name(theme);
    response.widget_info(|| {
        egui::WidgetInfo::selected(egui::WidgetType::Button, ui.is_enabled(), selected, label)
    });
    let amount = ui_style::animate(
        ui.ctx(),
        response.id.with("press"),
        response.is_pointer_button_down_on(),
        ui_style::FEEDBACK,
    );
    let key = Rect::from_center_size(rect.center() + vec2(0., amount), vec2(17., 15.));
    let (face, base) = match theme {
        InterfaceTheme::Paper => (Color32::from_gray(246), Color32::from_gray(169)),
        InterfaceTheme::Graphite => (Color32::from_gray(57), Color32::from_gray(27)),
    };
    ui.painter()
        .rect_filled(key.translate(vec2(0., (1.7 - amount).max(0.))), 4, base);
    ui.painter().rect_filled(key, 4, face);
    if selected || response.hovered() || response.has_focus() {
        ui.painter().rect_stroke(
            key.expand(3.),
            6,
            Stroke::new(1_f32, ui.visuals().weak_text_color()),
            StrokeKind::Inside,
        );
    }
    response
        .on_hover_text(label)
        .on_hover_cursor(egui::CursorIcon::PointingHand)
}

pub fn theme_name(theme: InterfaceTheme) -> &'static str {
    match theme {
        InterfaceTheme::Paper => "纸白",
        InterfaceTheme::Graphite => "石墨灰",
    }
}

/// Draw the caption actions in the right-to-left top bar.
pub fn window_buttons(ui: &mut Ui, maximized: bool) {
    use egui::ViewportCommand;

    let (middle, middle_label) = if maximized {
        (Icon::RestoreWindow, "还原窗口")
    } else {
        (Icon::MaximizeWindow, "最大化")
    };
    ui.scope(|ui| {
        ui.spacing_mut().item_spacing.x = 2.;
        for (icon, label, command) in [
            (Icon::CloseWindow, "关闭控制台", ViewportCommand::Close),
            (middle, middle_label, ViewportCommand::Maximized(!maximized)),
            (
                Icon::MinimizeWindow,
                "最小化",
                ViewportCommand::Minimized(true),
            ),
        ] {
            let (rect, response) = ui.allocate_exact_size(vec2(38., 32.), Sense::click());
            response.widget_info(|| {
                egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), label)
            });
            let pressed = response.is_pointer_button_down_on();
            let mut color = ui.visuals().text_color();
            if response.hovered() || pressed {
                let fill = if matches!(icon, Icon::CloseWindow) {
                    color = Color32::WHITE;
                    if pressed {
                        Color32::from_rgb(156, 26, 37)
                    } else {
                        Color32::from_rgb(196, 38, 50)
                    }
                } else {
                    ui.visuals()
                        .selection
                        .bg_fill
                        .gamma_multiply(if pressed { 0.9 } else { 0.58 })
                };
                ui.painter().rect_filled(rect, 5, fill);
            }
            paint_icon(ui, icon, rect.center(), 20., color);
            focus_outline(ui, &response);
            if response.on_hover_text(label).clicked() {
                ui.ctx().send_viewport_cmd(command);
            }
        }
    });
}

pub fn search(ui: &mut Ui, query: &mut String) -> Response {
    let (icon, _) = ui.allocate_exact_size(vec2(18., 24.), Sense::hover());
    paint_icon(
        ui,
        Icon::Search,
        icon.center(),
        18.,
        ui.visuals().weak_text_color(),
    );
    let focus = !ui.ctx().wants_keyboard_input()
        && ui.input_mut(|i| i.consume_key(egui::Modifiers::NONE, egui::Key::Slash));
    let response = ui.add(
        egui::TextEdit::singleline(query)
            .id_salt("wallpaper-search")
            .desired_width(110.)
            .frame(false)
            .hint_text("搜索壁纸"),
    );
    if focus {
        response.request_focus();
    }
    if response.has_focus() {
        ui.painter().hline(
            response.rect.x_range(),
            response.rect.bottom(),
            Stroke::new(1_f32, ui.visuals().weak_text_color()),
        );
    }
    let (rect, shortcut) = ui.allocate_exact_size(vec2(20., 19.), Sense::click());
    let face = rect.shrink(1.);
    ui.painter().rect_filled(
        face.translate(vec2(0., 1.)),
        3,
        ui.visuals().widgets.inactive.bg_stroke.color,
    );
    ui.painter()
        .rect_filled(face, 3, ui.visuals().widgets.inactive.bg_fill);
    ui.painter().text(
        rect.center(),
        Align2::CENTER_CENTER,
        "/",
        FontId::proportional(12.),
        ui.visuals().weak_text_color(),
    );
    shortcut.widget_info(|| {
        egui::WidgetInfo::labeled(egui::WidgetType::Button, ui.is_enabled(), "搜索壁纸")
    });
    if shortcut.clicked() {
        response.request_focus();
    }
    shortcut
        .on_hover_text("按 / 搜索")
        .on_hover_cursor(egui::CursorIcon::PointingHand);
    response
}

fn focus_outline(ui: &Ui, response: &Response) {
    if response.has_focus() {
        ui.painter().rect_stroke(
            response.rect.shrink(1.),
            7,
            Stroke::new(1_f32, ui.visuals().weak_text_color()),
            StrokeKind::Inside,
        );
    }
}

fn paint_icon(ui: &Ui, icon: Icon, center: Pos2, size: f32, color: Color32) {
    let scale = size / 24.;
    let p = |x: f32, y: f32| center + vec2(x - 12., y - 12.) * scale;
    let painter = ui.painter();
    let stroke = Stroke::new((1.45 * scale).max(1.), color);
    let line =
        |a: [f32; 2], b: [f32; 2]| painter.line_segment([p(a[0], a[1]), p(b[0], b[1])], stroke);
    let path = |xy: &[[f32; 2]]| {
        if xy.len() >= 2 {
            painter.add(egui::Shape::line(
                xy.iter().map(|xy| p(xy[0], xy[1])).collect(),
                stroke,
            ));
        }
    };
    match icon {
        Icon::Library => {
            path(&[
                [4., 15.5],
                [6., 14.6],
                [8.3, 11.3],
                [10.5, 10.5],
                [12.5, 11.3],
                [15.1, 14.6],
                [17., 15.5],
                [20., 15.5],
            ]);
            line([6., 19.], [18., 19.]);
            painter.circle_stroke(p(16.3, 7.8), 1.55 * scale, stroke);
        }
        Icon::Calibration => {
            for (x, y, dx, dy) in [
                (4.5, 4.5, 1., 1.),
                (19.5, 4.5, -1., 1.),
                (4.5, 19.5, 1., -1.),
                (19.5, 19.5, -1., -1.),
            ] {
                path(&[
                    [x, y + 3.6 * dy],
                    [x, y + 1.35 * dy],
                    [x + 1.35 * dx, y],
                    [x + 3.6 * dx, y],
                ]);
            }
            painter.circle_filled(p(12., 12.), 1.1 * scale, color);
        }
        Icon::Devices => {
            path(&[
                [4., 15.],
                [4., 7.],
                [5.5, 5.5],
                [18.5, 5.5],
                [20., 7.],
                [20., 14.],
                [18.5, 15.5],
                [9.5, 15.5],
            ]);
            line([12., 16.], [12., 19.5]);
            line([8.5, 19.5], [15.5, 19.5]);
        }
        Icon::Performance => {
            path(&[[4.5, 5.], [4.5, 18.5], [20., 18.5]]);
            path(&[[7.5, 14.], [11., 10.5], [14., 13.], [19.5, 6.5]]);
        }
        Icon::Settings => {
            for (x, middle) in [(8., 10.5), (16., 13.5)] {
                line([x, 4.5], [x, middle - 2.]);
                line([x, middle + 2.], [x, 19.5]);
                line([x - 2., middle], [x + 2., middle]);
            }
        }
        Icon::Search => {
            painter.circle_stroke(p(10.4, 10.4), 6.1 * scale, stroke);
            line([14.9, 14.9], [20., 20.]);
        }
        Icon::ClosePanel => path(&[[9., 5.], [15.5, 12.], [9., 19.]]),
        Icon::OpenPanel => path(&[[15., 5.], [8.5, 12.], [15., 19.]]),
        Icon::MinimizeWindow => {
            line([6., 14.], [18., 14.]);
        }
        Icon::MaximizeWindow => {
            painter.rect_stroke(
                Rect::from_min_max(p(6., 6.), p(18., 18.)),
                1,
                stroke,
                StrokeKind::Middle,
            );
        }
        Icon::RestoreWindow => {
            path(&[[9., 9.], [9., 5.], [19., 5.], [19., 15.], [15., 15.]]);
            painter.rect_stroke(
                Rect::from_min_max(p(5., 9.), p(15., 19.)),
                1,
                stroke,
                StrokeKind::Middle,
            );
        }
        Icon::CloseWindow => {
            line([6., 6.], [18., 18.]);
            line([18., 6.], [6., 18.]);
        }
        Icon::Refresh => {
            path(&[
                [20., 10.],
                [18., 6.],
                [14.8, 4.3],
                [10.1, 4.3],
                [6.3, 6.8],
                [4.5, 10.6],
                [5.2, 15.],
                [8.1, 18.5],
                [12.4, 19.7],
                [17.3, 17.8],
            ]);
            path(&[[15.1, 10.], [20., 10.], [20., 5.2]]);
        }
    };
}
