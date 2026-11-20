#pragma once
#include "BlackHoleAppearance.h"
#include <array>
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

namespace action75 {
class BlackHoleScene {
public:
  using Position = std::array<float, 2>;
  static constexpr std::size_t MaxStars = 96;

  bool load(ID3D11Device *, ID3D11DeviceContext *, ID3D11ShaderResourceView *artwork,
            std::string &error);
  void configure(const BlackHoleAppearance &, float starSize, float starBrightness, bool updateCenter = true);
  Position center() const noexcept { return appearance_.center; }
  void setCenter(Position center) noexcept;
  bool covers(Position point, unsigned width, unsigned height) const noexcept;
  bool push(double now, Position point, unsigned width, unsigned height) noexcept;
  void clear() noexcept { count_ = 0; }
  std::size_t count() const noexcept { return count_; }
  bool draw(ID3D11DeviceContext *, unsigned width, unsigned height, double now, bool paused,
            std::string &error);

private:
  struct HeatState {
    float weight = 0;
    std::array<float, 3> spectrum{};
    void decay(float factor) noexcept;
    void deposit(float amount, const std::array<float, 3> &color) noexcept;
  };
  struct PulseState {
    double time = 0;
    HeatState fast, retained;
    HeatState emission(double now) const noexcept;
  };
  struct Star {
    Position point{};
    double born = 0;
    float radius = 0, rotation = 0, seed = 0;
    PulseState pulse;
    std::array<PulseState, 16> history{};
    std::size_t nextHistory = 0, historyCount = 0;
    void addHeat(double now, const std::array<float, 3> &color) noexcept;
    HeatState emission(double now) const noexcept;
    bool expired(double now) const noexcept;
  };
  struct Surface {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  };
  bool resize(ID3D11DeviceContext *, unsigned width, unsigned height, std::string &error);
  float shadowPixels(unsigned width, unsigned height) const noexcept;
  std::uint32_t random() noexcept;
  float randomUnit() noexcept;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> screenVs_, starVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> scenePs_, starPs_, lightSourcePs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> bloomHorizontal_, bloomVertical_, composite_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sky_, skyStars_, turbulence_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> sceneConstants_, starConstants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> skySampler_, turbulenceSampler_, emissionSampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> starBlend_;
  Surface hdr_, bloom_[2];
  DXGI_FORMAT hdrFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  unsigned width_ = 0, height_ = 0, bloomWidth_ = 0, bloomHeight_ = 0;
  std::array<Star, MaxStars> stars_{};
  std::size_t count_ = 0;
  std::uint32_t rng_ = 0xdf93e507;
  std::size_t lastColor_ = 0;
  BlackHoleAppearance appearance_;
  float starSize_ = 1, starBrightness_ = 1;
  std::array<double, 4> skyPhase_{};
  double previousFrame_ = -1, phase_ = 0;
  bool wasPaused_ = false;
};
} // namespace action75
