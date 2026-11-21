#include "Wallpaper12.h"
#include "AtlasShaders12.h"
#include "SpriteBurstState.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atlas_packs {
namespace {
using Vec4 = std::array<float, 4>;
constexpr std::size_t maxHits = SpriteBurstState::capacity;
struct Constants {
  float width, height, diameter, invColumns;
  float invRows, atlasWidth, atlasHeight, columns;
  Vec4 tint, grading, opacity;
  std::array<SpriteBurstSample, maxHits> samples;
  std::array<Vec4, maxHits> bounds, palettes;
};
static_assert(sizeof(Constants) == 1232);

struct Recipe {
  unsigned columns, rows;
  float duration, diameter;
  bool multiply;
};

Vec4 tileBounds(const Image& image, unsigned tile, unsigned ox, unsigned oy) {
  unsigned minX = tile, minY = tile, maxX = 0, maxY = 0;
  for (unsigned y = 0; y < tile; ++y) {
    const auto* row = image.pixels.data() + (std::size_t(oy + y) * image.width + ox) * 4;
    auto lit = [row](unsigned x) { return row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]; };
    unsigned first = 0;
    while (first < tile && !lit(first))
      ++first;
    if (first == tile)
      continue;
    minX = std::min(minX, first);
    minY = std::min(minY, y);
    maxY = y;
    unsigned last = tile - 1;
    while (last > maxX && last > first && !lit(last))
      --last;
    maxX = std::max(maxX, last);
  }
  if (minX == tile)
    return {1, 1, 0, 0};
  // Leave two source pixels for linear filtering around a crop.
  auto low = [tile](unsigned v) { return float(v > 2 ? v - 2 : 0) / (tile - 1); };
  auto high = [tile](unsigned v) { return float(std::min(tile - 1, v + 2)) / (tile - 1); };
  return {low(minX), low(minY), high(maxX), high(maxY)};
}
} // namespace

class Atlas {
public:
  static constexpr bool canvas = false;
  Atlas(const A75Create12& create, const json& recipe)
      : recipe_{recipe.at("columns"), recipe.at("rows"), recipe.at("duration"),
                recipe.at("diameter"), recipe.value("composition", "add") == "multiply"},
        constants_(create.device, sizeof(Constants)),
        pipeline_(create.device, {kAtlasVS, sizeof(kAtlasVS)}, {kAtlasPS, sizeof(kAtlasPS)},
                  recipe_.multiply ? Blend::Multiply : Blend::Add, true) {
    const auto columns = recipe_.columns, rows = recipe_.rows;
    if (!columns || columns > 12 || !rows || rows > 12 || columns * rows < 2 ||
        columns * rows > bounds_.size() || !std::isfinite(recipe_.duration) ||
        recipe_.duration < .12f || recipe_.duration > 3 || !std::isfinite(recipe_.diameter) ||
        recipe_.diameter < 48 || recipe_.diameter > 440)
      throw std::runtime_error("Invalid sprite atlas recipe");
    const auto image = decodeImage(assetPath(create.pack_dir, recipe.at("effect")), 2048);
    const auto tile = image.width / columns;
    if (image.width % columns || image.height % rows || image.height / rows != tile || tile < 16)
      throw std::runtime_error("Sprite atlas requires equal square tiles of at least 16 pixels");
    atlasWidth_ = float(image.width);
    atlasHeight_ = float(image.height);
    for (unsigned i = 0; i < columns * rows; ++i)
      bounds_[i] = tileBounds(image, tile, i % columns * tile, i / columns * tile);
    texture_.load(create.device, image);
  }

  void configure(const Appearance& appearance, double) {
    opacity_ = appearance.foregroundOpacity;
    brightness_ = std::clamp(appearance.brightness, 0.f, 4.f);
    scale_ = std::clamp(appearance.scale, .5f, 3.f);
    intensity_ = std::clamp(appearance.intensity, 0.f, 3.f);
    duration_ = appearance.duration > 0 ? std::clamp(appearance.duration, .12f, 3.f) : recipe_.duration;
    duration_ /= std::clamp(appearance.speed, .25f, 3.f);

    for (unsigned i = 0; i < 3; ++i)
      tint_[i] = appearance.color[i] / 255.f;
    tint_[3] = recipe_.multiply ? 1.f : 0.f;
    const float hue = std::clamp(appearance.hue, -180.f, 180.f);
    const float radians = hue * .01745329251994329577f;
    grading_ = {std::clamp(appearance.saturation, 0.f, 2.5f), std::cos(radians),
                std::sin(radians), hue != 0 ? 1.f : 0.f};
    palette_.clear();
    for (const auto& rgb : appearance.palette)
      palette_.push_back({rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f, 1});
  }

  void clear() { hits_.reset(); }
  void key(const A75Event& event) {
    if (hits_.push(event.seconds, duration_, event.x, event.y, 1, nextColor_))
      ++nextColor_;
  }

  int draw(const A75Frame12& frame, bool paused) {
    if (paused)
      return -1;
    texture_.prepare(frame.commands);
    if (!hits_.count())
      return -1;
    const unsigned frames = recipe_.columns * recipe_.rows;
    Constants values{float(frame.width), float(frame.height),
                     recipe_.diameter * float(frame.height) / 720.f * scale_,
                     1.f / recipe_.columns, 1.f / recipe_.rows, atlasWidth_, atlasHeight_,
                     float(recipe_.columns), tint_, grading_, {opacity_}, {}, {}, {}};
    std::array<uint32_t, maxHits> paletteIndices{};
    const auto count = hits_.sample(frame.seconds, duration_, frames, values.samples, &paletteIndices);
    unsigned drawn = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto position = static_cast<unsigned>(values.samples[i].frame);
      const auto& a = bounds_[position];
      const auto& b = bounds_[std::min(frames - 1, position + 1)];
      const Vec4 bounds{std::min(a[0], b[0]), std::min(a[1], b[1]),
                       std::max(a[2], b[2]), std::max(a[3], b[3])};
      if (bounds[0] >= bounds[2] || bounds[1] >= bounds[3])
        continue;
      values.samples[drawn] = values.samples[i];
      values.samples[drawn].gain *= brightness_ * intensity_;
      values.palettes[drawn] = palette_.empty() ? Vec4{1, 1, 1, 0}
                                               : palette_[paletteIndices[i] % palette_.size()];
      values.bounds[drawn++] = bounds;
    }
    if (drawn) {
      constants_.write(&values, sizeof(values));
      pipeline_.bind(frame.commands, constants_, &texture_);
      frame.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      frame.commands->DrawInstanced(4, drawn, 0, 0);
    }
    return count ? 0 : -1;
  }

private:
  Recipe recipe_;
  float opacity_ = 1;
  std::array<Vec4, 96> bounds_{};
  SpriteBurstState hits_;
  Texture texture_;
  Upload constants_;
  Pipeline pipeline_;
  Vec4 tint_{1, 1, 1, 0}, grading_{1, 1, 0, 0};
  std::vector<Vec4> palette_;
  uint32_t nextColor_ = 0;
  float brightness_ = 1, scale_ = 1, intensity_ = 1, duration_ = 1;
  float atlasWidth_ = 0, atlasHeight_ = 0;
};
} // namespace atlas_packs

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
  return atlas_packs::api12<atlas_packs::Atlas>(abi);
}
