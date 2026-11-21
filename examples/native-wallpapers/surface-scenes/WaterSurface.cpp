#include "WaterSurface.h"

#include "WaterScreenVertex.h"
#include "WaterWaterPixel.h"
#include "WaterImpactVertex.h"
#include "WaterImpactPixel.h"
#include "WaterStepWave.h"
#include "WaterBuildSlope.h"
#include "WaterBakeOptics.h"
#include "WaterDownsampleOptics.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace surface {
namespace {
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

} // namespace

WaterSurface::WaterSurface(ID3D12Device *device, unsigned width, unsigned height)
    : gpu_(device, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
      surface_(gpu_.graphics(shader(WaterScreenVertex), shader(WaterWaterPixel))),
      stamp_(gpu_.graphics(shader(WaterImpactVertex), shader(WaterImpactPixel),
                           DXGI_FORMAT_R16G16_FLOAT, Blend::Pressure)),
      step_(gpu_.compute(shader(WaterStepWave))), slopePipeline_(gpu_.compute(shader(WaterBuildSlope))),
      bake_(gpu_.compute(shader(WaterBakeOptics))), downsample_(gpu_.compute(shader(WaterDownsampleOptics))) {
  static_assert((1u << (opticsLevels - 1)) == opticsSize && opticsSize % 8 == 0);
  static_assert(sizeof(Impulse) == 32 && maxImpacts == 128);
  constexpr auto format = DXGI_FORMAT_R8G8B8A8_UNORM;
  gpu_.texture(optics_, opticsSize, opticsSize, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, opticsLevels);
  for (unsigned i = 0; i < inputMip_.size(); ++i) {
    inputMip_[i] = gpu_.srv(optics_.resource.Get(), format, i, 1);
    outputMip_[i] = gpu_.uav(optics_.resource.Get(), format, i + 1);
  }
  resize(width, height);
  rng_ = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
}

void WaterSurface::resize(unsigned width, unsigned height) {
  if (width == width_ && height == height_)
    return;
  const float shortSide = static_cast<float>(std::min(width, height));
  // The slight tilt foreshortens the surface vertically. Use almost square
  // cells in water space and bound work independently of display resolution.
  const Position extent{width / shortSide, 1.17f * height / shortSide};
  const float pitch = std::max({1.f / 164, 3.f / shortSide, std::max(extent[0], extent[1]) / 768});
  const auto gridSize = [pitch](float span) {
    return static_cast<unsigned>(std::clamp(std::lround(span / pitch), 4l, 768l));
  };
  const auto columns = gridSize(extent[0]), rows = gridSize(extent[1]);
  for (auto &wave : wave_)
    gpu_.texture(wave, columns, rows, DXGI_FORMAT_R32G32_FLOAT,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu_.texture(slope_, columns, rows, DXGI_FORMAT_R16G16B16A16_FLOAT,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu_.texture(pressure_, columns, rows, DXGI_FORMAT_R16G16_FLOAT,
               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
  extent_ = extent;
  width_ = width;
  height_ = height;
  columns_ = columns;
  rows_ = rows;
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
}

void WaterSurface::configure(const Settings &settings) {
  intensity_ = settings.scalar("strength");
  speed_ = settings.scalar("speed");
  size_ = settings.scalar("radius");
  damping_ = settings.scalar("damping");
  ambient_ = settings.scalar("ambient");
  const float low = settings.scalar("impactMin"), high = settings.scalar("impactMax");
  minStrength_ = std::min(low, high);
  maxStrength_ = std::max(low, high);
  crestBrightness_ = settings.scalar("crest");
  backdrop_ = settings.index("backdrop");
  waterHue_ = LayerColor::make(settings.scalar("backgroundHue"), 1);
  crestColor_ = LayerColor::make(settings.scalar("effectHue"), settings.scalar("effectSaturation"),
                                 settings.color("effectColor"));
  backgroundBrightness_ = settings.scalar("brightness");
  backgroundSaturation_ = settings.scalar("saturation");
  texture_ = settings.scalar("texture");
  caustics_ = settings.scalar("caustics");
  const auto tint = settings.color("waterColor");
  tint_ = {linear(tint[0]), linear(tint[1]), linear(tint[2])};
}

void WaterSurface::push(double, float x, float y) noexcept {
  const Position point{x, y};
  if (intensity_ <= 0 || maxStrength_ <= 0 || !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
      point[0] < 0 || point[0] > 1 || point[1] < 0 || point[1] > 1)
    return;
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  const float sample = static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
  const float force = intensity_ * (minStrength_ + (maxStrength_ - minStrength_) * sample);
  const float radius = size_ * (.026f + .017f * force + .006f * std::sqrt(force));
  const float roundness = smoothUnit((force - .38f) / 1.82f);
  if (contactCount_ == maxImpacts) {
    std::move(contacts_.begin() + 1, contacts_.end(), contacts_.begin());
    --contactCount_;
  }
  contacts_[contactCount_++] = {point, radius, force, roundness, 0};
  pendingImpact_ = true;
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

void WaterSurface::bakeOptics(ID3D12GraphicsCommandList *commands) {
  commands->SetPipelineState(bake_.Get());
  commands->SetComputeRootDescriptorTable(Gpu::uavSlot(2), optics_.uav.gpu);
  commands->Dispatch(opticsSize / 8, opticsSize / 8, 1);
  commands->SetPipelineState(downsample_.Get());
  for (unsigned level = 1; level < opticsLevels; ++level) {
    // Each pass samples exactly the preceding level, never the UAV being written.
    Gpu::transition(commands, optics_.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, level - 1);
    commands->SetComputeRootDescriptorTable(Gpu::srvSlot(3), inputMip_[level - 1].gpu);
    commands->SetComputeRootDescriptorTable(Gpu::uavSlot(2), outputMip_[level - 1].gpu);
    const auto pitch = ((opticsSize >> level) + 7) / 8;
    commands->Dispatch(pitch, pitch, 1);
  }
  for (unsigned level = 0; level < opticsLevels; ++level) {
    const auto prior = level + 1 == opticsLevels ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Gpu::transition(commands, optics_.resource.Get(), prior, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, level);
  }
  optics_.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  opticsReady_ = true;
}

void WaterSurface::stampImpacts(ID3D12GraphicsCommandList *commands, unsigned count) {
  Gpu::transition(commands, pressure_, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Gpu::target(commands, pressure_.rtv, columns_, rows_);
  const float zero[4]{};
  commands->ClearRenderTargetView(pressure_.rtv, zero, 0, nullptr);
  commands->SetPipelineState(stamp_.Get());
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commands->SetGraphicsRootConstantBufferView(1, gpu_.constantBytes(impulses_.data(), count * sizeof(Impulse)));
  commands->DrawInstanced(4, count, 0, 0);
}

void WaterSurface::advanceWaves(ID3D12GraphicsCommandList *commands, unsigned steps, bool inject) {
  if (needsClear_) {
    auto &current = wave_[front_];
    Gpu::transition(commands, current, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float zero[4]{};
    commands->ClearUnorderedAccessViewFloat(current.uav.gpu, current.uav.cpu, current.resource.Get(),
                                            zero, 0, nullptr);
    needsClear_ = false;
  }
  if (inject)
    Gpu::transition(commands, pressure_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const unsigned groupsX = (columns_ + 15) / 16, groupsY = (rows_ + 7) / 8;
  commands->SetPipelineState(step_.Get());
  for (unsigned i = 0; i < steps; ++i) {
    auto &input = wave_[front_];
    front_ ^= 1;
    auto &output = wave_[front_];
    Gpu::transition(commands, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Gpu::transition(commands, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->SetComputeRootDescriptorTable(Gpu::srvSlot(0), input.srv.gpu);
    commands->SetComputeRootDescriptorTable(Gpu::srvSlot(1), i == 0 && inject ? pressure_.srv.gpu : gpu_.nullSrv().gpu);
    commands->SetComputeRootDescriptorTable(Gpu::uavSlot(0), output.uav.gpu);
    commands->Dispatch(groupsX, groupsY, 1);
  }
  auto &current = wave_[front_];
  Gpu::transition(commands, current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Gpu::transition(commands, slope_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  commands->SetPipelineState(slopePipeline_.Get());
  commands->SetComputeRootDescriptorTable(Gpu::srvSlot(0), current.srv.gpu);
  commands->SetComputeRootDescriptorTable(Gpu::uavSlot(1), slope_.uav.gpu);
  commands->Dispatch(groupsX, groupsY, 1);
}

void WaterSurface::draw(const A75Frame12 &frame, bool paused) {
  resize(frame.width, frame.height);
  const double now = frame.seconds;
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
  auto *commands = frame.commands;
  gpu_.begin(commands);
  if (!opticsReady_)
    bakeOptics(commands);
  const auto constants = gpu_.constants(data);
  commands->SetGraphicsRootConstantBufferView(0, constants);
  if (impulses)
    stampImpacts(commands, impulses);
  if (steps) {
    commands->SetComputeRootConstantBufferView(0, constants);
    advanceWaves(commands, steps, impulses > 0);
  }

  if (visibility > 0)
    Gpu::transition(commands, slope_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Gpu::target(commands, frame.rtv, frame.width, frame.height);
  commands->SetPipelineState(surface_.Get());
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->SetGraphicsRootDescriptorTable(Gpu::srvSlot(2), visibility > 0 ? slope_.srv.gpu : gpu_.nullSrv().gpu);
  commands->SetGraphicsRootDescriptorTable(Gpu::srvSlot(3), optics_.srv.gpu);
  commands->DrawInstanced(3, 1, 0, 0);
}

} // namespace surface
