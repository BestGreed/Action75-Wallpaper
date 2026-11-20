#pragma once
#include "LayerColor.h"
#include "PackSettings.h"
#include <array>
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

namespace action75 {
class FireworksScene {
public:
  static constexpr unsigned maxBursts = 112;
  static constexpr unsigned patternSize = 384;
  static constexpr unsigned maxVisibleSparks = 5600;

  bool load(ID3D11Device *device, unsigned width, unsigned height, std::string &error);
  void configure(const PackAppearance &appearance);
  bool readyForKeyBurst() noexcept { return playback_.readyForKeyBurst(); }
  bool push(double now, float x, float y) noexcept;
  void clear() noexcept;
  unsigned count() const noexcept { return count_; }
  unsigned particleCount() const noexcept { return visibleSparks_; }
  unsigned lastShape() const noexcept { return lastShape_; }
  unsigned lastPalette() const noexcept { return lastPalette_; }
  bool draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
            std::string &error);

private:
  using Float4 = std::array<float, 4>;
  static constexpr unsigned colorsPerPalette = 9;
  static constexpr unsigned paletteSlots = 1 + FireworksAppearance::paletteLimit;
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
  struct Target {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  };
  bool resize(unsigned width, unsigned height, std::string &error);
  void discardExpired(double now) noexcept;
  void filter(ID3D11DeviceContext *context, const Target &input, const Target &output,
              const Float4 &step);
  unsigned choosePalette() noexcept;
  float random() noexcept;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> sparkVs_, screenVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> sparkPs_, blurPs_, compositePs_, idlePs_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> patterns_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> frame_, filterStep_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> additive_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Target light_, blurA_, blurB_;
  DXGI_FORMAT format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  unsigned width_ = 0, height_ = 0, blurWidth_ = 0, blurHeight_ = 0;

  std::array<Burst, maxBursts> bursts_{};
  std::array<std::array<Float4, patternSize>, FireworksAppearance::shapeCount> timings_{};
  std::array<float, FireworksAppearance::shapeCount> lifespan_{};
  Frame data_{};
  std::array<unsigned, paletteSlots> paletteSizes_{};
  FireworksAppearance appearance_;
  VariantPlaybackState playback_{appearance_};
  float brightness_ = 1, scale_ = 1, speed_ = 1, intensity_ = 1;
  unsigned paletteChoices_ = 0, count_ = 0, visibleSparks_ = 0;
  unsigned lastShape_ = 0, lastPalette_ = 0;
  std::uint32_t randomState_ = 0xc2ab615du;
};
} // namespace action75
