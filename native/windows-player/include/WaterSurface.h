#pragma once
#include "LayerColor.h"
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

namespace action75 {
struct PackAppearance;
enum class WaterBackdrop;

class WaterSurface {
public:
  using Position = std::array<float, 2>;
  static constexpr unsigned MaxImpacts = 128;

  bool load(ID3D11Device *, ID3D11DeviceContext *, unsigned width, unsigned height,
            std::string &error);
  void configure(const PackAppearance &) noexcept;
  bool push(Position point) noexcept;
  void clear() noexcept;
  bool hasWaves() const noexcept { return contactCount_ > 0 || remaining_ > 0; }
  bool hasMotion() const noexcept;
  bool draw(ID3D11DeviceContext *, unsigned width, unsigned height, double now, bool paused,
            std::string &error);

private:
  struct Texture {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> output;
  };

  bool resize(ID3D11Device *, unsigned width, unsigned height, std::string &error);
  unsigned buildImpulses(float elapsed) noexcept;
  void stampImpacts(ID3D11DeviceContext *, unsigned count);
  void advanceWaves(ID3D11DeviceContext *, unsigned steps, bool inject);

  Microsoft::WRL::ComPtr<ID3D11VertexShader> screenVs_, impactVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> surfacePs_, impactPs_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> waveCs_, slopeCs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> frameConstants_, impactConstants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> clampSampler_, wrapSampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> additive_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> optics_, pressure_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pressureTarget_;
  std::array<Texture, 2> wave_;
  Texture slope_;

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
  std::array<Contact, MaxImpacts> contacts_{};
  std::array<Impulse, MaxImpacts> impulses_{};
  unsigned contactCount_ = 0;
  std::uint32_t rng_ = 1;
  bool pendingImpact_ = false, needsClear_ = true, wasPaused_ = false;
  double lastFrame_ = -1, phase_ = 0, accumulator_ = 0;
  float remaining_ = 0;
  float intensity_ = 1, size_ = 1, speed_ = 1, damping_ = 1.8f, ambient_ = .72f;
  float minStrength_ = .62f, maxStrength_ = 1.45f, crestBrightness_ = 1.35f;
  WaterBackdrop backdrop_{};
  float backgroundBrightness_ = 1, backgroundSaturation_ = 1, texture_ = .50f, caustics_ = .45f;
  LayerColor waterHue_, crestColor_;
  std::array<float, 3> tint_{1, 1, 1};
};
} // namespace action75
