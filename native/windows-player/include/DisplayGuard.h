#pragma once
#include <memory>
#include <string>
#include <windows.h>

namespace action75 {
struct DisplayIdentity;
// Constructor and unused stop() perform no desktop operations. configure() is
// non-blocking apart from a short state lock/first-use thread creation. All
// clip, hook and window operations run on one private message-pump thread.
class DisplayGuard final {
public:
  DisplayGuard();
  ~DisplayGuard();
  DisplayGuard(const DisplayGuard&)=delete;
  DisplayGuard& operator=(const DisplayGuard&)=delete;
  void configure(HWND wallpaper,const std::wstring& targetDevice,const DisplayIdentity& identity,
                 bool enabled,bool suspended);
  void stop() noexcept;
  bool active() const;
  std::string mode() const;
  std::string error() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
