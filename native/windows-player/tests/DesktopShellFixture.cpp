#include <windows.h>
#include <cwchar>
#include <string_view>

// Minimal Explorer hierarchy for the player integration test. Never run on an interactive desktop.
int wmain(int argc, wchar_t **argv) {
  wchar_t name[128]{};
  if (argc != 2 ||
      !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, name,
                                 sizeof(name), nullptr) ||
      std::wstring_view(name).find(L"Action75MemQA-") != 0)
    return 1;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (std::wstring_view(argv[1]) == L"foreign") {
    const auto progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
      return 4;
    const auto icons = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    const auto folder = FindWindowW(L"WorkerW", L"folder");
    const auto host = icons ? progman : FindWindowExW(nullptr, folder, L"WorkerW", L"background");
    if (!host)
      return 5;
    const auto overlay =
        CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"changed wallpaper", WS_CHILD | WS_VISIBLE, 0,
                        0, 800, 450, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!overlay || !SetWindowPos(overlay, icons ? icons : HWND_TOP, 0, 0, 0, 0,
                                  SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE))
      return 6;
    if (!SetPropW(overlay, L"Action75ReorderComplete", reinterpret_cast<HANDLE>(1)))
      return 7;
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
      DispatchMessageW(&msg);
    return 0;
  }
  auto instance = GetModuleHandleW(nullptr);
  for (auto cls : {L"Progman", L"WorkerW", L"SHELLDLL_DefView", L"Shell_TrayWnd",
                   L"Shell_SecondaryTrayWnd"}) {
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpszClassName = cls;
    wc.lpfnWndProc = DefWindowProcW;
    if (!RegisterClassW(&wc))
      return 2;
  }
  const bool modern = std::wstring_view(argv[1]) == L"modern";
  auto window = [&](const wchar_t *cls, const wchar_t *title, HWND parent) {
    return CreateWindowExW(WS_EX_NOACTIVATE, cls, title,
                           (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE, parent ? 0 : -100,
                           parent ? 0 : -50, 3200, 1800, parent, nullptr, instance, nullptr);
  };
  HWND progman = window(L"Progman", L"fixture", nullptr);
  HWND backdrop = window(L"WorkerW", L"background", modern ? progman : nullptr);
  HWND folder = modern ? progman : window(L"WorkerW", L"folder", nullptr);
  HWND icons = window(L"SHELLDLL_DefView", L"icons", folder);
  auto bar = [&](const wchar_t *cls, const wchar_t *title, int x) {
    return CreateWindowExW(WS_EX_NOACTIVATE, cls, title, WS_POPUP | WS_VISIBLE,
                           x, 330, 640, 30, nullptr, nullptr, instance, nullptr);
  };
  const auto primary = bar(L"Shell_TrayWnd", L"primary bar", -700);
  const auto selected = bar(L"Shell_SecondaryTrayWnd", L"keyboard bar", 0);
  const auto other = bar(L"Shell_SecondaryTrayWnd", L"other secondary bar", 800);
  if (!progman || !backdrop || !folder || !icons || !primary || !selected || !other ||
      !SetWindowPos(modern ? icons : folder, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE))
    return 3;
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    DispatchMessageW(&msg);
  return 0;
}
