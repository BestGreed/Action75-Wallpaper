#pragma once
#include "SpriteBurstState.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>

namespace action75 {
class SpriteBurst {
public:
  struct Params {
    unsigned columns = 0, rows = 0;
    float duration = 0, diameter = 0; // Seconds; pixels at 720 pixels of display height.
    bool multiply = false;
  };
  static bool valid(const Params &p) noexcept;
  // The caller loads into a fresh instance and swaps only after all assets succeed.
  bool load(ID3D11Device *device, const std::filesystem::path &file,
            Params params, std::string &error, bool forceAtlas2D=false);
  bool push(double now, float x, float y) noexcept {
    if (!state_.push(now, duration(), x, y, 1, nextPaletteIndex_)) return false;
    ++nextPaletteIndex_;
    return true;
  }
  bool draw(ID3D11DeviceContext *context, unsigned width, unsigned height,
            double now, std::string &error);
  void clear() noexcept { state_.reset(); }
  void setBrightness(float brightness) noexcept {
    brightness_ = std::isfinite(brightness) ? std::clamp(brightness, 0.0f, 4.0f) : 1.0f;
  }
  void setScale(float scale) noexcept {
    scale_ = std::isfinite(scale) ? std::clamp(scale, .5f, 3.0f) : 1.0f;
  }
  void setIntensity(float intensity) noexcept {
    intensity_ = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 3.0f) : 1.0f;
  }
  void setColor(const std::array<std::uint8_t, 3>& color) noexcept {
    for (std::size_t i = 0; i < color.size(); ++i) tint_[i] = color[i] / 255.0f;
  }
  void setColorGrade(float saturation, float hueDegrees,
                    const std::vector<std::array<std::uint8_t,3>>& palette, bool usePalette);
  void setDuration(float seconds) noexcept {
    durationOverride_=std::isfinite(seconds) && seconds>0 ? std::clamp(seconds,.12f,3.0f) : 0;
  }
  void setSpeed(float speed) noexcept {
    speed_=std::isfinite(speed) ? std::clamp(speed,.25f,3.0f) : 1;
  }
  bool volumeAtlas() const noexcept { return volumeAtlas_; }
  std::size_t count() const noexcept { return state_.count(); }

private:
  float duration() const noexcept {return (durationOverride_>0 ? durationOverride_ : p_.duration)/speed_;}
  using Bound = std::array<float,4>; // normalized tile UV range, including bilinear guard
  std::array<Bound,96> bounds_{};
  SpriteBurstState state_;
  Params p_;
  float atlasWidth_ = 0, atlasHeight_ = 0;
  float brightness_ = 1;
  float scale_ = 1, intensity_ = 1;
  float durationOverride_=0, speed_=1;
  std::array<float, 4> tint_{1, 1, 1, 1};
  std::array<float, 4> grading_{1, 1, 0, 0}; // saturation, cos(hue), sin(hue), hue enabled
  std::vector<std::array<float, 4>> palette_;
  std::uint32_t nextPaletteIndex_=0;
  bool volumeAtlas_ = false;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
};
}
