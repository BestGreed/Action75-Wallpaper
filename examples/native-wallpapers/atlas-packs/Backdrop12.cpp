#include "Wallpaper12.h"
#include "BackdropShaders12.h"

#include <algorithm>
#include <cmath>

namespace atlas_packs {
namespace {
std::array<float, 4> solidBackground(const std::array<float, 8>& appearance) {
  const float r = appearance[4], g = appearance[5], b = appearance[6];
  const float peak = std::max({r, g, b}), chroma = peak - std::min({r, g, b});
  std::array<float, 4> color{r, g, b, 1};
  if (chroma > 0 && (appearance[2] != 0 || appearance[1] != 1)) {
    float hue = peak == r ? (g - b) / chroma : peak == g ? (b - r) / chroma + 2 : (r - g) / chroma + 4;
    hue = hue / 6 + appearance[2];
    const float saturation = std::clamp(chroma / peak * appearance[1], 0.f, 1.f);
    constexpr float offsets[]{0, 2.f / 3, 1.f / 3};
    for (unsigned i = 0; i < 3; ++i) {
      const float h = hue + offsets[i];
      const float pure = std::clamp(std::abs((h - std::floor(h)) * 6 - 3) - 1, 0.f, 1.f);
      color[i] = peak * (1 + saturation * (pure - 1));
    }
  }
  const float gain = appearance[0];
  const float exposure = gain <= 1 ? gain : gain / (1 + (gain - 1) * peak);
  for (unsigned i = 0; i < 3; ++i)
    color[i] *= exposure;
  return color;
}
} // namespace

Backdrop::Backdrop(ID3D12Device* device)
    : constants_(device, sizeof(appearance_)),
      pipeline_(device, {kBackdropVS, sizeof(kBackdropVS)}, {kBackdropPS, sizeof(kBackdropPS)},
                Blend::Opaque, true) {}

void Backdrop::setImage(ID3D12Device* device, const fs::path& file) {
  const auto image = decodeImage(file, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
  texture_.load(device, image);
  for (std::size_t i = 3; i < image.pixels.size(); i += 4)
    if (image.pixels[i] != 255)
      return;
  opaqueWidth_ = image.width;
  opaqueHeight_ = image.height;
}

void Backdrop::setCanvas(ID3D12Device* device, ComPtr<ID3D12Resource> texture) {
  if (texture)
    texture_.attach(device, std::move(texture));
  else
    texture_ = {};
}

void Backdrop::configure(const Appearance& settings) {
  const float gain = std::clamp(settings.backgroundBrightness, 0.f, 4.f);
  const float saturation = std::clamp(settings.backgroundSaturation, 0.f, 2.5f);
  float hue = std::fmod(settings.backgroundHue, 360.f);
  if (hue < 0)
    hue += 360;
  hue /= 360;
  const auto tint = settings.backgroundColor;
  const bool grade = gain != 1 || saturation != 1 || hue != 0 || tint != Rgb{255, 255, 255};
  appearance_ = {gain, saturation, hue, float(grade), tint[0] / 255.f, tint[1] / 255.f,
                 tint[2] / 255.f, float(settings.solidBackground)};
}

void Backdrop::draw(const A75Frame12& frame) {
  auto* commands = frame.commands;
  texture_.prepare(commands);
  if (appearance_[3] == 0 && appearance_[7] == 0 &&
      frame.width == opaqueWidth_ && frame.height == opaqueHeight_) {
    // Match the original fast path for opaque, ungraded art at native size.
    auto* source = texture_.resource();
    transition(commands, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(commands, frame.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    commands->CopyResource(frame.target, source);
    transition(commands, frame.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(commands, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return;
  }
  if (appearance_[7] != 0 || appearance_[0] == 0 ||
      (appearance_[4] == 0 && appearance_[5] == 0 && appearance_[6] == 0)) {
    const auto color = solidBackground(appearance_);
    commands->ClearRenderTargetView(frame.rtv, color.data(), 0, nullptr);
    return;
  }
  constants_.write(appearance_.data(), sizeof(appearance_));
  pipeline_.bind(commands, constants_, &texture_);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->DrawInstanced(3, 1, 0, 0);
}
} // namespace atlas_packs
