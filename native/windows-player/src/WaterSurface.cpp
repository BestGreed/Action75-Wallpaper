#include "WaterSurface.h"
#include "PackSettings.h"
#include "WaterSurfaceShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace action75 {
namespace {
constexpr unsigned OpticsSize = 512;
constexpr unsigned MaxSteps = 8;
constexpr double Step = 1.0 / 180.0;

float smoothUnit(float value) noexcept {
  const float t = std::clamp(value, 0.f, 1.f);
  return t * t * (3 - 2 * t);
}

struct FrameConstants {
  float extentX, extentY, inverseExtentX, inverseExtentY;
  float cellX, cellY, inverseCellX, inverseCellY;
  float step, waveSpeedSquared, damping, visibility;
  float phase, backgroundBrightness, crestBrightness, backdrop;
  float tintR, tintG, tintB, backgroundSaturation;
  float texture, caustics, padding[2];
  LayerColor waterHue, crestColor;
};
static_assert(sizeof(FrameConstants) == 192);

bool check(HRESULT hr, const char *operation, std::string &error) {
  if (SUCCEEDED(hr))
    return true;
  std::ostringstream message;
  message << "Water surface " << operation << " failed (0x" << std::hex
          << static_cast<unsigned long>(hr) << ')';
  error = message.str();
  return false;
}
} // namespace

bool WaterSurface::load(ID3D11Device *device, ID3D11DeviceContext *context, unsigned width,
                        unsigned height, std::string &error) {
  ComPtr<ID3D11ComputeShader> bake;
  if (!check(device->CreateVertexShader(WaterScreenVertex, sizeof(WaterScreenVertex), nullptr,
                                  screenVs_.ReleaseAndGetAddressOf()), "ScreenVertex", error) ||
      !check(device->CreateVertexShader(WaterImpactVertex, sizeof(WaterImpactVertex), nullptr,
                                  impactVs_.ReleaseAndGetAddressOf()), "ImpactVertex", error) ||
      !check(device->CreatePixelShader(WaterWaterPixel, sizeof(WaterWaterPixel), nullptr,
                                  surfacePs_.ReleaseAndGetAddressOf()), "WaterPixel", error) ||
      !check(device->CreatePixelShader(WaterImpactPixel, sizeof(WaterImpactPixel), nullptr,
                                  impactPs_.ReleaseAndGetAddressOf()), "ImpactPixel", error) ||
      !check(device->CreateComputeShader(WaterStepWave, sizeof(WaterStepWave), nullptr,
                                  waveCs_.ReleaseAndGetAddressOf()), "StepWave", error) ||
      !check(device->CreateComputeShader(WaterBuildSlope, sizeof(WaterBuildSlope), nullptr,
                                  slopeCs_.ReleaseAndGetAddressOf()), "BuildSlope", error) ||
      !check(device->CreateComputeShader(WaterBakeOptics, sizeof(WaterBakeOptics), nullptr,
                                  bake.ReleaseAndGetAddressOf()), "BakeOptics", error))
    return false;

  D3D11_BUFFER_DESC buffer{};
  buffer.Usage = D3D11_USAGE_DEFAULT;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.ByteWidth = sizeof(FrameConstants);
  static_assert(sizeof(Impulse) == 32);
  if (!check(device->CreateBuffer(&buffer, nullptr, &frameConstants_), "frame constants", error))
    return false;
  buffer.ByteWidth = sizeof(impulses_);
  if (!check(device->CreateBuffer(&buffer, nullptr, &impactConstants_), "impact constants", error))
    return false;

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (!check(device->CreateSamplerState(&sampler, &clampSampler_), "slope sampler", error))
    return false;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  if (!check(device->CreateSamplerState(&sampler, &wrapSampler_), "optics sampler", error))
    return false;

  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend{};
  auto &channel = blend.RenderTarget[0];
  channel.BlendEnable = TRUE;
  channel.SrcBlend = channel.DestBlend = channel.SrcBlendAlpha = channel.DestBlendAlpha =
      D3D11_BLEND_ONE;
  channel.BlendOp = channel.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  channel.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
  if (!check(device->CreateRasterizerState(&raster, &raster_), "rasterizer", error) ||
      !check(device->CreateBlendState(&blend, &additive_), "impact blending", error) ||
      !resize(device, width, height, error))
    return false;

  D3D11_TEXTURE2D_DESC map{};
  map.Width = map.Height = OpticsSize;
  map.ArraySize = map.SampleDesc.Count = 1;
  map.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  map.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
  map.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11UnorderedAccessView> output;
  D3D11_UNORDERED_ACCESS_VIEW_DESC mip{};
  mip.Format = map.Format;
  mip.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
  if (!check(device->CreateTexture2D(&map, nullptr, &texture), "optics texture", error) ||
      !check(device->CreateUnorderedAccessView(texture.Get(), &mip, &output), "optics UAV", error) ||
      !check(device->CreateShaderResourceView(texture.Get(), nullptr, &optics_), "optics SRV", error))
    return false;

  ID3D11UnorderedAccessView *uav = output.Get(), *nullOutput = nullptr;
  context->CSSetShader(bake.Get(), nullptr, 0);
  context->CSSetUnorderedAccessViews(2, 1, &uav, nullptr);
  context->Dispatch(OpticsSize / 8, OpticsSize / 8, 1);
  context->CSSetUnorderedAccessViews(2, 1, &nullOutput, nullptr);
  context->CSSetShader(nullptr, nullptr, 0);
  context->GenerateMips(optics_.Get());
  rng_ = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
  return true;
}

bool WaterSurface::resize(ID3D11Device *device, unsigned width, unsigned height, std::string &error) {
  if (!width || !height) {
    error = "Water surface requires a nonempty viewport";
    return false;
  }
  const float shortSide = static_cast<float>(std::min(width, height));
  // The slight tilt foreshortens the surface vertically. Use almost square
  // cells in water space and bound work independently of display resolution.
  const Position extent{width / shortSide, 1.17f * height / shortSide};
  const float pitch = std::max({1.f / 164, 3.f / shortSide, std::max(extent[0], extent[1]) / 768});
  const auto gridSize = [pitch](float span) {
    return static_cast<unsigned>(std::clamp(std::lround(span / pitch), 4l, 768l));
  };
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = gridSize(extent[0]);
  desc.Height = gridSize(extent[1]);
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  const auto create = [&](DXGI_FORMAT format, Texture &surface) {
    desc.Format = format;
    ComPtr<ID3D11Texture2D> texture;
    return check(device->CreateTexture2D(&desc, nullptr, &texture), "grid texture", error) &&
           check(device->CreateShaderResourceView(texture.Get(), nullptr, &surface.view),
                 "grid SRV", error) &&
           check(device->CreateUnorderedAccessView(texture.Get(), nullptr, &surface.output),
                 "grid UAV", error);
  };
  std::array<Texture, 2> wave;
  Texture slope;
  if (!create(DXGI_FORMAT_R32G32_FLOAT, wave[0]) || !create(DXGI_FORMAT_R32G32_FLOAT, wave[1]) ||
      !create(DXGI_FORMAT_R16G16B16A16_FLOAT, slope))
    return false;

  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> pressure;
  ComPtr<ID3D11ShaderResourceView> pressureView;
  ComPtr<ID3D11RenderTargetView> pressureTarget;
  if (!check(device->CreateTexture2D(&desc, nullptr, &pressure), "impact texture", error) ||
      !check(device->CreateShaderResourceView(pressure.Get(), nullptr, &pressureView),
             "impact SRV", error) ||
      !check(device->CreateRenderTargetView(pressure.Get(), nullptr, &pressureTarget),
             "impact target", error))
    return false;

  wave_ = std::move(wave);
  slope_ = std::move(slope);
  pressure_ = std::move(pressureView);
  pressureTarget_ = std::move(pressureTarget);
  extent_ = extent;
  width_ = width;
  height_ = height;
  columns_ = desc.Width;
  rows_ = desc.Height;
  front_ = 0;
  // Started contacts belong to the old grid. Keep inputs still awaiting their first step.
  const auto end = contacts_.begin() + contactCount_;
  const auto pending = std::remove_if(contacts_.begin(), end,
                                      [](const Contact &contact) { return contact.age > 0; });
  contactCount_ = static_cast<unsigned>(pending - contacts_.begin());
  pendingImpact_ = contactCount_ > 0;
  remaining_ = 0;
  needsClear_ = true;
  accumulator_ = 0;
  return true;
}

void WaterSurface::configure(const PackAppearance &appearance) noexcept {
  intensity_ = appearance.intensity;
  speed_ = appearance.speed;
  size_ = appearance.scale;
  damping_ = appearance.waterSurface.damping;
  ambient_ = appearance.waterSurface.ambient;
  minStrength_ = std::min(appearance.waterSurface.impactMin, appearance.waterSurface.impactMax);
  maxStrength_ = std::max(appearance.waterSurface.impactMin, appearance.waterSurface.impactMax);
  crestBrightness_ = appearance.brightness;
  backdrop_ = appearance.waterSurface.backdrop;
  waterHue_ = LayerColor::make(appearance.backgroundHue, 1);
  crestColor_ = LayerColor::make(appearance.hue, appearance.saturation, appearance.color);
  backgroundBrightness_ = appearance.backgroundBrightness;
  backgroundSaturation_ = appearance.backgroundSaturation;
  texture_ = appearance.waterSurface.texture;
  caustics_ = appearance.waterSurface.caustics;
  for (unsigned i = 0; i < tint_.size(); ++i) {
    const float code = appearance.backgroundColor[i] / 255.f;
    tint_[i] = code <= .04045f ? code / 12.92f : std::pow((code + .055f) / 1.055f, 2.4f);
  }
}

bool WaterSurface::push(Position point) noexcept {
  if (intensity_ <= 0 || maxStrength_ <= 0 || !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
      point[0] < 0 || point[0] > 1 || point[1] < 0 || point[1] > 1)
    return false;
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  const float sample = static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
  const float force = intensity_ * (minStrength_ + (maxStrength_ - minStrength_) * sample);
  const float radius = size_ * (.026f + .017f * force + .006f * std::sqrt(force));
  const float roundness = smoothUnit((force - .38f) / 1.82f);
  if (contactCount_ == MaxImpacts) {
    std::move(contacts_.begin() + 1, contacts_.end(), contacts_.begin());
    --contactCount_;
  }
  contacts_[contactCount_++] = {point, radius, force, roundness, 0};
  pendingImpact_ = true;
  return true;
}

bool WaterSurface::hasMotion() const noexcept {
  return hasWaves() || (ambient_ > 0 && backgroundBrightness_ > 0 && (texture_ > 0 || caustics_ > 0));
}

void WaterSurface::clear() noexcept {
  contactCount_ = 0;
  pendingImpact_ = false;
  needsClear_ = true;
  accumulator_ = 0;
  remaining_ = 0;
}

unsigned WaterSurface::buildImpulses(float elapsed) noexcept {
  unsigned count = 0, active = 0;
  for (unsigned i = 0; i < contactCount_; ++i) {
    auto contact = contacts_[i];
    const float start = contact.age, end = start + elapsed;
    const float force = contact.force, roundness = contact.roundness;
    const float compressionTime = .025f + .095f * roundness;
    const float reboundTime = .028f + .10f * roundness;
    // Integrate the two impulse envelopes over this frame's simulated time.
    const float compression = smoothUnit(end / compressionTime) - smoothUnit(start / compressionTime);
    const float rebound = smoothUnit((end - compressionTime) / reboundTime) -
                          smoothUnit((start - compressionTime) / reboundTime);
    float depression = 0;
    if (start == 0) {
      depression = force * (.00345f + .0024f * force);
      const float settleTime = 1.1f + (.96f + 2.f * std::sqrt(force)) / damping_;
      remaining_ = std::max(remaining_, settleTime - elapsed);
    }
    impulses_[count++] = {contact.position, contact.radius, roundness, depression,
                          force * (.30f + .14f * force) * compression,
                          force * (.015f + .46f * roundness) * rebound, 0};
    if (end < compressionTime + reboundTime) {
      contact.age = end;
      contacts_[active++] = contact;
    }
  }
  contactCount_ = active;
  pendingImpact_ = false;
  return count;
}

void WaterSurface::stampImpacts(ID3D11DeviceContext *context, unsigned count) {
  ComPtr<ID3D11RenderTargetView> backBuffer;
  context->OMGetRenderTargets(1, &backBuffer, nullptr);
  const float zero[4]{};
  context->ClearRenderTargetView(pressureTarget_.Get(), zero);
  const D3D11_VIEWPORT grid{0, 0, static_cast<float>(columns_), static_cast<float>(rows_), 0, 1};
  auto *target = pressureTarget_.Get();
  context->OMSetRenderTargets(1, &target, nullptr);
  context->RSSetViewports(1, &grid);
  context->OMSetBlendState(additive_.Get(), nullptr, 0xffffffff);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  context->VSSetShader(impactVs_.Get(), nullptr, 0);
  auto *impacts = impactConstants_.Get();
  context->UpdateSubresource(impacts, 0, nullptr, impulses_.data(), 0, 0);
  context->VSSetConstantBuffers(1, 1, &impacts);
  context->PSSetShader(impactPs_.Get(), nullptr, 0);
  context->DrawInstanced(4, count, 0, 0);

  target = backBuffer.Get();
  const D3D11_VIEWPORT screen{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
  context->OMSetRenderTargets(1, &target, nullptr);
  context->RSSetViewports(1, &screen);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  impacts = nullptr;
  context->VSSetConstantBuffers(1, 1, &impacts);
}

void WaterSurface::advanceWaves(ID3D11DeviceContext *context, unsigned steps, bool inject) {
  if (needsClear_) {
    const float zero[4]{};
    context->ClearUnorderedAccessViewFloat(wave_[front_].output.Get(), zero);
    needsClear_ = false;
  }
  const unsigned groupsX = (columns_ + 15) / 16, groupsY = (rows_ + 7) / 8;
  ID3D11UnorderedAccessView *nullOutput = nullptr;
  auto *frame = frameConstants_.Get();
  context->CSSetConstantBuffers(0, 1, &frame);
  context->CSSetShader(waveCs_.Get(), nullptr, 0);
  for (unsigned i = 0; i < steps; ++i) {
    ID3D11ShaderResourceView *inputs[] = {wave_[front_].view.Get(),
                                         i == 0 && inject ? pressure_.Get() : nullptr};
    front_ ^= 1;
    auto *output = wave_[front_].output.Get();
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch(groupsX, groupsY, 1);
    context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
  }
  ID3D11ShaderResourceView *inputs[] = {wave_[front_].view.Get(), nullptr};
  auto *slope = slope_.output.Get();
  context->CSSetShader(slopeCs_.Get(), nullptr, 0);
  context->CSSetShaderResources(0, 2, inputs);
  context->CSSetUnorderedAccessViews(1, 1, &slope, nullptr);
  context->Dispatch(groupsX, groupsY, 1);
  context->CSSetUnorderedAccessViews(1, 1, &nullOutput, nullptr);
  inputs[0] = nullptr;
  context->CSSetShaderResources(0, 2, inputs);
  frame = nullptr;
  context->CSSetConstantBuffers(0, 1, &frame);
  context->CSSetShader(nullptr, nullptr, 0);
}

bool WaterSurface::draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
                        bool paused, std::string &error) {
  if (width != width_ || height != height_) {
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!resize(device.Get(), width, height, error))
      return false;
  }
  const double elapsed = lastFrame_ < 0 || paused || wasPaused_
                             ? 0
                             : std::clamp(now - lastFrame_, 0.0, MaxSteps * Step);
  lastFrame_ = now;
  wasPaused_ = paused;
  // The optical drifts and swell complete whole cycles over this period.
  phase_ = std::fmod(phase_ + elapsed * ambient_, 1000.0);

  unsigned steps = 0, impulses = 0;
  if (!paused && hasWaves()) {
    accumulator_ = std::min(accumulator_ + elapsed, MaxSteps * Step);
    if (pendingImpact_)
      // A click arriving in an idle frame still reaches the very next present.
      accumulator_ = std::max(accumulator_, Step);
    steps = std::min(static_cast<unsigned>((accumulator_ + 1e-9) / Step), MaxSteps);
    accumulator_ = std::max(0.0, accumulator_ - steps * Step);
    if (steps) {
      const auto simulatedTime = static_cast<float>(steps * Step);
      remaining_ = std::max(0.f, remaining_ - simulatedTime);
      impulses = buildImpulses(simulatedTime);
      if (!hasWaves()) {
        clear();
        steps = 0;
      }
    }
  }
  const float visibility = smoothUnit(remaining_ / .62f);
  const float cellX = extent_[0] / columns_, cellY = extent_[1] / rows_;
  // Keep the explicit wave solver stable at all supported speeds and sizes.
  const float cflLimit = .67f * std::min(cellX, cellY) / static_cast<float>(Step);
  const float velocity = std::min(.47f * speed_, cflLimit);
  const FrameConstants data{extent_[0], extent_[1], 1 / extent_[0], 1 / extent_[1],
                            cellX, cellY, 1 / cellX, 1 / cellY,
                            static_cast<float>(Step), velocity * velocity, damping_, visibility,
                            static_cast<float>(phase_), backgroundBrightness_, crestBrightness_,
                            static_cast<float>(backdrop_), tint_[0], tint_[1], tint_[2],
                            backgroundSaturation_, texture_, caustics_, {}, waterHue_, crestColor_};
  auto *frame = frameConstants_.Get();
  context->UpdateSubresource(frame, 0, nullptr, &data, 0, 0);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->RSSetState(raster_.Get());
  context->OMSetDepthStencilState(nullptr, 0);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->IASetInputLayout(nullptr);
  if (impulses) {
    context->VSSetConstantBuffers(0, 1, &frame);
    stampImpacts(context, impulses);
  }
  if (steps)
    advanceWaves(context, steps, impulses > 0);

  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  context->PSSetShader(surfacePs_.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &frame);
  ID3D11ShaderResourceView *views[] = {visibility > 0 ? slope_.view.Get() : nullptr, optics_.Get()};
  ID3D11SamplerState *samplers[] = {clampSampler_.Get(), wrapSampler_.Get()};
  context->PSSetShaderResources(2, 2, views);
  context->PSSetSamplers(0, 2, samplers);
  context->Draw(3, 0);
  views[0] = views[1] = nullptr;
  context->PSSetShaderResources(2, 2, views);
  frame = nullptr;
  context->PSSetConstantBuffers(0, 1, &frame);
  if (impulses)
    context->VSSetConstantBuffers(0, 1, &frame);
  return true;
}
} // namespace action75
