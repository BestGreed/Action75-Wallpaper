#pragma once
#include "Action75Wallpaper.h"
#include <filesystem>
#include <memory>
#include <string>

namespace action75 {
class NativePluginSession {
public:
  NativePluginSession();
  ~NativePluginSession();
  NativePluginSession(const NativePluginSession &) = delete;
  NativePluginSession &operator=(const NativePluginSession &) = delete;
  bool start(ID3D11Device *, const std::filesystem::path &packRoot,
             const std::filesystem::path &module, unsigned width, unsigned height,
             const std::string &settingsJson, std::string &error);
  bool event(const A75Event &, std::string &error);
  std::string takeSettingsPatch();
  bool hitTest(float x, float y, bool &hit, std::string &error);
  bool render(ID3D11Texture2D *target, double monotonicSeconds, int32_t &nextDelayMs, std::string &error);
  bool active() const noexcept;
  DWORD workerPid() const noexcept;
    HANDLE exitHandle() const noexcept;
  void stop() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace action75
