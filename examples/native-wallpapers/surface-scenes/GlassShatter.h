#pragma once

#include "Action75Wallpaper12.h"
#include "Gpu.h"
#include "Settings.h"

#include <array>
#include <cstdint>
#include <random>

namespace surface {

class GlassShatter {
public:
  static constexpr unsigned maxImpacts = 24, maxBranches = 7, maxChips = 4;
  GlassShatter(ID3D12Device *device, unsigned width, unsigned height);
  void configure(const Settings &settings);
  void push(double now, float x, float y) noexcept;
  void clear() noexcept { count_ = 0; }
  bool hasMotion() const noexcept { return count_ != 0; }
  void draw(const A75Frame12 &frame, bool paused);

private:
  using Float4 = std::array<float, 4>;
  struct DrawImpact {
    Float4 position, motion, reaction;
  };
  struct Impact {
    double lastHit;
    float reachOnHit, targetReach;
    DrawImpact draw;
  };
  struct Frame {
    Float4 screen, geometry, shadow, reflection, rim, prism, optics, baseShadow, baseReflection;
    std::array<DrawImpact, maxImpacts> impacts;
  };
  void discardExpired(double now) noexcept;

  Gpu gpu_;
  ComPtr<ID3D12PipelineState> background_, cracks_, panes_;
  ComPtr<ID3D12PipelineState> transparentCracks_, transparentPanes_;
  StructuredBuffer crackGeometry_, paneGeometry_;
  std::array<Impact, maxImpacts> impacts_{};
  Frame data_{};
  std::mt19937 random_;
  unsigned count_ = 0, crackVertices_ = 0, paneVertices_ = 0;
  float duration_ = 1.20f;
  float fractureOpacity_ = 1.f;
  float minStrength_ = .60f, maxStrength_ = 1.45f;
};

} // namespace surface
