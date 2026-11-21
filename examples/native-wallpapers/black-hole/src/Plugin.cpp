#include "BlackHoleDx12.h"
#include "BlackHoleProfile.h"
#include <json.hpp>
#include <cmath>
#include <optional>

namespace {
using namespace action75;
using Json = nlohmann::json;
using Point = BlackHoleDx12::Position;

struct Drag { Point start, center; };

struct Instance {
  BlackHoleDx12 scene;
  std::optional<Drag> drag;
  Point profileCenter{.58f,.46f};
  void *host;
  decltype(A75Create12::commit_settings) commit;
  bool paused = false;

  explicit Instance(const A75Create12 &create)
      : scene(create.device, create.pack_dir), host(create.host), commit(create.commit_settings) {
    configure(create.settings_json);
  }

  void configure(const char *values) {
    const Json profile = values && *values ? Json::parse(values) : Json::object();
    const auto settings = BlackHoleProfile::fromSettings(profile);
    profileCenter = settings.blackHole.center;
    scene.configure(settings.blackHole, settings.starSize, settings.starBrightness, !drag);
  }

  void move(Point cursor) {
    scene.setCenter({drag->center[0] + cursor[0] - drag->start[0],
                     drag->center[1] + cursor[1] - drag->start[1]});
  }

  void cancel() {
    if (drag) scene.setCenter(profileCenter);
    drag.reset();
  }

  void event(const A75Event &e) {
    Point point{e.x, e.y};
    switch (e.type) {
    case A75_KEY_DOWN:
      if (!paused) scene.push(e.seconds, point, e.width, e.height);
      break;
    case A75_POINTER_DOWN:
      if (!paused && scene.covers(point,e.width,e.height)) drag = Drag{point,scene.center()};
      break;
    case A75_POINTER_MOVE:
      if (drag) move(point);
      break;
    case A75_POINTER_UP:
      if (drag) {
        move(point);
        const auto center = scene.center();
        const auto start = drag->center;
        drag.reset();
        if (commit && center != start) {
          auto pct = [](float v) { return std::round(v * 1000.0) / 10.0; };
          const auto patch = Json{{"centerX",pct(center[0])},{"centerY",pct(center[1])}}.dump();
          commit(host,patch.c_str());
        }
      }
      break;
    case A75_SETTINGS: configure(e.settings_json); break;
    case A75_PAUSE: cancel(); paused = true; break;
    case A75_RESUME: paused = false; break;
    case A75_INPUT_RESET: cancel(); scene.clear(); break;
    }
  }
};

void * __cdecl createInstance(const A75Create12 *c) noexcept {
  try {
    if (!c || c->size < sizeof(*c) || !c->device || !c->pack_dir) return nullptr;
    return new Instance(*c);
  } catch (...) { return nullptr; }
}
void __cdecl onEvent(void *ptr,const A75Event *e) noexcept {
  try { if (ptr && e && e->size >= sizeof(*e)) static_cast<Instance *>(ptr)->event(*e); }
  catch (...) { /* Keep the last complete appearance. */ }
}
int32_t __cdecl render(void *ptr, const A75Frame12 *f) noexcept {
  try {
    if (!ptr || !f || f->size < sizeof(*f) || !f->target || !f->commands || !f->rtv.ptr ||
        !f->width || !f->height) return -2;
    auto &s = *static_cast<Instance *>(ptr);
    s.scene.draw(*f, s.paused);
    return !s.paused && s.scene.animates() ? 0 : -1;
  } catch (...) { return -2; }
}
void __cdecl destroy(void *ptr) noexcept { delete static_cast<Instance *>(ptr); }
int32_t __cdecl hitTest(void *ptr,float x,float y,uint32_t w,uint32_t h) noexcept {
  if (!ptr || !w || !h) return 0;
  const auto &s = *static_cast<const Instance *>(ptr);
  return !s.paused && (s.drag || s.scene.covers({x,y},w,h));
}
const A75Wallpaper12 api{sizeof(A75Wallpaper12),ACTION75_WALLPAPER_ABI12,createInstance,onEvent,render,destroy,hitTest};
}

extern "C" const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi) {
  return abi == ACTION75_WALLPAPER_ABI12 ? &api : nullptr;
}
