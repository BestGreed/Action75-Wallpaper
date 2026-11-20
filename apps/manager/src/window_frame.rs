use eframe::egui;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{cell::Cell, rc::Rc};
use windows::Win32::{
    Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM},
    Graphics::Gdi::ScreenToClient,
    UI::{
        HiDpi::{GetDpiForWindow, GetSystemMetricsForDpi},
        Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass},
        WindowsAndMessaging::{
            GetWindowRect, HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT, HTCAPTION, HTLEFT, HTRIGHT,
            HTTOP, HTTOPLEFT, HTTOPRIGHT, IsZoomed, SM_CXPADDEDBORDER, SM_CXSIZEFRAME,
            SM_CYSIZEFRAME, WM_NCDESTROY, WM_NCHITTEST,
        },
    },
};

#[cfg_attr(test, derive(Default))]
pub struct WindowFrame {
    caption: Rc<Cell<RECT>>,
}

impl WindowFrame {
    pub fn attach(window: &impl HasWindowHandle) -> Option<Self> {
        let RawWindowHandle::Win32(handle) = window.window_handle().ok()?.as_raw() else {
            return None;
        };
        let hwnd = HWND(handle.hwnd.get() as *mut _);
        let caption = Rc::new(Cell::new(RECT::default()));
        // The subclass owns one reference until its window is destroyed.
        let data = Rc::into_raw(Rc::clone(&caption));
        let installed = unsafe { SetWindowSubclass(hwnd, Some(hit_test), 0, data as usize) };
        if !installed.as_bool() {
            unsafe { drop(Rc::from_raw(data)) };
            return None;
        }
        Some(Self { caption })
    }

    pub fn set_caption(&self, rect: egui::Rect, pixels_per_point: f32) {
        self.caption.set(RECT {
            left: (rect.left() * pixels_per_point).ceil() as i32,
            top: (rect.top() * pixels_per_point).ceil() as i32,
            right: (rect.right() * pixels_per_point).floor() as i32,
            bottom: (rect.bottom() * pixels_per_point).floor() as i32,
        });
    }
}

unsafe extern "system" fn hit_test(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    id: usize,
    data: usize,
) -> LRESULT {
    let caption = data as *const Cell<RECT>;
    if message == WM_NCHITTEST {
        // Both coordinates are signed because a monitor can be left of or above the primary.
        let mut point = POINT {
            x: i32::from(lparam.0 as i16),
            y: i32::from((lparam.0 >> 16) as i16),
        };
        let mut bounds = RECT::default();
        if !unsafe { IsZoomed(window) }.as_bool()
            && unsafe { GetWindowRect(window, &mut bounds) }.is_ok()
        {
            let dpi = unsafe { GetDpiForWindow(window) };
            let padding = unsafe { GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi) };
            let horizontal = unsafe { GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) } + padding;
            let vertical = unsafe { GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) } + padding;
            if let Some(edge) = resize_edge(bounds, point, horizontal, vertical) {
                return LRESULT(edge as isize);
            }
        }
        if unsafe { ScreenToClient(window, &mut point) }.as_bool()
            && contains(unsafe { (*caption).get() }, point)
        {
            return LRESULT(HTCAPTION as isize);
        }
    } else if message == WM_NCDESTROY
        && unsafe { RemoveWindowSubclass(window, Some(hit_test), id) }.as_bool()
    {
        unsafe { drop(Rc::from_raw(caption)) };
    }
    unsafe { DefSubclassProc(window, message, wparam, lparam) }
}

fn contains(rect: RECT, point: POINT) -> bool {
    (rect.left..rect.right).contains(&point.x) && (rect.top..rect.bottom).contains(&point.y)
}

fn resize_edge(bounds: RECT, point: POINT, horizontal: i32, vertical: i32) -> Option<u32> {
    if !contains(bounds, point) {
        return None;
    }
    let column = i32::from(point.x >= bounds.right - horizontal)
        - i32::from(point.x < bounds.left + horizontal);
    let row =
        i32::from(point.y >= bounds.bottom - vertical) - i32::from(point.y < bounds.top + vertical);
    match (column, row) {
        (-1, -1) => Some(HTTOPLEFT),
        (0, -1) => Some(HTTOP),
        (1, -1) => Some(HTTOPRIGHT),
        (-1, 0) => Some(HTLEFT),
        (1, 0) => Some(HTRIGHT),
        (-1, 1) => Some(HTBOTTOMLEFT),
        (0, 1) => Some(HTBOTTOM),
        (1, 1) => Some(HTBOTTOMRIGHT),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resizing_keeps_all_edges_and_corners_on_scaled_negative_origin_displays() {
        let bounds = RECT {
            left: -1800,
            top: -600,
            right: -720,
            bottom: 80,
        };
        for thickness in [8, 10, 12] {
            let left = bounds.left + thickness - 1;
            let right = bounds.right - thickness;
            let top = bounds.top + thickness - 1;
            let bottom = bounds.bottom - thickness;
            let center_x = (bounds.left + bounds.right) / 2;
            let center_y = (bounds.top + bounds.bottom) / 2;
            for (x, y, expected) in [
                (left, top, Some(HTTOPLEFT)),
                (center_x, top, Some(HTTOP)),
                (right, top, Some(HTTOPRIGHT)),
                (left, center_y, Some(HTLEFT)),
                (right, center_y, Some(HTRIGHT)),
                (left, bottom, Some(HTBOTTOMLEFT)),
                (center_x, bottom, Some(HTBOTTOM)),
                (right, bottom, Some(HTBOTTOMRIGHT)),
                (left + 1, top + 1, None),
                (right - 1, bottom - 1, None),
                (bounds.right, bottom, None),
            ] {
                assert_eq!(
                    resize_edge(bounds, POINT { x, y }, thickness, thickness),
                    expected
                );
            }
        }
    }
}
