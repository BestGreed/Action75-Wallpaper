use eframe::egui::{
    self, Color32, Painter, Pos2, Rect, Shape, Stroke, StrokeKind, TextureHandle, Vec2,
    epaint::RectShape, layers::ShapeIdx, pos2,
};

use std::sync::Arc;

use crate::ui_style;

pub struct Tile {
    pub rect: Rect,
    pub cell: Vec2,
    pub frost: Option<Arc<egui::ColorImage>>,
}

struct Slide {
    from: Vec2,
    to: Vec2,
    started: f64,
}

impl Slide {
    fn position(&self, now: f64, duration: f32) -> Vec2 {
        let elapsed = (now - self.started) as f32;
        if elapsed >= duration {
            return self.to;
        }
        self.from + (self.to - self.from) * ui_style::ease_out((elapsed / duration).max(0.))
    }
}

#[derive(Default)]
pub struct SelectionFrame {
    slide: Option<Slide>,
    columns: usize,
    frost: Option<(Arc<egui::ColorImage>, TextureHandle)>,
}

impl SelectionFrame {
    pub fn reset(&mut self) {
        self.slide = None;
        self.frost = None;
    }

    fn follow(&mut self, ctx: &egui::Context, cell: Vec2, columns: usize) -> Vec2 {
        let duration = ui_style::motion_duration(ctx, ui_style::PANEL);
        if self.columns != columns || duration == 0. {
            self.slide = None;
            self.columns = columns;
        }
        let now = ctx.input(|input| input.time);
        let slide = self.slide.get_or_insert(Slide {
            from: cell,
            to: cell,
            started: now,
        });
        if slide.to != cell {
            *slide = Slide {
                from: slide.position(now, duration),
                to: cell,
                started: now,
            };
        }
        let position = slide.position(now, duration);
        if position != cell {
            ctx.request_repaint();
        }
        position
    }

    pub fn paint(
        &mut self,
        painter: &Painter,
        slot: ShapeIdx,
        tile: Option<Tile>,
        columns: usize,
        step: Vec2,
    ) {
        let Some(tile) = tile else {
            self.reset();
            return;
        };
        // Measure the motion in grid cells so scrolling moves the glass with the artwork.
        let position = self.follow(painter.ctx(), tile.cell, columns);
        let rect = tile.rect.translate((position - tile.cell) * step);
        if !rect.intersects(painter.clip_rect()) {
            return;
        }
        let style = painter.ctx().style();
        let colors = style.visuals.selection;
        let tint = colors.bg_fill.gamma_multiply(0.86);
        let rim = Stroke::new(1_f32, colors.stroke.color.gamma_multiply(0.33));
        let mut shapes = vec![Shape::from(
            RectShape::new(rect, 9, tint, rim, StrokeKind::Inside).with_round_to_pixels(false),
        )];
        if let Some(mask) = tile.frost {
            // Only the selected card displays this mask. Keep one texture for it.
            let (previous, texture) = self.frost.get_or_insert_with(|| {
                (
                    mask.clone(),
                    painter.ctx().load_texture(
                        "wallpaper selection glass",
                        mask.clone(),
                        egui::TextureOptions::LINEAR,
                    ),
                )
            });
            if !Arc::ptr_eq(previous, &mask) {
                if previous.size == mask.size {
                    texture.set_partial([0, 0], mask.clone(), egui::TextureOptions::LINEAR);
                } else {
                    // Preview aspect ratios differ. A partial update cannot resize
                    // the existing texture or discard pixels outside the new mask.
                    texture.set(mask.clone(), egui::TextureOptions::LINEAR);
                }
                *previous = mask;
            }
            shapes.push(Shape::from(
                RectShape::filled(rect.shrink(1.), 8, Color32::WHITE)
                    .with_round_to_pixels(false)
                    .with_texture(texture.id(), Rect::from_min_max(Pos2::ZERO, pos2(1., 1.))),
            ));
        } else {
            self.frost = None;
        }
        painter.set(slot, Shape::Vec(shapes));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui::{RawInput, ViewportId, vec2};
    use std::time::Duration;

    #[test]
    fn selection_mask_reuses_one_texture_and_releases_it_when_hidden() {
        let ctx = egui::Context::default();
        let mut frame = SelectionFrame::default();
        let mut render = |mask: Option<Arc<egui::ColorImage>>| {
            ctx.run(RawInput::default(), |ctx| {
                let painter = ctx.debug_painter();
                let slot = painter.add(Shape::Noop);
                let tile = mask.clone().map(|mask| Tile {
                    rect: Rect::from_min_size(Pos2::ZERO, vec2(100., 50.)),
                    cell: Vec2::ZERO,
                    frost: Some(mask),
                });
                frame.paint(&painter, slot, tile, 1, vec2(100., 50.));
            })
        };
        let mask = |c| Arc::new(egui::ColorImage::filled([24, 9], c));
        let first = mask(Color32::GRAY);
        let initial = render(Some(first.clone()));
        let id = initial
            .textures_delta
            .set
            .iter()
            .find(|(_, delta)| delta.image.size() == [24, 9])
            .unwrap()
            .0;
        let stable = render(Some(first));
        assert!(
            stable
                .textures_delta
                .set
                .iter()
                .all(|(other, _)| *other != id)
        );
        let changed = render(Some(mask(Color32::WHITE)));
        assert!(
            changed
                .textures_delta
                .set
                .iter()
                .any(|(other, delta)| { *other == id && delta.pos == Some([0, 0]) })
        );
        assert!(render(None).textures_delta.free.contains(&id));
    }

    #[test]
    fn selecting_different_preview_aspect_ratios_replaces_the_whole_mask() {
        let ctx = egui::Context::default();
        let mut selection = SelectionFrame::default();
        let mut texture_id = None;
        for size in [[24, 9], [9, 9], [24, 9]] {
            let mask = Arc::new(egui::ColorImage::filled(size, Color32::WHITE));
            let output = ctx.run(RawInput::default(), |ctx| {
                let painter = ctx.debug_painter();
                let slot = painter.add(Shape::Noop);
                selection.paint(
                    &painter,
                    slot,
                    Some(Tile {
                        rect: Rect::from_min_size(Pos2::ZERO, vec2(100., 50.)),
                        cell: Vec2::ZERO,
                        frost: Some(mask.clone()),
                    }),
                    1,
                    vec2(100., 50.),
                );
            });
            let id = selection.frost.as_ref().unwrap().1.id();
            if let Some(previous) = texture_id {
                assert_eq!(id, previous);
            }
            texture_id = Some(id);
            let delta = &output
                .textures_delta
                .set
                .iter()
                .find(|(key, _)| *key == id)
                .unwrap()
                .1;
            assert_eq!(
                delta.pos, None,
                "different dimensions require a full upload"
            );
            assert_eq!(delta.image.size(), size);
            assert_eq!(ctx.tex_manager().read().meta(id).unwrap().size, size);
        }
    }

    #[test]
    fn slide_retargets_from_the_in_flight_position_and_stops_repainting() {
        let ctx = egui::Context::default();
        let mut selection = SelectionFrame::default();
        let mut frame = |time, cell| {
            let mut position = Vec2::ZERO;
            let output = ctx.run(
                RawInput {
                    time: Some(time),
                    ..Default::default()
                },
                |ctx| position = selection.follow(ctx, cell, 3),
            );
            (
                position,
                output.viewport_output[&ViewportId::ROOT].repaint_delay,
            )
        };
        let first = Vec2::ZERO;
        let second = vec2(2., 0.);
        let last = vec2(1., 2.);
        assert_eq!(frame(0., first).0, first);
        assert_eq!(frame(0.05, second).0, first);
        let halfway = frame(0.11, second).0;
        assert!(first.x < halfway.x && halfway.x < second.x);
        assert_eq!(frame(0.11, last).0, halfway);
        assert!(frame(0.16, last).0.y > 0.);
        assert_eq!(frame(0.38, last).0, last);
        assert_eq!(frame(0.40, last).1, Duration::MAX);

        ctx.style_mut(|style| style.animation_time = 0.);
        assert_eq!(frame(0.41, first).0, first);
    }
}
