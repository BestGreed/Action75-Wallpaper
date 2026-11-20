#include "Wallpaper.h"
#include "EffekseerBackend.h"

namespace atlas_packs {
struct Paintball {
  static constexpr bool canvas = true;
  EffekseerBackend paint;
  std::vector<int> pigments;
  float scale = 1, speed = 1;
  double lastFrame = 0;
  uint64_t randomState = (GetTickCount64() << 16) ^ GetCurrentProcessId() ^ 0x9e3779b97f4a7c15ull;
  void load(const fs::path &dir, const json &recipe, NativeRenderer &r) {
    // The nine authored trees contain at most eleven instances per hit, root included.
    // 64 overlapping hits need 704 slots; allow headroom for generation chunks and recycling.
    if (!paint.Initialize(r.device(), r.context(), 2048))
      throw std::runtime_error(paint.LastError());
    paint.Resize(r.width(), r.height());
    scale = recipe.value("scale", 1.f);
    for (const auto &name : recipe.at("effects")) {
      // Paintball's material renderer and packaged shaders run inside the native worker.
      int id = paint.LoadEffect(dir / fs::u8path(name.get<std::string>()), true, pigments.size());
      if (id < 0)
        throw std::runtime_error(paint.LastError());
      pigments.push_back(id);
    }
    if (pigments.empty())
      throw std::runtime_error("Paintball pigment list is empty");
    paint.SetBackground(r.background());
  }
  void configure(const PackAppearance &a) {
    speed = a.speed;
    paint.SetAppearance(a.scale, a.brightness, a.intensity, a.color, speed);
    paint.SetDynamicInputs(a.dynamicInputs);
    paint.SetColorGrade(a.saturation, a.hue, a.resolvedPaletteColors, a.paletteOverride);
    std::vector<EffekseerBackend::UniformValue> uniforms;
    for (auto &u : a.uniforms)
      uniforms.push_back({u.name, u.component, u.value});
    std::string error;
    for (int id : pigments)
      if (!paint.SetMaterialUniforms(id, uniforms, error))
        throw std::runtime_error(error);
  }
  void clear() { paint.StopAll(); }
  void key(const A75Event &e, NativeRenderer &r) {
    bool wasIdle = paint.CatchUpIdle();
    if (paint.ActiveEffectCount() >= 64 && !paint.StopOldest())
      return;
    if (wasIdle || !paint.HasActiveEffects())
      lastFrame = e.seconds;
    randomState ^= randomState >> 12;
    randomState ^= randomState << 25;
    randomState ^= randomState >> 27;
    size_t i = (randomState * 2685821657736338717ull) % pigments.size();
    paint.PlayAt(pigments[i], e.x * r.width(), e.y * r.height(), scale);
  }
  int draw(double now, NativeRenderer &, bool paused) {
    if (!paused && paint.HasActiveEffects()) {
      paint.UpdateForFrame(static_cast<float>(std::clamp(now - lastFrame, 0.0, 0.1)) * speed);
      if (!paint.Draw())
        return -2;
    }
    lastFrame = now;
    return !paused && paint.HasVisibleEffects() ? 0 : -1;
  }
};
} // namespace atlas_packs
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  return atlas_packs::api<atlas_packs::Paintball>(abi);
}
