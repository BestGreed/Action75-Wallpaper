#pragma once
#include "Action75Wallpaper.h"
#include "NativeRenderer.h"
#include "PackSettings.h"
#include <fstream>
#include <stdexcept>

// Each DLL links only its own effect layer. The background uses the same color
// transform as the original artwork, including the baked canvas for Paintball.
namespace atlas_packs {
using namespace action75;
using json = nlohmann::json;
namespace fs = std::filesystem;

template <class Layer> class Wallpaper {
  NativeRenderer output;
  Layer layer;
  PackSettingsCandidate controls;
  bool paused = false, failed = false;
  float intensity = 1;

  void configure(const char *values) {
    controls.values = json::parse(values);
    const auto appearance = PackSettingsStore::evaluate(controls);
    output.setBackgroundAppearance(appearance.backgroundBrightness, appearance.backgroundSaturation,
                                   appearance.backgroundHue, appearance.backgroundColor,
                                   appearance.backgroundStyle == 1);
    layer.configure(appearance);
    intensity = appearance.intensity;
    if (intensity == 0)
      layer.clear();
  }

public:
  explicit Wallpaper(const A75Create &c) {
    const fs::path dir(c.pack_dir);
    std::ifstream input(dir / L"scene.json");
    const auto recipe = json::parse(input);
    controls = PackSettingsStore::packageControls(dir / L"settings.json");
    std::string error;
    if (!output.initializeForPlugin(c.device, c.context, c.width, c.height, error))
      throw std::runtime_error(error);
    if constexpr (Layer::canvas) {
      if (!output.loadCanvasBackground(
              dir / fs::u8path(recipe.at("canvasEffect").get<std::string>()), error))
        throw std::runtime_error(error);
    } else if (!output.loadBackground(dir / fs::u8path(recipe.at("background").get<std::string>()),
                                      error)) {
      throw std::runtime_error(error);
    }
    layer.load(dir, recipe, output);
    configure(c.settings_json);
  }
  void event(const A75Event &e) noexcept {
    try {
      switch (e.type) {
      case A75_SETTINGS:
        configure(e.settings_json);
        break;
      case A75_KEY_DOWN:
        if (!paused && intensity != 0)
          layer.key(e, output);
        break;
      case A75_INPUT_RESET:
        layer.clear();
        break;
      case A75_PAUSE:
        paused = true;
        break;
      case A75_RESUME:
        paused = false;
        break;
      }
    } catch (...) {
      failed = true;
    }
  }
  int32_t frame(const A75Frame &f) {
    if (failed || !output.setPluginTarget(f.target))
      return -2;
    output.beginFrame();
    return layer.draw(f.seconds, output, paused);
  }
};

template <class Layer> const A75Wallpaper *api(uint32_t version) {
  using State = Wallpaper<Layer>;
  static const A75Wallpaper table{
      sizeof(A75Wallpaper),
      ACTION75_WALLPAPER_ABI,
      [](const A75Create *c) -> void * {
        try {
          return new State(*c);
        } catch (...) {
          return nullptr;
        }
      },
      [](void *p, const A75Event *e) { static_cast<State *>(p)->event(*e); },
      [](void *p, const A75Frame *f) -> int32_t {
        try {
          return static_cast<State *>(p)->frame(*f);
        } catch (...) {
          return -2;
        }
      },
      [](void *p) { delete static_cast<State *>(p); },
      nullptr};
  return version == ACTION75_WALLPAPER_ABI ? &table : nullptr;
}
} // namespace atlas_packs
