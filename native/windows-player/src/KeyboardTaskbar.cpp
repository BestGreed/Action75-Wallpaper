#include "KeyboardTaskbar.h"

#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <shellapi.h>
#include <string>
#include <vector>

namespace action75 {
namespace {
constexpr wchar_t HelperArgument[] = L"--keyboard-taskbar-helper";
constexpr wchar_t SecondaryTray[] = L"Shell_SecondaryTrayWnd";

struct Handle {
  HANDLE value = nullptr;
  ~Handle() {
    if (value)
      CloseHandle(value);
  }
};

DWORD owner(HWND window) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid;
}

bool secondaryTray(HWND window, DWORD shell) {
  wchar_t name[64]{};
  return window && shell && owner(window) == shell &&
         GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
         std::wcscmp(name, SecondaryTray) == 0;
}

HWND taskbarOnDisplay(const RECT &display, DWORD shell, bool testDesktop) {
  APPBARDATA data{sizeof(data)};
  if (!testDesktop && (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE)) {
    // An auto-hidden taskbar may be parked across the boundary of its monitor.
    data.rc = display;
    for (const auto edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM}) {
      data.uEdge = edge;
      const auto window = reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data));
      if (secondaryTray(window, shell))
        return window;
    }
    return nullptr;
  }

  HWND window = nullptr;
  while ((window = FindWindowExW(nullptr, window, SecondaryTray, nullptr)) != nullptr) {
    if (owner(window) != shell)
      continue;
    if (!testDesktop && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) ==
                            MonitorFromRect(&display, MONITOR_DEFAULTTONULL))
      return window;
    RECT bounds{};
    if (testDesktop && GetWindowRect(window, &bounds) &&
        bounds.left < bounds.right && bounds.top < bounds.bottom &&
        bounds.left >= display.left && bounds.top >= display.top &&
        bounds.right <= display.right && bounds.bottom <= display.bottom)
      return window;
  }
  return nullptr;
}

struct HiddenTaskbar {
  HWND window = nullptr;
  DWORD shell = 0;
  void restore() {
    if (secondaryTray(window, shell))
      ShowWindowAsync(window, SW_SHOWNOACTIVATE);
    window = nullptr;
  }
  ~HiddenTaskbar() { restore(); }
};

// The parent handle becomes signalled even when the player is ended forcibly.
int watchTaskbar(HANDLE stop, HANDLE player, HWND wallpaper) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto parent = GetProcessId(player);
  RECT display{};
  if (!parent || owner(wallpaper) != parent || !GetWindowRect(wallpaper, &display))
    return 1;
  wchar_t desktop[128]{};
  const bool testDesktop =
      GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop,
                                sizeof(desktop), nullptr) &&
      std::wcsncmp(desktop, L"Action75MemQA-", 14) == 0;
  HANDLE waits[]{stop, player};
  HiddenTaskbar hidden;
  while (WaitForMultipleObjects(2, waits, FALSE, 0) == WAIT_TIMEOUT) {
    RECT current{};
    if (owner(wallpaper) != parent || !GetWindowRect(wallpaper, &current) ||
        !EqualRect(&display, &current))
      break;
    if (!testDesktop) {
      MONITORINFO monitor{sizeof(monitor)};
      const auto handle = MonitorFromRect(&display, MONITOR_DEFAULTTONULL);
      if (!handle || !GetMonitorInfoW(handle, &monitor) || (monitor.dwFlags & MONITORINFOF_PRIMARY) ||
          !EqualRect(&display, &monitor.rcMonitor))
        break;
    }
    const auto shell = owner(FindWindowW(L"Shell_TrayWnd", nullptr));
    if (hidden.window && (hidden.shell != shell || !secondaryTray(hidden.window, shell)))
      hidden.restore();
    if (!hidden.window && shell) {
      const auto candidate = taskbarOnDisplay(display, shell, testDesktop);
      if (candidate && IsWindowVisible(candidate) &&
          WaitForMultipleObjects(2, waits, FALSE, 0) == WAIT_TIMEOUT &&
          ShowWindowAsync(candidate, SW_HIDE)) {
        hidden.window = candidate;
        hidden.shell = shell;
      }
    } else if (hidden.window && IsWindowVisible(hidden.window)) {
      ShowWindowAsync(hidden.window, SW_HIDE);
    }
    if (WaitForMultipleObjects(2, waits, FALSE, hidden.window ? 250 : 1000) != WAIT_TIMEOUT)
      break;
  }
  return 0;
}

uintptr_t parseHandle(const wchar_t *value) {
  wchar_t *end = nullptr;
  const auto result = std::wcstoull(value, &end, 10);
  return end != value && !*end && result <= UINTPTR_MAX ? static_cast<uintptr_t>(result) : 0;
}
}

KeyboardTaskbar::~KeyboardTaskbar() { restore(); }

void KeyboardTaskbar::restore() noexcept {
  if (stop_) {
    SetEvent(stop_);
    CloseHandle(stop_);
    stop_ = nullptr;
  }
  if (helper_) {
    CloseHandle(helper_);
    helper_ = nullptr;
  }
}

bool KeyboardTaskbar::hide(HWND wallpaper, const RECT &display) {
  if (helper_ && EqualRect(&display_, &display) &&
      WaitForSingleObject(helper_, 0) == WAIT_TIMEOUT)
    return true;
  restore();
  wchar_t executable[32768]{};
  const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  if (!length || length == std::size(executable))
    return false;
  SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
  Handle stop{CreateEventW(&inherited, TRUE, FALSE, nullptr)}, player;
  const auto self = GetCurrentProcess();
  if (!stop.value || !DuplicateHandle(self, self, self, &player.value,
                                     SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
    return false;
  auto command = L"\"" + std::wstring(executable, length) + L"\" " + HelperArgument + L" " +
                 std::to_wstring(reinterpret_cast<uintptr_t>(stop.value)) + L" " +
                 std::to_wstring(reinterpret_cast<uintptr_t>(player.value)) + L" " +
                 std::to_wstring(reinterpret_cast<uintptr_t>(wallpaper));
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  if (!bytes)
    return false;
  std::vector<unsigned char> storage(bytes);
  const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
    return false;
  const auto destroy = [](void *list) {
    DeleteProcThreadAttributeList(static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list));
  };
  const std::unique_ptr<void, decltype(destroy)> cleanup(attributes, destroy);
  HANDLE handles[]{stop.value, player.value};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                 sizeof(handles), nullptr, nullptr))
    return false;
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                      &startup.StartupInfo, &child))
    return false;
  CloseHandle(child.hThread);
  helper_ = child.hProcess;
  stop_ = stop.value;
  stop.value = nullptr;
  display_ = display;
  return true;
}

std::optional<int> KeyboardTaskbar::runHelper() {
  int argc = 0;
  const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const std::unique_ptr<void, decltype(&LocalFree)> cleanup(argv, LocalFree);
  if (!argv || argc < 2 || std::wcscmp(argv[1], HelperArgument) != 0)
    return {};
  if (argc != 5)
    return 1;
  Handle stop{reinterpret_cast<HANDLE>(parseHandle(argv[2]))};
  Handle player{reinterpret_cast<HANDLE>(parseHandle(argv[3]))};
  const auto wallpaper = reinterpret_cast<HWND>(parseHandle(argv[4]));
  return watchTaskbar(stop.value, player.value, wallpaper);
}
}
