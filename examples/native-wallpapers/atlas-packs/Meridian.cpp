#include "Wallpaper.h"
#include "ProceduralBurst.h"
namespace atlas_packs {
struct Meridian {
  static constexpr bool canvas = false;
  ProceduralBurst arcs;
  void load(const fs::path &, const json &recipe, NativeRenderer &r) {
    std::string error;
    if (!arcs.load(r.device(), recipe.at("proceduralStyle"), recipe.at("diameter"),
                   recipe.at("duration"), error))
      throw std::runtime_error(error);
  }
  void configure(const PackAppearance &a) { arcs.configure(a); }
  void clear() { arcs.clear(); }
  void key(const A75Event &e, NativeRenderer &) { arcs.push(e.seconds, e.x, e.y); }
  int draw(double now, NativeRenderer &r, bool paused) {
    std::string error;
    if (!arcs.draw(r.context(), r.width(), r.height(), now, paused, error))
      return -2;
    return !paused && arcs.count() ? 0 : -1;
  }
};
} // namespace atlas_packs
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  return atlas_packs::api<atlas_packs::Meridian>(abi);
}
