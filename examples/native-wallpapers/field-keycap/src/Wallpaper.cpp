#include "Action75Wallpaper.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <stdexcept>
#include <wrl/client.h>
#ifdef FIELD_THEME
#include "FieldEffect.h"
#else
#include "KeycapEffect.h"
#endif

namespace {
using namespace action75;
using Json = nlohmann::json;

struct Wallpaper {
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  bool paused = false;
#ifdef FIELD_THEME
  FieldEffect fx;
  Json palettes;
#else
  KeycapEffect fx;
#endif
  explicit Wallpaper(const A75Create &c) : context(c.context) {
    const auto folder = std::filesystem::path(c.pack_dir);
    const auto layout = folder / L"layout.json";
    std::string error;
#ifdef FIELD_THEME
    std::ifstream input(folder / L"settings.json");
    const auto schema = Json::parse(input);
    for (const auto &group : schema.at("groups"))
      for (const auto &control : group.at("controls"))
        if (control.at("id") == "palette") palettes = control.at("palettes");
    if (!fx.load(c.device,layout,FIELD_THEME,error)) throw std::runtime_error(error);
#else
    if (!fx.load(c.device,layout,error)) throw std::runtime_error(error);
#endif
    configure(c.settings_json);
  }
  void configure(const char *data) {
    const auto values = Json::parse(data && *data ? data : "{}");
    PackAppearance appearance;
    appearance.intensity = values.value("intensity", 1.f);
#ifdef FIELD_THEME
    appearance.field = values;
    appearance.brightness = values.value("brightness", 1.f);
    appearance.scale = values.value("size", 1.f);
    appearance.speed = values.value("speed", 1.f);
    auto &colors = appearance.resolvedPaletteColors;
    if (values.contains("palette")) {
      const auto &palette = values.at("palette");
      colors = palette.at("colors").get<std::vector<EffectRgb>>();
      for (const auto &preset : palettes) {
        if (preset.at("id") == palette.at("preset")) {
          colors = preset.at("colors").get<std::vector<EffectRgb>>();
          break;
        }
      }
    }
#else
    appearance.keycap = values;
    appearance.brightness = values.value("brightness", 1.1f);
    appearance.color = values.value("color", EffectRgb{64,217,255});
    appearance.keycapOutlineColor = values.value("outlineColor", EffectRgb{62,140,166});
#endif
    fx.configure(appearance);
  }
  void event(const A75Event &e) {
    switch (e.type) {
    case A75_KEY_DOWN:
      if (!paused) {
#ifdef FIELD_THEME
        fx.push(e.seconds,e.x,e.y);
#else
        if (e.code) fx.push(e.code,e.seconds);
#endif
      }
      break;
    case A75_SETTINGS: configure(e.settings_json); break;
    case A75_INPUT_RESET: fx.clear(); break;
    case A75_PAUSE: paused=true; break;
    case A75_RESUME: paused=false; break;
    }
  }
  int32_t render(const A75Frame &f) {
    context->OMSetRenderTargets(1,&f.target,nullptr);
    const D3D11_VIEWPORT vp{0,0,float(f.width),float(f.height),0,1};
    context->RSSetViewports(1,&vp);
    std::string error;
#ifdef FIELD_THEME
    if (!fx.draw(context.Get(),f.width,f.height,f.seconds,paused,error)) return -2;
    return paused ? -1 : 0;
#else
    context->ClearRenderTargetView(f.target,fx.clearColor().data());
    if (!fx.draw(context.Get(),f.width,f.height,f.seconds,error)) return -2;
    return !paused && (fx.count() || fx.animated()) ? 0 : -1;
#endif
  }
};
void *__cdecl create(const A75Create *c) noexcept {
  try {
    if (!c || c->size<sizeof(*c) || !c->device || !c->context || !c->pack_dir) return nullptr;
    return new Wallpaper(*c);
  } catch (...) { return nullptr; }
}
void __cdecl event(void *p,const A75Event *e) noexcept {
  try { if (p && e && e->size>=sizeof(*e)) static_cast<Wallpaper*>(p)->event(*e); }
  catch (...) {}
}
int32_t __cdecl render(void *p,const A75Frame *f) noexcept {
  try {
    if (!p || !f || f->size<sizeof(*f) || !f->target) return -2;
    return static_cast<Wallpaper*>(p)->render(*f);
  } catch (...) { return -2; }
}
void __cdecl destroy(void *p) noexcept { delete static_cast<Wallpaper*>(p); }
const A75Wallpaper api{sizeof(A75Wallpaper),ACTION75_WALLPAPER_ABI,create,event,render,destroy,nullptr};
}
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  return abi==ACTION75_WALLPAPER_ABI ? &api : nullptr;
}
