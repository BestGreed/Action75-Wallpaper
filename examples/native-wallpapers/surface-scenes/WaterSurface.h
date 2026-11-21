#pragma once

#include "Action75Wallpaper12.h"
#include "Gpu.h"
#include "LayerColor.h"
#include "Settings.h"

#include <array>
#include <cstdint>

namespace surface {

class WaterSurface {
public:
  using Position = std::array<float, 2>;
  static constexpr unsigned maxImpacts = 128;
  WaterSurface(ID3D12Device *device, unsigned width, unsigned height);
  void configure(const Settings &settings);
  void push(double now, float x, float y) noexcept;
  void clear() noexcept;
  bool hasMotion() const noexcept;
  void draw(const A75Frame12 &frame, bool paused);

private:
  static constexpr unsigned opticsSize = 512, opticsLevels = 10;
  bool hasWaves() const noexcept { return contactCount_ > 0 || remaining_ > 0; }
  void resize(unsigned width, unsigned height);
  void bakeOptics(ID3D12GraphicsCommandList *commands);
  unsigned buildImpulses(float elapsed) noexcept;
  void stampImpacts(ID3D12GraphicsCommandList *commands, unsigned count);
  void advanceWaves(ID3D12GraphicsCommandList *commands, unsigned steps, bool inject);

  Gpu gpu_;
  ComPtr<ID3D12PipelineState> surface_, stamp_, step_, slopePipeline_, bake_, downsample_;
  Texture optics_, pressure_, slope_;
  std::array<Texture, 2> wave_;
  std::array<View, opticsLevels - 1> inputMip_, outputMip_;
  bool opticsReady_ = false;

  unsigned width_ = 0, height_ = 0, columns_ = 0, rows_ = 0, front_ = 0;
  Position extent_{};
  struct Contact {
    Position position;
    float radius, force, roundness, age;
  };
  struct Impulse {
    Position position;
    float radius, roundness;
    float displacement, compression, rebound, padding;
  };
  std::array<Contact, maxImpacts> contacts_{};
  std::array<Impulse, maxImpacts> impulses_{};
  unsigned contactCount_ = 0;
  std::uint32_t rng_ = 1;
  bool pendingImpact_ = false, needsClear_ = true, wasPaused_ = false;
  double lastFrame_ = -1, phase_ = 0, accumulator_ = 0;
  float remaining_ = 0;
  float intensity_ = 1, size_ = 1, speed_ = 1, damping_ = 1.8f, ambient_ = .72f;
  float minStrength_ = .62f, maxStrength_ = 1.45f, crestBrightness_ = 1.35f;
  unsigned backdrop_ = 0;
  float backgroundBrightness_ = 1, backgroundSaturation_ = 1, texture_ = .50f, caustics_ = .45f;
  LayerColor waterHue_, crestColor_;
  std::array<float, 3> tint_{1, 1, 1};
};

} // namespace surface
