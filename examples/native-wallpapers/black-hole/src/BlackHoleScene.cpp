#include "BlackHoleScene.h"
#include "BlackHoleDx11.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace action75 {
namespace {
constexpr unsigned SkyWidth = 2048, SkyHeight = 768, NoiseSize = 128;
constexpr double SkyCycleSeconds = 16;
constexpr double FullTurn = 6.283185307179586;
constexpr float ReferenceHeight = 720;
constexpr float MinStarRadius = 32, MaxStarRadius = 146;
constexpr float ShadowFraction = .149f;
constexpr float HeatTransferSeconds = .080f, CoolingSeconds = .54f;
constexpr float SinglePressPeak = .612f;
constexpr double StarLifetime = 2.7;
constexpr double MaxSecondaryDelay = .104;
constexpr double PulseHistoryInterval = .008;

struct DiskColors {
  std::array<float, 4> outer, middle, inner;
};

struct SceneConstants {
  float width, height, centerX, centerY;
  float shadowPx, lensStrength, diskBrightness, orbitPhase;
  float skyWidth, skyHeight, starFlare, hasEmissionMaps;
  float bloomTexelX, bloomTexelY, einsteinRadiusSquared, padding;
  DiskColors disk;
  float skyDriftX, skyDriftY, skyPhase, skyBrightness;
  std::array<float, 3> skyTint;
  std::uint32_t skyMode;
  float skyHue, skySaturation, starHue, starSaturation;
  std::array<float,3> starTint;
  float starGradeEnabled;
};
struct StarConstants {
  float x, y, radius, rotation;
  float red, green, blue, envelope;
  float age, seed, temperature, lensMix;
  float echoRed, echoGreen, echoBlue, echoEnvelope;
  float echoRadius, padding[3];
};
static_assert(sizeof(SceneConstants) == 176 && sizeof(StarConstants) == 80);

constexpr std::array<std::array<float, 3>, 7> StarColors{{
    {1.f, .36f, .10f}, {.43f, .65f, 1.f}, {1.f, .82f, .53f}, {.27f, .49f, 1.f},
    {1.f, .61f, .30f}, {1.f, .20f, .054f}, {.73f, .87f, 1.f},
}};

constexpr DiskColors diskColors(BlackHoleAppearance::DiskPalette palette) noexcept {
  using Palette = BlackHoleAppearance::DiskPalette;
  switch (palette) {
  case Palette::Hot:
    return {{.36f, .31f, .26f}, {.51f, .65f, .93f}, {.94f, 1.18f, 1.48f}};
  case Palette::Radio:
    return {{.34f, .044f, .008f}, {1.22f, .235f, .021f}, {1.62f, .85f, .19f}};
  case Palette::Warm:
  default:
    return {{.48f, .105f, .026f}, {1.02f, .51f, .19f}, {1.48f, 1.34f, 1.13f}};
  }
}

bool check(HRESULT result, const char *operation, std::string &error) {
  if (SUCCEEDED(result))
    return true;
  std::ostringstream message;
  message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
  error = message.str();
  return false;
}

void bindRaster(ID3D11DeviceContext *context, ID3D11RasterizerState *raster) {
  context->RSSetState(raster);
  context->OMSetDepthStencilState(nullptr, 0);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
}

float smooth(float lo, float hi, float value) noexcept {
  const float t = std::clamp((value - lo) / (hi - lo), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

float linearChannel(std::uint8_t channel) noexcept {
  const float value = channel / 255.f;
  return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}

float luminousPower(float temperature) noexcept {
  // T^4 starts gently. The shoulder bounds repeated energy deposits.
  const float squared = temperature * temperature;
  const float power = squared * squared;
  return 1.65f * power / (.65f + power);
}

double secondaryDelay(float sourceRadius, float einsteinRadiusSquared) noexcept {
  const double y = sourceRadius / std::sqrt(einsteinRadiusSquared);
  // Fermat arrival-time difference for the two point-lens paths. Replace the
  // astronomical time unit by seconds appropriate to a keystroke animation.
  const double difference = .5 * y * std::sqrt(y * y + 4) + 2 * std::asinh(.5 * y);
  return std::min(MaxSecondaryDelay, .042 * difference);
}

} // namespace

bool BlackHoleScene::load(ID3D11Device *device, ID3D11DeviceContext *context,
                         ID3D11ShaderResourceView *artwork, std::string &error) {
  const auto vertex = [&](auto &slot, const auto &bytes, const char *name) {
    return check(device->CreateVertexShader(bytes, sizeof(bytes), nullptr, slot.GetAddressOf()), name, error);
  };
  const auto pixel = [&](auto &slot, const auto &bytes, const char *name) {
    return check(device->CreatePixelShader(bytes, sizeof(bytes), nullptr, slot.GetAddressOf()), name, error);
  };
  using namespace black_hole_dx11;
  ComPtr<ID3D11PixelShader> bakeNebula, bakeStars;
  if (!vertex(screenVs_, ScreenVs, "ScreenVs") || !pixel(scenePs_, ScenePs, "ScenePs") ||
      !vertex(starVs_, StarVs, "StarVs") || !pixel(starPs_, StarPs, "StarPs") ||
      !pixel(lightSourcePs_, LightSourcePs, "LightSourcePs") ||
      !pixel(bloomHorizontal_, BloomHorizontalPs, "BloomHorizontalPs") ||
      !pixel(bloomVertical_, BloomVerticalPs, "BloomVerticalPs") ||
      !pixel(composite_, CompositePs, "CompositePs") || !pixel(bakeNebula, BakeArtworkPs, "BakeArtworkPs") ||
      !pixel(bakeStars, BakeStarsPs, "BakeStarsPs"))
    return false;

  constexpr UINT hdrSupport = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                              D3D11_FORMAT_SUPPORT_BLENDABLE;
  UINT supported = 0;
  if (SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R11G11B10_FLOAT, &supported)) &&
      (supported & hdrSupport) == hdrSupport)
    hdrFormat_ = DXGI_FORMAT_R11G11B10_FLOAT;

  D3D11_BUFFER_DESC buffer{};
  buffer.ByteWidth = sizeof(SceneConstants);
  buffer.Usage = D3D11_USAGE_DYNAMIC;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (!check(device->CreateBuffer(&buffer, nullptr, &sceneConstants_), "Black hole constants", error))
    return false;
  buffer.ByteWidth = sizeof(StarConstants) * MaxStars;
  if (!check(device->CreateBuffer(&buffer, nullptr, &starConstants_), "Star instances", error))
    return false;

  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                           D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                           D3D11_COLOR_WRITE_ENABLE_ALL};
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (!check(device->CreateRasterizerState(&raster, &raster_), "Black hole rasterizer", error) ||
      !check(device->CreateBlendState(&blend, &starBlend_), "Star blending", error) ||
      !check(device->CreateSamplerState(&sampler, &skySampler_), "Sky sampler", error))
    return false;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  if (!check(device->CreateSamplerState(&sampler, &turbulenceSampler_), "Disk sampler", error))
    return false;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
  if (!check(device->CreateSamplerState(&sampler, &emissionSampler_), "Emission map sampler", error))
    return false;

  std::array<std::uint8_t, NoiseSize * NoiseSize * 2> noise{};
  std::uint32_t seed = 0xc41c64ad;
  for (auto &sample : noise) {
    seed = seed * 1664525u + 1013904223u;
    sample = static_cast<std::uint8_t>(seed >> 24);
  }
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = texture.Height = NoiseSize;
  texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
  texture.Format = DXGI_FORMAT_R8G8_UNORM;
  texture.Usage = D3D11_USAGE_IMMUTABLE;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA noiseData{noise.data(), 2 * NoiseSize, 0};
  ComPtr<ID3D11Texture2D> noiseTexture;
  if (!check(device->CreateTexture2D(&texture, &noiseData, &noiseTexture), "Disk turbulence", error) ||
      !check(device->CreateShaderResourceView(noiseTexture.Get(), nullptr, &turbulence_),
             "Disk turbulence view", error))
    return false;

  texture.Width = SkyWidth;
  texture.Height = SkyHeight;
  texture.MipLevels = 0;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  const D3D11_VIEWPORT viewport{0, 0, SkyWidth, SkyHeight, 0, 1};
  bindRaster(context, raster_.Get());
  context->RSSetViewports(1, &viewport);
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  const auto bake = [&](DXGI_FORMAT format, ID3D11PixelShader *shader, ID3D11ShaderResourceView **view) {
    texture.Format = format;
    ComPtr<ID3D11Texture2D> plate;
    ComPtr<ID3D11RenderTargetView> target;
    if (!check(device->CreateTexture2D(&texture, nullptr, &plate), "Sky texture", error) ||
        !check(device->CreateRenderTargetView(plate.Get(), nullptr, &target), "Sky target", error) ||
        !check(device->CreateShaderResourceView(plate.Get(), nullptr, view), "Sky view", error))
      return false;
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->PSSetShader(shader, nullptr, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->GenerateMips(*view);
    return true;
  };
  context->PSSetShaderResources(2, 1, &artwork);
  auto *skySampler = skySampler_.Get();
  context->PSSetSamplers(0, 1, &skySampler);
  const bool baked = bake(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, bakeNebula.Get(), sky_.GetAddressOf());
  ID3D11ShaderResourceView *empty = nullptr;
  context->PSSetShaderResources(2, 1, &empty);

  constexpr UINT mapSupport = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                              D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
  supported = 0;
  const auto starFormat = SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R8G8_UNORM, &supported)) &&
                                  (supported & mapSupport) == mapSupport
                              ? DXGI_FORMAT_R8G8_UNORM
                              : DXGI_FORMAT_R8G8B8A8_UNORM;
  return baked && bake(starFormat, bakeStars.Get(), skyStars_.GetAddressOf());
}

bool BlackHoleScene::resize(ID3D11DeviceContext *context, unsigned width, unsigned height,
                           std::string &error) {
  if (width == width_ && height == height_)
    return true;
  ComPtr<ID3D11Device> device;
  context->GetDevice(&device);
  const auto create = [&](unsigned w, unsigned h, Surface &surface) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = hdrFormat_;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    return check(device->CreateTexture2D(&desc, nullptr, &texture), "Black hole HDR surface", error) &&
           check(device->CreateRenderTargetView(texture.Get(), nullptr, &surface.target),
                 "Black hole HDR target", error) &&
           check(device->CreateShaderResourceView(texture.Get(), nullptr, &surface.view),
                 "Black hole HDR view", error);
  };
  const unsigned blurWidth = (width + 3) / 4, blurHeight = (height + 3) / 4;
  Surface image, horizontal, vertical;
  if (!create(width, height, image) || !create(blurWidth, blurHeight, horizontal) ||
      !create(blurWidth, blurHeight, vertical))
    return false;
  hdr_ = std::move(image);
  bloom_[0] = std::move(horizontal);
  bloom_[1] = std::move(vertical);
  width_ = width;
  height_ = height;
  bloomWidth_ = blurWidth;
  bloomHeight_ = blurHeight;
  return true;
}

void BlackHoleScene::configure(const BlackHoleAppearance &profile, float starSize, float starBrightness, bool updateCenter) {
  const Position previous = center();
  appearance_ = profile;
  setCenter(updateCenter ? appearance_.center : previous);
  starSize_ = starSize;
  starBrightness_ = starBrightness;
}

void BlackHoleScene::setCenter(Position point) noexcept {
  for (auto &axis : point)
    axis = std::clamp(axis, 0.f, 1.f);
  appearance_.center = point;
}

float BlackHoleScene::shadowPixels(unsigned width, unsigned height) const noexcept {
  return ShadowFraction * appearance_.size * static_cast<float>(std::min(width, height));
}

void BlackHoleScene::HeatState::decay(float factor) noexcept {
  weight *= factor;
  for (auto &channel : spectrum)
    channel *= factor;
}

void BlackHoleScene::HeatState::deposit(float amount, const std::array<float, 3> &color) noexcept {
  weight += amount;
  for (std::size_t channel = 0; channel < spectrum.size(); ++channel)
    spectrum[channel] += amount * color[channel];
}

void BlackHoleScene::Star::addHeat(double now, const std::array<float, 3> &color) noexcept {
  if (pulse.retained.weight > 0 && now > pulse.time) {
    const auto last = (nextHistory + history.size() - 1) % history.size();
    // Keep enough history for the longer optical path, even when an entire
    // synthetic burst of repeats reaches the same key within one frame.
    if (!historyCount || pulse.time - history[last].time >= PulseHistoryInterval) {
      history[nextHistory] = pulse;
      nextHistory = (nextHistory + 1) % history.size();
      historyCount = std::min(history.size(), historyCount + 1);
    }
  }
  const float elapsed = static_cast<float>(std::max(0.0, now - pulse.time));
  pulse.fast.decay(std::exp(-elapsed / HeatTransferSeconds));
  pulse.retained.decay(std::exp(-elapsed / CoolingSeconds));

  // The difference of the two decays is the impulse response of a heated
  // reservoir. Equal deposits preserve temperature and color on a repeat.
  const float amount = std::clamp(2.1f - pulse.retained.weight, 0.f, 1.f);
  pulse.fast.deposit(amount, color);
  pulse.retained.deposit(amount, color);
  pulse.time = now;
}

BlackHoleScene::HeatState BlackHoleScene::PulseState::emission(double now) const noexcept {
  const float elapsed = static_cast<float>(now - time);
  const float cooled = std::exp(-elapsed / CoolingSeconds);
  const float transferred = std::exp(-elapsed / HeatTransferSeconds);
  HeatState result{retained.weight * cooled - fast.weight * transferred};
  for (std::size_t channel = 0; channel < result.spectrum.size(); ++channel)
    result.spectrum[channel] = retained.spectrum[channel] * cooled - fast.spectrum[channel] * transferred;
  return result;
}

BlackHoleScene::HeatState BlackHoleScene::Star::emission(double now) const noexcept {
  if (now >= pulse.time)
    return pulse.emission(now);
  for (std::size_t offset = 0; offset < historyCount; ++offset) {
    const auto index = (nextHistory + history.size() - 1 - offset) % history.size();
    if (history[index].time <= now)
      return history[index].emission(now);
  }
  return {};
}

bool BlackHoleScene::Star::expired(double now) const noexcept {
  return now - pulse.time >= StarLifetime + MaxSecondaryDelay;
}

bool BlackHoleScene::covers(Position point, unsigned width, unsigned height) const noexcept {
  const auto position = center();
  const float x = (point[0] - position[0]) * static_cast<float>(width);
  const float y = (point[1] - position[1]) * static_cast<float>(height);
  const float radius = shadowPixels(width, height);
  return x * x + y * y <= radius * radius;
}

std::uint32_t BlackHoleScene::random() noexcept {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

float BlackHoleScene::randomUnit() noexcept {
  return static_cast<float>(random() >> 8) / 16777216.f;
}

bool BlackHoleScene::push(double now, Position point, unsigned width, unsigned height) noexcept {
  if (!width || !height || starBrightness_ <= 0 ||
      !std::all_of(point.begin(), point.end(), [](float x) { return x >= 0.f && x <= 1.f; }) ||
      covers(point, width, height))
    return false;

  // A held or repeated chord uses one pulse state and one instance per key.
  const auto end = stars_.begin() + count_;
  auto slot = std::find_if(stars_.begin(), end, [&](const Star &star) { return star.point == point; });
  if (slot == end || slot->expired(now)) {
    if (slot == end) {
      if (count_ < MaxStars)
        ++count_;
      else
        slot = std::min_element(stars_.begin(), end,
                                [](const Star &a, const Star &b) { return a.pulse.time < b.pulse.time; });
    }
    // Bias the distribution toward small stars, with a few larger ones.
    // Retriggers deposit heat in the same source without changing its diameter.
    const float roll = randomUnit();
    const float radius = MinStarRadius + (MaxStarRadius - MinStarRadius) * roll * roll;
    *slot = Star{point, now, radius, .64f * (randomUnit() - .5f), randomUnit(), {now}};
  }
  lastColor_ = (lastColor_ + 1 + random() % (StarColors.size() - 1)) % StarColors.size();
  slot->addHeat(now, StarColors[lastColor_]);
  return true;
}

bool BlackHoleScene::draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
                         bool paused, std::string &error) {
  if (!width || !height)
    return true;
  if (!resize(context, width, height, error))
    return false;
  const auto skyMode = static_cast<unsigned>(appearance_.sky);
  constexpr BlackHoleSkyStyle solid{1,0};
  const BlackHoleSkyStyle *styles[] = {&appearance_.nebula, &appearance_.voidSky, &appearance_.starfield, &solid};
  const auto &style = *styles[skyMode];
  double &skyPhase = skyPhase_[skyMode];
  if (previousFrame_ >= 0 && !paused && !wasPaused_) {
    const double elapsed = std::clamp(now - previousFrame_, 0.0, .10);
    phase_ = std::fmod(phase_ + .067 * elapsed, 1.0);
    skyPhase = std::fmod(skyPhase + style.speed * elapsed / SkyCycleSeconds, 1.0);
  }
  previousFrame_ = now;
  wasPaused_ = paused;
  const double angle = FullTurn * skyPhase;

  const float canvasWidth = static_cast<float>(width), canvasHeight = static_cast<float>(height);
  const float skyScale = std::max(canvasWidth / SkyWidth, canvasHeight / SkyHeight);
  const float starScale = starSize_ * std::min(canvasWidth, canvasHeight) / ReferenceHeight;
  const float bloomWidth = static_cast<float>(bloomWidth_);
  const float bloomHeight = static_cast<float>(bloomHeight_);
  const auto position = center();
  const float einsteinRadiusSquared = 2.92f * appearance_.lensStrength;
  const float lensEnabled = smooth(0, .42f, appearance_.lensStrength);
  SceneConstants scene{canvasWidth, canvasHeight, position[0] * canvasWidth,
                       position[1] * canvasHeight, shadowPixels(width, height),
                       appearance_.lensStrength, appearance_.diskBrightness, static_cast<float>(phase_),
                       SkyWidth * skyScale, SkyHeight * skyScale, appearance_.starFlare, 0,
                       1.f / bloomWidth, 1.f / bloomHeight, einsteinRadiusSquared, 0,
                       diskColors(appearance_.diskPalette),
                       .034f * static_cast<float>(std::sin(angle)), .044f * static_cast<float>(std::sin(2 * angle)),
                       static_cast<float>(angle), style.brightness * appearance_.skyBrightness,
                       {linearChannel(style.tint[0]), linearChannel(style.tint[1]), linearChannel(style.tint[2])},
                       skyMode, appearance_.skyHue / 360.f, appearance_.skySaturation,
                       appearance_.starHue / 360.f, appearance_.starSaturation,
                       {linearChannel(appearance_.starTint[0]), linearChannel(appearance_.starTint[1]),
                        linearChannel(appearance_.starTint[2])},
                       (appearance_.starHue != 0 || appearance_.starSaturation != 1 ||
                        appearance_.starTint != EffectRgb{255,255,255}) ? 1.f : 0.f};
  for (unsigned i=0;i<3;++i) scene.skyTint[i] *= linearChannel(appearance_.skyTint[i]);
  D3D11_MAPPED_SUBRESOURCE mapped{};

  const auto end = std::remove_if(stars_.begin(), stars_.begin() + count_, [&](const Star &star) {
    return star.expired(now);
  });
  count_ = static_cast<std::size_t>(end - stars_.begin());
  UINT visibleStars = 0;
  if (!paused && count_ && starBrightness_ > 0) {
    if (!check(context->Map(starConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
               "Map star instances", error))
      return false;
    auto *instances = static_cast<StarConstants *>(mapped.pData);
    for (std::size_t index = 0; index < count_; ++index) {
      const auto &star = stars_[index];
      const float x = star.point[0] * canvasWidth, y = star.point[1] * canvasHeight;
      const float dx = x - scene.centerX, dy = y - scene.centerY;
      const float distanceSquared = dx * dx + dy * dy;
      if (distanceSquared <= scene.shadowPx * scene.shadowPx)
        continue;
      const auto heat = star.emission(now);
      const float temperature = std::max(0.f, heat.weight) / SinglePressPeak;
      const float brightness = starBrightness_ * (.65f + .45f * smooth(MinStarRadius, MaxStarRadius, star.radius));
      const float radiance = brightness * luminousPower(temperature);
      const float sourceRadius = std::sqrt(distanceSquared) / scene.shadowPx;
      const float lensMix = lensEnabled * (1.f - smooth(1.9f, 3.15f, sourceRadius));
      const bool hasSecondary = lensMix > 0 && einsteinRadiusSquared > 1.f;
      const double delay = hasSecondary ? secondaryDelay(sourceRadius, einsteinRadiusSquared) : 0;
      const auto echo = hasSecondary ? star.emission(now - delay) : HeatState{};
      const float echoRadiance = brightness * luminousPower(std::max(0.f, echo.weight) / SinglePressPeak);
      if (radiance < .00012f && echoRadiance < .00012f)
        continue;

      const float age = static_cast<float>(std::max(0.0, now - star.born));
      const float inverseWeight = 1.f / std::max(heat.weight, 1e-7f);
      const float inverseEchoWeight = 1.f / std::max(echo.weight, 1e-7f);
      const float size = star.radius * starScale;
      const auto extent = [size](float time) {
        return size * (1.04f - .13f * std::exp2(-time * 2.8f));
      };
      const float radius = extent(age);
      const float echoRadius = extent(std::max(0.f, age - static_cast<float>(delay)));
      instances[visibleStars++] = {x, y, radius, star.rotation,
                                  heat.spectrum[0] * inverseWeight, heat.spectrum[1] * inverseWeight,
                                  heat.spectrum[2] * inverseWeight, radiance,
                                  age, star.seed, std::min(temperature, 1.4f), lensMix,
                                  echo.spectrum[0] * inverseEchoWeight, echo.spectrum[1] * inverseEchoWeight,
                                  echo.spectrum[2] * inverseEchoWeight, echoRadiance, echoRadius, {}};
      if (lensMix > 0)
        scene.hasEmissionMaps = 1;
    }
    context->Unmap(starConstants_.Get(), 0);
  }
  if (!check(context->Map(sceneConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
             "Map black hole constants", error))
    return false;
  std::memcpy(mapped.pData, &scene, sizeof(scene));
  context->Unmap(sceneConstants_.Get(), 0);

  const D3D11_VIEWPORT full{0, 0, canvasWidth, canvasHeight, 0, 1};
  const D3D11_VIEWPORT reduced{0, 0, bloomWidth, bloomHeight, 0, 1};
  ComPtr<ID3D11RenderTargetView> output;
  context->OMGetRenderTargets(1, &output, nullptr);
  const auto pass = [&](ID3D11RenderTargetView *target, const D3D11_VIEWPORT &viewport,
                        ID3D11PixelShader *shader) {
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->PSSetShader(shader, nullptr, 0);
  };

  bindRaster(context, raster_.Get());
  ID3D11Buffer *constants = sceneConstants_.Get();
  ID3D11ShaderResourceView *skyViews[] = {sky_.Get(), turbulence_.Get()};
  ID3D11SamplerState *samplers[] = {skySampler_.Get(), turbulenceSampler_.Get(), emissionSampler_.Get()};
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &constants);
  context->PSSetShaderResources(0, 2, skyViews);
  auto *starFieldView = skyStars_.Get();
  context->PSSetShaderResources(4, 1, &starFieldView);
  context->PSSetSamplers(0, 3, samplers);
  const auto drawStars = [&](ID3D11PixelShader *shader) {
    ID3D11Buffer *starBuffers[] = {constants, starConstants_.Get()};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(starVs_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 2, starBuffers);
    context->PSSetShader(shader, nullptr, 0);
    context->OMSetBlendState(starBlend_.Get(), nullptr, 0xffffffff);
    context->DrawInstanced(4, visibleStars, 0, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(screenVs_.Get(), nullptr, 0);
    ID3D11Buffer *empty[] = {nullptr, nullptr};
    context->VSSetConstantBuffers(0, 2, empty);
  };
  if (scene.hasEmissionMaps > 0) {
    // The bloom buffers are scratch surfaces at this stage. Store the light
    // seen by the short and long paths before reusing them for the bloom passes.
    ID3D11RenderTargetView *maps[] = {bloom_[0].target.Get(), bloom_[1].target.Get()};
    const float black[4]{};
    context->ClearRenderTargetView(maps[0], black);
    context->ClearRenderTargetView(maps[1], black);
    context->OMSetRenderTargets(2, maps, nullptr);
    context->RSSetViewports(1, &reduced);
    drawStars(lightSourcePs_.Get());
  }
  pass(hdr_.target.Get(), full, scenePs_.Get());
  if (scene.hasEmissionMaps > 0) {
    ID3D11ShaderResourceView *emissions[] = {bloom_[0].view.Get(), bloom_[1].view.Get()};
    context->PSSetShaderResources(2, 2, emissions);
  }
  context->Draw(3, 0);
  ID3D11ShaderResourceView *emptyEmissions[2]{};
  context->PSSetShaderResources(2, 2, emptyEmissions);
  if (visibleStars)
    drawStars(starPs_.Get());

  // The first blur also extracts highlights and downsamples to a quarter of the output size.
  pass(bloom_[0].target.Get(), reduced, bloomHorizontal_.Get());
  auto *source = hdr_.view.Get();
  context->PSSetShaderResources(2, 1, &source);
  context->Draw(3, 0);
  pass(bloom_[1].target.Get(), reduced, bloomVertical_.Get());
  source = bloom_[0].view.Get();
  context->PSSetShaderResources(2, 1, &source);
  context->Draw(3, 0);
  pass(output.Get(), full, composite_.Get());
  ID3D11ShaderResourceView *finish[] = {hdr_.view.Get(), bloom_[1].view.Get()};
  context->PSSetShaderResources(2, 2, finish);
  context->Draw(3, 0);
  ID3D11ShaderResourceView *unbound[5]{};
  context->PSSetShaderResources(0, 5, unbound);
  ID3D11Buffer *empty = nullptr;
  context->PSSetConstantBuffers(0, 1, &empty);
  return true;
}
} // namespace action75
