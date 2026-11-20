#include "Action75Wallpaper.h"
#include "PackSettings.h"
#if defined(A75_WATER)
#include "WaterSurface.h"
using Scene = action75::WaterSurface;
#elif defined(A75_FIREWORKS)
#include "FireworksScene.h"
using Scene = action75::FireworksScene;
#else
#include "GlassShatter.h"
using Scene = action75::GlassShatter;
#endif
#include <filesystem>
#include <memory>

namespace {
struct Wallpaper {
  Scene scene;
  action75::PackSettingsCandidate controls;
  ID3D11DeviceContext *context;
  bool paused = false, enabled = true, valid = true;

  explicit Wallpaper(const A75Create &host)
      : controls(action75::PackSettingsStore::packageControls(
            std::filesystem::path(host.pack_dir) / L"settings.json")), context(host.context) {}

  void configure(const char *json) {
    controls.values = nlohmann::json::parse(json);
    const auto appearance = action75::PackSettingsStore::evaluate(controls);
    enabled = appearance.intensity != 0;
    scene.configure(appearance);
  }
};

void *__cdecl create(const A75Create *host) noexcept {
  try {
    auto wallpaper = std::make_unique<Wallpaper>(*host);
    std::string error;
#if defined(A75_WATER)
    if (!wallpaper->scene.load(host->device, host->context, host->width, host->height, error))
#elif defined(A75_FIREWORKS)
    if (!wallpaper->scene.load(host->device, host->width, host->height, error))
#else
    if (!wallpaper->scene.load(host->device, error))
#endif
      return nullptr;
    wallpaper->configure(host->settings_json);
    return wallpaper.release();
  } catch (...) {
    return nullptr;
  }
}

void __cdecl event(void *instance, const A75Event *e) noexcept {
  auto &w = *static_cast<Wallpaper *>(instance);
  switch (e->type) {
  case A75_SETTINGS:
    try { w.configure(e->settings_json); } catch (...) { w.valid = false; }
    break;
  case A75_INPUT_RESET: w.scene.clear(); break;
  case A75_PAUSE: w.paused = true; break;
  case A75_RESUME: w.paused = false; break;
  case A75_KEY_DOWN:
#if !defined(A75_GLASS)
  case A75_POINTER_DOWN:
#endif
    if (w.paused || !w.enabled) break;
#if defined(A75_WATER)
    w.scene.push({e->x, e->y});
#else
#if defined(A75_FIREWORKS)
    if (e->type == A75_KEY_DOWN && e->code && *e->code && !w.scene.readyForKeyBurst()) break;
#endif
    w.scene.push(e->seconds, e->x, e->y);
#endif
    break;
  }
}

int32_t __cdecl render(void *instance, const A75Frame *frame) noexcept {
  auto &w = *static_cast<Wallpaper *>(instance);
  if (!w.valid) return -2;
  D3D11_VIEWPORT view{0, 0, static_cast<float>(frame->width), static_cast<float>(frame->height), 0, 1};
  w.context->OMSetRenderTargets(1, &frame->target, nullptr);
  w.context->RSSetViewports(1, &view);
  std::string error;
  try {
#if defined(A75_WATER)
    if (!w.scene.draw(w.context, frame->width, frame->height, frame->seconds, w.paused, error)) return -2;
    return !w.paused && w.scene.hasMotion() ? 0 : -1;
#else
    if (!w.scene.draw(w.context, frame->width, frame->height, frame->seconds, error)) return -2;
    return !w.paused && w.scene.count() ? 0 : -1;
#endif
  } catch (...) { return -2; }
}

void __cdecl destroy(void *instance) { delete static_cast<Wallpaper *>(instance); }

#if defined(A75_GLASS)
constexpr auto hit = nullptr;
#else
int32_t __cdecl hit(void *, float, float, uint32_t, uint32_t) { return 1; }
#endif
}
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  static const A75Wallpaper api{sizeof(api), ACTION75_WALLPAPER_ABI, create, event, render, destroy, hit};
  return abi == ACTION75_WALLPAPER_ABI ? &api : nullptr;
}
