#pragma once

#include <optional>
#include <windows.h>

namespace action75 {
class KeyboardTaskbar final {
public:
  KeyboardTaskbar() = default;
  ~KeyboardTaskbar();
  KeyboardTaskbar(const KeyboardTaskbar &) = delete;
  KeyboardTaskbar &operator=(const KeyboardTaskbar &) = delete;

  bool hide(HWND wallpaper, const RECT &display);
  void restore() noexcept;
  static std::optional<int> runHelper();

private:
  HANDLE stop_ = nullptr, helper_ = nullptr;
  RECT display_{};
};
}
