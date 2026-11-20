"""Run the installed controller on a Windows desktop that is never activated."""

from contextlib import contextmanager
import ctypes as C
from ctypes import wintypes as W
import subprocess
import time
import uuid


def api(dll, name, result, *args):
    fn = getattr(dll, name)
    fn.restype = result
    fn.argtypes = args
    return fn


def checked(result):
    if not result:
        raise C.WinError(C.get_last_error())
    return result


class Startup(C.Structure):
    _fields_ = [
        ("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR), ("title", W.LPWSTR),
        *[(field, W.DWORD) for field in ("x", "y", "width", "height", "cols", "rows", "fill", "flags")],
        ("show", W.WORD), ("reserved_bytes", W.WORD), ("reserved_ptr", C.c_void_p),
        ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE),
    ]


class Process(C.Structure):
    _fields_ = [("handle", W.HANDLE), ("thread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]


USER = C.WinDLL("user32", use_last_error=True)
KERNEL = C.WinDLL("kernel32", use_last_error=True)
VISIT = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
create_desktop = api(USER, "CreateDesktopW", W.HANDLE, W.LPCWSTR, W.LPCWSTR,
                     C.c_void_p, W.DWORD, W.DWORD, C.c_void_p)
close_desktop = api(USER, "CloseDesktop", W.BOOL, W.HANDLE)
enum_desktop = api(USER, "EnumDesktopWindows", W.BOOL, W.HANDLE, VISIT, W.LPARAM)
enum_children = api(USER, "EnumChildWindows", W.BOOL, W.HWND, VISIT, W.LPARAM)
window_title = api(USER, "GetWindowTextW", C.c_int, W.HWND, W.LPWSTR, C.c_int)
window_class = api(USER, "GetClassNameW", C.c_int, W.HWND, W.LPWSTR, C.c_int)
window_owner = api(USER, "GetWindowThreadProcessId", W.DWORD, W.HWND, C.POINTER(W.DWORD))
client_rect = api(USER, "GetClientRect", W.BOOL, W.HWND, C.POINTER(W.RECT))
window_dpi = api(USER, "GetDpiForWindow", W.UINT, W.HWND)
thread_dpi = api(USER, "SetThreadDpiAwarenessContext", W.HANDLE, W.HANDLE)
visible = api(USER, "IsWindowVisible", W.BOOL, W.HWND)
post = api(USER, "PostMessageW", W.BOOL, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
send_timeout = api(USER, "SendMessageTimeoutW", W.LPARAM, W.HWND, W.UINT,
                   W.WPARAM, W.LPARAM, W.UINT, W.UINT, C.POINTER(C.c_size_t))
spawn = api(KERNEL, "CreateProcessW", W.BOOL, W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p,
            W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR, C.POINTER(Startup), C.POINTER(Process))
wait = api(KERNEL, "WaitForSingleObject", W.DWORD, W.HANDLE, W.DWORD)
terminate = api(KERNEL, "TerminateProcess", W.BOOL, W.HANDLE, W.UINT)
close = api(KERNEL, "CloseHandle", W.BOOL, W.HANDLE)
exit_code = api(KERNEL, "GetExitCodeProcess", W.BOOL, W.HANDLE, C.POINTER(W.DWORD))


def wait_for(callback, condition, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if result := callback():
            return result
        time.sleep(0.06)
    raise TimeoutError(f"Timed out waiting for {condition}")


def text_of(reader, window):
    text = C.create_unicode_buffer(256)
    reader(window, text, len(text))
    return text.value


def matching_windows(enumerate_windows, parent, predicate):
    windows = []

    @VISIT
    def collect(window, _):
        if predicate(window):
            windows.append(window)
        return True

    enumerate_windows(parent, collect, 0)
    return windows


@contextmanager
def manager_session(manager, app, *args):
    name = "Action75ReleaseQA-" + uuid.uuid4().hex
    desktop = checked(create_desktop(name, None, None, 0, 0x01FF, None))
    process = Process()
    try:
        command = C.create_unicode_buffer(subprocess.list2cmdline([str(manager), "--root", str(app), *args]))
        startup = Startup(cb=C.sizeof(Startup), desktop="WinSta0\\" + name)
        checked(spawn(str(manager), command, None, None, False, 0, None, str(app),
                      C.byref(startup), C.byref(process)))

        def windows(title, class_name=None):
            def matches(window):
                owner = W.DWORD()
                window_owner(window, C.byref(owner))
                return (owner.value == process.pid and title in text_of(window_title, window)
                        and (class_name is None or text_of(window_class, window) == class_name))

            return matching_windows(enum_desktop, desktop, matches)

        yield process, windows
    finally:
        if process.handle:
            if wait(process.handle, 0) != 0:
                for window in windows("Action75 Wallpaper"):
                    post(window, 0x0010, 0, 0)  # WM_CLOSE on the test controller
                if wait(process.handle, 8_000) != 0:
                    terminate(process.handle, 1)
                    wait(process.handle, 5_000)
            close(process.thread)
            close(process.handle)
        close_desktop(desktop)


def screenshot(manager, app, image, *args):
    with manager_session(manager, app, "--screenshot", str(image), "--exit-after-screenshot", *args) as (process, _):
        if wait(process.handle, 40_000) != 0:
            raise TimeoutError(f"Screenshot did not finish: {image.name}")
        code = W.DWORD()
        checked(exit_code(process.handle, C.byref(code)))
        if code.value or not image.is_file():
            raise RuntimeError(f"Controller screenshot failed with exit code {code.value}: {image.name}")


@contextmanager
def per_monitor_coordinates():
    previous = checked(thread_dpi(C.c_void_p(-4)))  # PER_MONITOR_AWARE_V2
    try:
        yield
    finally:
        checked(thread_dpi(previous))


def import_wallpaper(manager, app, source, published, ready_image, *, click=None):
    with per_monitor_coordinates(), manager_session(
        manager, app, "--screenshot-size", "1380", "880", "--screenshot", str(ready_image)
    ) as (_, windows):
        window = wait_for(lambda: windows("Action75 Wallpaper"), "the test controller")[0]
        wait_for(ready_image.is_file, "the first gallery frame", seconds=20)
        client = W.RECT()
        checked(client_rect(window, C.byref(client)))
        scale = window_dpi(window) / 96
        # The inspector is folded. Position relative to the gallery's right edge in logical pixels.
        logical_x, logical_y = click or (client.right / scale - 271, 106 if client.bottom / scale >= 760 else 98)
        x, y = round(logical_x * scale), round(logical_y * scale)
        point = (y << 16) | x
        for message, argument, position in (
            (0x0086, 1, 0), (0x0006, 1, 0), (0x0007, 0, 0),
            (0x0200, 0, point - 2), (0x0200, 0, point), (0x0201, 1, point), (0x0202, 0, point),
        ):
            checked(post(window, message, argument, position))

        def pickers():
            return windows("选择要导入", "#32770")

        dialog = wait_for(
            pickers, f"the import folder picker at logical position {logical_x}, {logical_y}"
        )[0]
        edits = wait_for(lambda: matching_windows(
            enum_children, dialog, lambda hwnd: visible(hwnd) and text_of(window_class, hwnd) == "Edit"),
            "the folder picker input")
        value = C.create_unicode_buffer(str(source))
        outcome = C.c_size_t()
        checked(send_timeout(edits[-1], 0x000C, 0, C.addressof(value), 2, 2_000, C.byref(outcome)))
        checked(outcome.value)
        checked(post(dialog, 0x0111, 1, 0))  # IDOK, scoped to this picker's PID and desktop
        time.sleep(0.35)
        if pickers():
            checked(post(dialog, 0x0111, 1, 0))
        wait_for(published.is_file, "the imported wallpaper manifest", seconds=20)
