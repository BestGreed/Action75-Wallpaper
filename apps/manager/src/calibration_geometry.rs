//! Pure calibration coordinate conversions. No window, input or file access.
//!
//! The native player stores normalized coordinates against the whole physical
//! display. Egui uses points against the actual zero-margin content canvas.
//! Keep DPI conversion at the window boundary: do not apply it again to keys.

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CalibrationGeometry {
    /// Physical desktop x, y, width and height, including negative origins.
    physical: [i32; 4],
    /// Egui content left, top, width and height, in logical points.
    canvas: [f64; 4],
}

impl CalibrationGeometry {
    pub fn new(physical: [i32; 4], canvas: [f64; 4]) -> Option<Self> {
        if physical[2] <= 0
            || physical[3] <= 0
            || !canvas.iter().all(|value| value.is_finite())
            || canvas[2] <= 0.0
            || canvas[3] <= 0.0
        {
            return None;
        }
        Some(Self { physical, canvas })
    }

    pub fn normalized_to_canvas(self, point: [f64; 2]) -> [f64; 2] {
        [
            self.canvas[0] + point[0] * self.canvas[2],
            self.canvas[1] + point[1] * self.canvas[3],
        ]
    }

    pub fn canvas_to_normalized(self, point: [f64; 2]) -> [f64; 2] {
        [
            (point[0] - self.canvas[0]) / self.canvas[2],
            (point[1] - self.canvas[1]) / self.canvas[3],
        ]
    }

    #[cfg(test)]
    fn normalized_to_physical(self, point: [f64; 2]) -> [f64; 2] {
        [
            f64::from(self.physical[0]) + point[0] * f64::from(self.physical[2]),
            f64::from(self.physical[1]) + point[1] * f64::from(self.physical[3]),
        ]
    }

    #[cfg(test)]
    fn physical_to_normalized(self, point: [f64; 2]) -> [f64; 2] {
        [
            (point[0] - f64::from(self.physical[0])) / f64::from(self.physical[2]),
            (point[1] - f64::from(self.physical[1])) / f64::from(self.physical[3]),
        ]
    }

    /// Arrow keys: [1,0] is exactly one physical display pixel at every DPI.
    /// Shift can pass [10,0]. The caller retains its existing [0,1] clamping.
    pub fn normalized_delta_for_physical_pixels(self, delta: [f64; 2]) -> [f64; 2] {
        [
            delta[0] / f64::from(self.physical[2]),
            delta[1] / f64::from(self.physical[3]),
        ]
    }

    /// Pointer deltas arrive in egui points, not physical pixels.
    #[cfg(test)]
    fn normalized_delta_for_canvas_points(self, delta: [f64; 2]) -> [f64; 2] {
        [delta[0] / self.canvas[2], delta[1] / self.canvas[3]]
    }

    /// Size for ViewportBuilder/ViewportCommand::InnerSize. Use the calibration
    /// viewport's current pixels_per_point (including egui zoom), not the main
    /// window's DPI nor just Windows scale_factor.
    pub fn logical_size(self, pixels_per_point: f64) -> Option<[f64; 2]> {
        valid_scale(pixels_per_point).then(|| {
            [
                f64::from(self.physical[2]) / pixels_per_point,
                f64::from(self.physical[3]) / pixels_per_point,
            ]
        })
    }

    /// Logical outer-position command corresponding to the native PMv2 origin.
    pub fn logical_position(self, pixels_per_point: f64) -> Option<[f64; 2]> {
        valid_scale(pixels_per_point).then(|| {
            [
                f64::from(self.physical[0]) / pixels_per_point,
                f64::from(self.physical[1]) / pixels_per_point,
            ]
        })
    }

    /// Only the caller queries its owned HWND. `actual` is physical client
    /// desktop x/y/width/height, not outer decorations or egui logical bounds.
    pub fn client_matches_target(self, actual: [i32; 4], tolerance_pixels: i32) -> bool {
        if actual[2] <= 0 || actual[3] <= 0 || tolerance_pixels < 0 {
            return false;
        }
        actual.iter().zip(self.physical).all(|(&actual, expected)| {
            (i64::from(actual) - i64::from(expected)).abs() <= i64::from(tolerance_pixels)
        })
    }

    /// A correctly placed HWND alone is insufficient: wait until winit's
    /// resize/DPI events have updated the actual egui content canvas too.
    pub fn canvas_matches_target(self, pixels_per_point: f64, tolerance_pixels: f64) -> bool {
        valid_scale(pixels_per_point)
            && tolerance_pixels.is_finite()
            && tolerance_pixels >= 0.0
            && self.canvas[0].abs() * pixels_per_point <= tolerance_pixels
            && self.canvas[1].abs() * pixels_per_point <= tolerance_pixels
            && (self.canvas[2] * pixels_per_point - f64::from(self.physical[2])).abs()
                <= tolerance_pixels
            && (self.canvas[3] * pixels_per_point - f64::from(self.physical[3])).abs()
                <= tolerance_pixels
    }
}

fn valid_scale(value: f64) -> bool {
    value.is_finite() && value > 0.0
}

#[cfg(test)]
mod tests {
    use super::CalibrationGeometry;

    const TARGET: [i32; 4] = [-1920, -648, 1920, 720];

    fn close(actual: [f64; 2], expected: [f64; 2]) {
        for axis in 0..2 {
            assert!(
                (actual[axis] - expected[axis]).abs() < 1e-9,
                "{actual:?} != {expected:?}"
            );
        }
    }

    fn geometry(scale: f64) -> CalibrationGeometry {
        CalibrationGeometry::new(TARGET, [0.0, 0.0, 1920.0 / scale, 720.0 / scale]).unwrap()
    }

    #[test]
    fn normalized_canvas_and_global_physical_roundtrip_at_each_dpi() {
        for scale in [1.0, 1.25, 1.5, 2.0, 1.5 * 1.2] {
            let g = geometry(scale);
            for point in [[0.0, 0.0], [1.0, 1.0], [0.5, 0.5], [0.127, 0.881]] {
                close(g.canvas_to_normalized(g.normalized_to_canvas(point)), point);
                close(
                    g.physical_to_normalized(g.normalized_to_physical(point)),
                    point,
                );
                let canvas = g.normalized_to_canvas(point);
                close(
                    [canvas[0] * scale - 1920.0, canvas[1] * scale - 648.0],
                    g.normalized_to_physical(point),
                );
            }
            assert!(g.canvas_matches_target(scale, 0.001));
        }
    }

    #[test]
    fn negative_desktop_origin_and_corners_are_exact() {
        let g = geometry(1.5);
        close(g.normalized_to_physical([0.0, 0.0]), [-1920.0, -648.0]);
        close(g.normalized_to_physical([1.0, 1.0]), [0.0, 72.0]);
        close(g.normalized_to_physical([0.5, 0.5]), [-960.0, -288.0]);
    }

    #[test]
    fn all_eighty_three_sample_keys_map_identically_at_125_and_150_percent() {
        for scale in [1.25, 1.5] {
            let g = geometry(scale);
            for key in 0..83 {
                let normalized = [
                    f64::from((key * 37) % 101) / 100.0,
                    f64::from((key * 23) % 97) / 96.0,
                ];
                let canvas = g.normalized_to_canvas(normalized);
                close(
                    [canvas[0] * scale - 1920.0, canvas[1] * scale - 648.0],
                    g.normalized_to_physical(normalized),
                );
            }
        }
    }

    #[test]
    fn arrow_steps_are_one_or_ten_physical_pixels_at_all_dpi() {
        for scale in [1.0, 1.25, 1.5, 2.0] {
            let g = geometry(scale);
            for pixels in [[1.0, 0.0], [0.0, -1.0], [10.0, 10.0]] {
                let delta = g.normalized_delta_for_physical_pixels(pixels);
                let before = g.normalized_to_physical([0.5, 0.5]);
                let after = g.normalized_to_physical([0.5 + delta[0], 0.5 + delta[1]]);
                close([after[0] - before[0], after[1] - before[1]], pixels);
            }
        }
    }

    #[test]
    fn pointer_point_deltas_match_their_physical_pixel_distance() {
        for scale in [1.25, 1.5, 1.8] {
            let g = geometry(scale);
            let delta = g.normalized_delta_for_canvas_points([12.0 / scale, -7.0 / scale]);
            close([delta[0] * 1920.0, delta[1] * 720.0], [12.0, -7.0]);
        }
    }

    #[test]
    fn logical_viewport_commands_convert_back_to_exact_physical_target() {
        for scale in [1.0, 1.25, 1.5, 1.8] {
            let g = geometry(scale);
            let size = g.logical_size(scale).unwrap();
            let position = g.logical_position(scale).unwrap();
            close([size[0] * scale, size[1] * scale], [1920.0, 720.0]);
            close(
                [position[0] * scale, position[1] * scale],
                [-1920.0, -648.0],
            );
        }
    }

    #[test]
    fn canvas_margin_and_stale_oversized_logical_viewport_are_rejected() {
        let margin = CalibrationGeometry::new(TARGET, [8.0, 8.0, 1520.0, 560.0]).unwrap();
        assert!(!margin.canvas_matches_target(1.25, 1.0));
        let stale = CalibrationGeometry::new(TARGET, [0.0, 0.0, 1920.0, 720.0]).unwrap();
        assert!(!stale.canvas_matches_target(1.25, 1.0));
        assert!(!stale.canvas_matches_target(1.5, 1.0));
        assert!(geometry(1.25).canvas_matches_target(1.25, 1.0));
    }

    #[test]
    fn physical_client_check_includes_origin_size_and_safe_tolerance() {
        let g = geometry(1.25);
        assert!(g.client_matches_target(TARGET, 0));
        assert!(!g.client_matches_target([-1920, -648, 2400, 900], 1));
        assert!(!g.client_matches_target([-1912, -640, 1920, 720], 1));
        assert!(!g.client_matches_target([0, 0, 1920, 720], 1));
        assert!(g.client_matches_target([-1919, -649, 1921, 719], 1));
        assert!(!g.client_matches_target(TARGET, -1));
        assert!(!g.client_matches_target([i32::MAX, i32::MIN, 1920, 720], 1));
    }

    #[test]
    fn invalid_dimensions_and_nonfinite_values_fail_closed() {
        for target in [[0, 0, 0, 720], [0, 0, 1920, -1]] {
            assert!(CalibrationGeometry::new(target, [0.0, 0.0, 100.0, 100.0]).is_none());
        }
        for canvas in [
            [0.0, 0.0, 0.0, 1.0],
            [0.0, 0.0, 1.0, -1.0],
            [f64::NAN, 0.0, 1.0, 1.0],
            [0.0, 0.0, f64::INFINITY, 1.0],
        ] {
            assert!(CalibrationGeometry::new(TARGET, canvas).is_none());
        }
        let g = geometry(1.25);
        for scale in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            assert!(g.logical_size(scale).is_none());
            assert!(g.logical_position(scale).is_none());
            assert!(!g.canvas_matches_target(scale, 1.0));
        }
        assert!(!g.canvas_matches_target(1.25, f64::NAN));
    }
}
