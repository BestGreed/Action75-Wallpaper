#include "Action75Wallpaper12.h"
#if defined(A75_WATER)
#include "WaterSurface.h"
using Scene = surface::WaterSurface;
#elif defined(A75_FIREWORKS)
#include "FireworksScene.h"
using Scene = surface::FireworksScene;
#elif defined(A75_GLASS)
#include "GlassShatter.h"
using Scene = surface::GlassShatter;
#else
#error Select a surface scene
#endif

#include <filesystem>

namespace {
struct Wallpaper {
  surface::Settings settings;
  Scene scene;
  bool paused = false, valid = true;

  explicit Wallpaper(const A75Create12 &host)
      : settings(std::filesystem::path(host.pack_dir) / L"settings.json"),
        scene(host.device, host.width, host.height) {
    configure(host.settings_json);
  }

  void configure(const char *json) {
    settings.update(json);
    scene.configure(settings);
    valid = true;
  }
};

void *__cdecl create(const A75Create12 *host) noexcept {
  if (!host || host->size < sizeof(A75Create12) || !host->device || !host->pack_dir ||
      !host->width || !host->height)
    return nullptr;
  try {
    return new Wallpaper(*host);
  } catch (...) {
    return nullptr;
  }
}

void __cdecl onEvent(void *instance, const A75Event *event) noexcept {
  if (!instance || !event || event->size < sizeof(A75Event))
    return;
  auto &wallpaper = *static_cast<Wallpaper *>(instance);
  switch (event->type) {
  case A75_SETTINGS:
    try {
      wallpaper.configure(event->settings_json);
    } catch (...) {
      wallpaper.valid = false;
    }
    break;
  case A75_INPUT_RESET: wallpaper.scene.clear(); break;
  case A75_PAUSE: wallpaper.paused = true; break;
  case A75_RESUME: wallpaper.paused = false; break;
  case A75_KEY_DOWN:
#if !defined(A75_GLASS)
  case A75_POINTER_DOWN:
#endif
    if (wallpaper.paused || !wallpaper.valid)
      break;
#if defined(A75_FIREWORKS)
    if (event->type == A75_KEY_DOWN && event->code && *event->code && !wallpaper.scene.readyForKeyBurst())
      break;
#endif
    wallpaper.scene.push(event->seconds, event->x, event->y);
    break;
  }
}

int32_t __cdecl render(void *instance, const A75Frame12 *frame) noexcept {
  if (!instance || !frame || frame->size < sizeof(A75Frame12) || !frame->commands ||
      !frame->width || !frame->height || !frame->target)
    return -2;
  auto &wallpaper = *static_cast<Wallpaper *>(instance);
  if (!wallpaper.valid)
    return -2;
  try {
    wallpaper.scene.draw(*frame, wallpaper.paused);
    return !wallpaper.paused && wallpaper.scene.hasMotion() ? 0 : -1;
  } catch (...) {
    return -2;
  }
}

void __cdecl destroy(void *instance) noexcept { delete static_cast<Wallpaper *>(instance); }

#if defined(A75_GLASS)
constexpr auto hitTest = nullptr;
#else
int32_t __cdecl hitTest(void *, float, float, uint32_t, uint32_t) noexcept { return 1; }
#endif
} // namespace

extern "C" const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi) {
  static const A75Wallpaper12 api{sizeof(api), ACTION75_WALLPAPER_ABI12, create, onEvent, render,
                                  destroy, hitTest};
  return abi == ACTION75_WALLPAPER_ABI12 ? &api : nullptr;
}
