#pragma once

#include "Action75Wallpaper12.h"
#include "Gpu.h"
#include "LayerColor.h"
#include "Settings.h"
#include "VariantPlayback.h"

#include <array>
#include <cstdint>

namespace surface {

class FireworksScene {
public:
  static constexpr unsigned maxBursts = 112, patternSize = 384, maxVisibleSparks = 5600;
  static constexpr unsigned shapeCount = VariantPlaybackRules::maxVariants, heartShape = 8;
  FireworksScene(ID3D12Device *device, unsigned width, unsigned height);
  void configure(const Settings &settings);
  bool readyForKeyBurst() noexcept { return playback_.readyForKeyBurst(); }
  void push(double now, float x, float y) noexcept;
  void clear() noexcept;
  bool hasMotion() const noexcept { return count_ != 0; }
  void draw(const A75Frame12 &frame, bool paused);

private:
  using Float4 = std::array<float, 4>;
  static constexpr unsigned colorsPerPalette = 9, paletteSlots = 9;
  struct Style {
    unsigned quality = 1;
    bool randomPalette = false;
    float density = .82f, trail = 1, gravity = 1, lifetime = 1, shimmer = .85f;
    float embers = 1, bloom = .82f, smoke = .55f, reflection = .56f, horizon = .87f;
  };
  struct DrawBurst {
    Float4 location, motion, rotation, palette;
  };
  struct Burst {
    double time, expires;
    DrawBurst render;
    float speed;
    unsigned paletteSlot;
  };
  struct Frame {
    Float4 screen, exposure, atmosphere, background;
    LayerColor skyGrade, sparkGrade;
    std::array<Float4, paletteSlots * colorsPerPalette> palette;
    std::array<DrawBurst, maxBursts> burst;
  };

  void resize(unsigned width, unsigned height);
  void discardExpired(double now) noexcept;
  void filter(ID3D12GraphicsCommandList *commands, Texture &input, Texture &output,
              const Float4 &step);
  unsigned choosePalette() noexcept;
  float random() noexcept;

  Gpu gpu_;
  ComPtr<ID3D12PipelineState> spark_, blur_, composite_, idle_;
  StructuredBuffer patterns_;
  Texture light_, blurA_, blurB_;
  DXGI_FORMAT format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  unsigned width_ = 0, height_ = 0, blurWidth_ = 0, blurHeight_ = 0;

  std::array<Burst, maxBursts> bursts_{};
  std::array<std::array<Float4, patternSize>, shapeCount> timings_{};
  std::array<float, shapeCount> lifespan_{};
  Frame data_{};
  std::array<unsigned, paletteSlots> paletteSizes_{};
  Style style_;
  VariantPlaybackState playback_;
  float brightness_ = 1, scale_ = 1, speed_ = 1;
  unsigned paletteChoices_ = 0, count_ = 0, lastPalette_ = 0;
  std::uint32_t randomState_ = 0xc2ab615du;
};

} // namespace surface
