#include "Wallpaper.h"
#include "SpriteBurst.h"
namespace atlas_packs {
struct Atlas {
  static constexpr bool canvas = false;
  SpriteBurst burst;
  void load(const fs::path &dir, const json &recipe, NativeRenderer &renderer) {
    SpriteBurst::Params params{recipe.at("columns"), recipe.at("rows"), recipe.at("duration"),
                               recipe.at("diameter"),
                               recipe.value("composition", "add") == "multiply"};
    std::string error;
    if (!burst.load(renderer.device(), dir / fs::u8path(recipe.at("effect").get<std::string>()),
                    params, error, true))
      throw std::runtime_error(error);
  }
  void configure(const PackAppearance &a) {
    burst.setBrightness(a.brightness);
    burst.setScale(a.scale);
    burst.setIntensity(a.intensity);
    burst.setColor(a.color);
    burst.setDuration(a.spriteDuration);
    burst.setSpeed(a.speed);
    burst.setColorGrade(a.saturation, a.hue, a.resolvedPaletteColors, a.paletteOverride);
  }
  void clear() { burst.clear(); }
  void key(const A75Event &e, NativeRenderer &) { burst.push(e.seconds, e.x, e.y); }
  int draw(double now, NativeRenderer &r, bool paused) {
    std::string error;
    if (!paused && !burst.draw(r.context(), r.width(), r.height(), now, error))
      return -2;
    return !paused && burst.count() ? 0 : -1;
  }
};
} // namespace atlas_packs
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  return atlas_packs::api<atlas_packs::Atlas>(abi);
}
