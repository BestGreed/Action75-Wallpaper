#include "Action75Wallpaper12.h"

#ifdef FIELD_THEME
#include "FieldScene.h"
using Scene = wallpaper::FieldScene;
#else
#include "KeycapScene.h"
using Scene = wallpaper::KeycapScene;
#endif

namespace {
using wallpaper::Json;

struct Wallpaper {
  Scene scene;
  bool paused = false;

  explicit Wallpaper(const A75Create12 &host) : scene(host) { configure(host.settings_json); }

  void configure(const char *settings) {
    scene.configure(Json::parse(settings && *settings ? settings : "{}"));
  }

  void event(const A75Event &event) {
    switch (event.type) {
    case A75_KEY_DOWN:
      if (!paused)
        scene.press(event);
      break;
    case A75_SETTINGS: configure(event.settings_json); break;
    case A75_INPUT_RESET: scene.clear(); break;
    case A75_PAUSE: paused = true; break;
    case A75_RESUME: paused = false; break;
    }
  }
};

void *__cdecl create(const A75Create12 *host) noexcept {
  try {
    if (!host || host->size < sizeof(*host) || !host->device || !host->pack_dir)
      return nullptr;
    return new Wallpaper(*host);
  } catch (...) {
    return nullptr;
  }
}

void __cdecl event(void *instance, const A75Event *message) noexcept {
  try {
    if (instance && message && message->size >= sizeof(*message))
      static_cast<Wallpaper *>(instance)->event(*message);
  } catch (...) {
  }
}

int32_t __cdecl render(void *instance, const A75Frame12 *frame) noexcept {
  try {
    if (!instance || !frame || frame->size < sizeof(*frame) || !frame->target || !frame->commands ||
        !frame->width || !frame->height)
      return -2;
    auto &wallpaper = *static_cast<Wallpaper *>(instance);
    return wallpaper.scene.render(*frame, wallpaper.paused);
  } catch (...) {
    return -2;
  }
}

void __cdecl destroy(void *instance) noexcept { delete static_cast<Wallpaper *>(instance); }
} // namespace

extern "C" const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi) {
  static const A75Wallpaper12 api{sizeof(api), ACTION75_WALLPAPER_ABI12, create, event, render,
                                  destroy, nullptr};
  return abi == api.abi ? &api : nullptr;
}
