#pragma once
#include <windows.h>
#include <cwchar>
#include <string>

// Explorer has two desktop layouts. In both cases leave its existing windows untouched.
namespace action75 {
struct ShellLayer {
  HWND parent = nullptr, after = nullptr;
};
inline DWORD shellWindowPid(HWND w) {
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  return pid;
}
inline bool shellClass(HWND w, const wchar_t *name) {
  wchar_t cls[64]{};
  return GetClassNameW(w, cls, 64) && std::wcscmp(cls, name) == 0;
}
inline ShellLayer findShellLayer(HWND progman) {
  if (!progman)
    return {};
  const auto pid = shellWindowPid(progman);
  const auto icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
  if (icons && shellWindowPid(icons) == pid) {
    for (HWND h = GetWindow(icons, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT))
      if (shellClass(h, L"WorkerW") && shellWindowPid(h) == pid)
        return {progman, icons};
  }
  for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
    if (shellWindowPid(h) != pid || (h != progman && !shellClass(h, L"WorkerW")))
      continue;
    if (!FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr))
      continue;
    for (auto w = GetWindow(h, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT))
      if (shellClass(w, L"WorkerW") && shellWindowPid(w) == pid &&
          !FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr))
        return {w, HWND_TOP};
  }
  return {};
}

class DesktopWallpaperLayer {
public:
  static constexpr UINT Lost = WM_APP + 54, Refresh = WM_APP + 55;
  ~DesktopWallpaperLayer() { reset(); }
  HWND get() const { return window_; }
  template <class Bind>
  bool place(HWND controller, HWND progman, bool requestLayer, std::string &error, Bind bind) {
    auto layer = findShellLayer(progman);
    if (!layer.parent && progman && requestLayer) {
      DWORD_PTR ignored{};
      SendMessageTimeoutW(progman, 0x052c, 0xd, 1, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &ignored);
      layer = findShellLayer(progman);
    }
    if (!layer.parent || !IsWindowVisible(layer.parent)) {
      error = "无法找到桌面图标下方的壁纸层，请等待 Windows 资源管理器就绪后重试。";
      return false;
    }
    RECT bounds{};
    if (!GetWindowRect(controller, &bounds))
      return failed(error);
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(HWND_DESKTOP, layer.parent, reinterpret_cast<POINT *>(&bounds), 2) &&
        GetLastError())
      return failed(error);
    RECT area{}, overlap{};
    if (!GetClientRect(layer.parent, &area) || !IntersectRect(&overlap, &area, &bounds) ||
        !EqualRect(&overlap, &bounds))
      return failed(error);
    const bool replace = !window_ || GetParent(window_) != layer.parent;
    HWND target = window_;
    if (replace) {
      WNDCLASSW cls{};
      cls.lpfnWndProc = procedure;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpszClassName = L"Action75DesktopSurface";
      if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return failed(error);
      target = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
                               cls.lpszClassName, L"", WS_CHILD, 0, 0, 0, 0, layer.parent, nullptr,
                               cls.hInstance, nullptr);
      if (!target)
        return failed(error);
    }
    RECT screen{}, wanted{};
    const bool moved = !GetWindowRect(target, &screen) || !GetWindowRect(controller, &wanted) ||
                       !EqualRect(&screen, &wanted);
    if ((moved || GetWindow(target, GW_HWNDPREV) != layer.after) &&
        !SetWindowPos(target, layer.after, bounds.left, bounds.top, bounds.right - bounds.left,
                      bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
      if (replace)
        DestroyWindow(target);
      return failed(error);
    }
    if (!bind(target)) {
      if (replace)
        DestroyWindow(target);
      return false;
    }
    if (replace) {
      reset();
      window_ = target;
      owner_ = controller;
      parent_ = layer.parent;
      SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controller));
      listener_ = this;
      destroyHook_ = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
                                     shellReordered, shellWindowPid(parent_), 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
      reorderHook_ =
          SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, shellReordered, 0, 0,
                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    return true;
  }
  void show(bool visible) {
    if (window_ && shellWindowPid(window_) == GetCurrentProcessId() &&
        bool(GetWindowLongPtrW(window_, GWL_STYLE) & WS_VISIBLE) != visible)
      ShowWindow(window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
  }
  void reset() {
    if (reorderHook_)
      UnhookWinEvent(reorderHook_);
    if (destroyHook_)
      UnhookWinEvent(destroyHook_);
    reorderHook_ = destroyHook_ = nullptr;
    if (listener_ == this)
      listener_ = nullptr;
    owner_ = parent_ = nullptr;
    if (window_ && shellWindowPid(window_) == GetCurrentProcessId() &&
        shellClass(window_, L"Action75DesktopSurface")) {
      SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      DestroyWindow(window_);
    }
    window_ = nullptr;
  }

private:
  HWND window_ = nullptr, owner_ = nullptr, parent_ = nullptr;
  HWINEVENTHOOK reorderHook_ = nullptr, destroyHook_ = nullptr;
  inline static DesktopWallpaperLayer *listener_ = nullptr; // One output window per player.
  static void CALLBACK shellReordered(HWINEVENTHOOK hook, DWORD, HWND window, LONG object, LONG,
                                      DWORD, DWORD) {
    auto *self = listener_;
    if (self && (self->reorderHook_ == hook || self->destroyHook_ == hook) && window &&
        (object == OBJID_WINDOW || object == OBJID_CLIENT) &&
        (window == self->parent_ || GetParent(window) == self->parent_))
      PostMessageW(self->owner_, Refresh, 0, 0);
  }
  static bool failed(std::string &error) {
    error = "无法在所选屏幕上建立透明桌面窗口。";
    return false;
  }
  static LRESULT CALLBACK procedure(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST)
      return HTTRANSPARENT;
    if (m == WM_ERASEBKGND)
      return 1;
    if (m == WM_MOUSEACTIVATE)
      return MA_NOACTIVATE;
    if (m == WM_NCDESTROY)
      if (auto owner = reinterpret_cast<HWND>(GetWindowLongPtrW(h, GWLP_USERDATA)))
        PostMessageW(owner, Lost, reinterpret_cast<WPARAM>(h), 0);
    return DefWindowProcW(h, m, w, l);
  }
};
} // namespace action75
