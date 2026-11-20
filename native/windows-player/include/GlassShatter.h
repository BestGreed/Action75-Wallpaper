#pragma once
#include "LayerColor.h"
#include "PackSettings.h"
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <random>
#include <string>
#include <wrl/client.h>

namespace action75 {
class GlassShatter {
public:
  static constexpr unsigned maxImpacts = 24;

  GlassShatter();
  explicit GlassShatter(std::uint32_t seed);
  bool load(ID3D11Device *device, std::string &error);
  void configure(const PackAppearance &appearance) noexcept;
  bool push(double now, float x, float y) noexcept;
  void clear() noexcept { count_ = 0; }
  unsigned count() const noexcept { return count_; }
  float lastStrength() const noexcept { return lastStrength_; }
  bool draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
            std::string &error);

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

  Microsoft::WRL::ComPtr<ID3D11VertexShader> backdropVs_, crackVs_, paneVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> backdropPs_, crackPs_, panePs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> frame_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cracks_, panes_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> alphaBlend_;

  std::array<Impact, maxImpacts> impacts_{};
  Frame data_{};
  std::mt19937 random_;
  unsigned count_ = 0, crackVertices_ = 0, paneVertices_ = 0;
  float duration_ = 1.20f;
  float minStrength_ = .60f, maxStrength_ = 1.45f, lastStrength_ = 0;
};
} // namespace action75
