#include "FireworksScene.h"
#include "FireworksShaders.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace action75 {
namespace {
using Vec4 = std::array<float, 4>;
struct Vec3 {
  float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(Vec3 v, float f) { return {v.x * f, v.y * f, v.z * f}; }
Vec3 mix(Vec3 a, Vec3 b, float t) { return a * (1 - t) + b * t; }
Vec4 pack(Vec3 v, float w) { return {v.x, v.y, v.z, w}; }
Vec3 xyz(Vec4 v) { return {v[0], v[1], v[2]}; }
constexpr float pi = 3.14159265f;
constexpr float goldenAngle = 2.39996323f;
constexpr unsigned shapeCount = FireworksAppearance::shapeCount;
constexpr unsigned trailVertices = 42;
constexpr Vec3 amber{1, .36f, .066f}, gold{1, .54f, .135f}, copper{1, .24f, .055f};
constexpr Vec3 cream{1, .83f, .52f}, silver{.72f, .86f, 1}, cherry{1, .028f, .078f};
constexpr Vec3 rose{1, .09f, .42f}, lilac{.40f, .18f, 1}, blue{.075f, .25f, 1};
constexpr Vec3 ice{.115f, .64f, 1}, mint{.095f, 1, .58f}, jade{.025f, 1, .30f};
constexpr float heartContour = 3, heartEmber = 4;

struct Spark {
  Vec4 origin, direction, burn, light, early, late;
};
static_assert(sizeof(Spark) == 96);

float sample(std::uint32_t &state) noexcept {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<float>(state >> 8) / 16777216.0f;
}
struct Random {
  std::uint32_t state;
  float between(float lo, float hi) { return lo + (hi - lo) * sample(state); }
};
struct Style {
  Vec3 start, finish;
  float life, drag, trail, width, energy, gravity, change, twinkle;
  unsigned colorIndex;
};

// The sequence is well distributed even when rendering a prefix under load.
Vec3 sphere(unsigned index, float phase, float depth, Random &rng) {
  const float z = (2 * std::fmod(index * .754877666f + .318f, 1.f) - 1) * depth;
  const float angle = goldenAngle * index + phase + rng.between(-.07f, .07f);
  const float r = std::sqrt(1 - z * z);
  return {std::cos(angle) * r, std::sin(angle) * r, z};
}

Spark star(Random &rng, Vec3 origin, float delay, Vec3 direction, const Style &style,
           float kind = 0) {
  return {pack(origin, delay), pack(direction, style.gravity * rng.between(.74f, 1.27f)),
          {style.life * rng.between(.79f, 1.18f), style.drag * rng.between(.89f, 1.13f),
           style.trail * rng.between(.75f, 1.30f), style.width * rng.between(.72f, 1.3f)},
          {style.energy * rng.between(.48f, 1.37f), rng.between(0, 2 * pi), kind, style.twinkle},
          pack(style.start, style.change * rng.between(.82f, 1.24f)),
          pack(style.finish, static_cast<float>(style.colorIndex))};
}

struct ChildFlower {
  Vec3 position;
  float size, delay;
};
using Children = std::array<ChildFlower, 13>;

struct HeartContour {
  std::array<Vec3, 257> points{};
  std::array<float, 257> lengths{};

  HeartContour() {
    for (unsigned i = 0; i < points.size(); ++i) {
      const float t = 2 * pi * i / (points.size() - 1);
      const float x = std::sin(t);
      const float y = 13 * std::cos(t) - 5 * std::cos(2 * t) -
                      2 * std::cos(3 * t) - std::cos(4 * t);
      points[i] = {1.10f * x * x * x, -(y + 2.35f) / 16.5f, 0};
      if (i) {
        lengths[i] = lengths[i - 1] + std::hypot(points[i].x - points[i - 1].x,
                                                 points[i].y - points[i - 1].y);
      }
    }
  }

  Vec3 at(float fraction) const {
    const float distance = fraction * lengths.back();
    const auto end = std::lower_bound(lengths.begin() + 1, lengths.end(), distance);
    const auto i = static_cast<std::size_t>(end - lengths.begin());
    const float t = (distance - lengths[i - 1]) / (lengths[i] - lengths[i - 1]);
    return mix(points[i - 1], points[i], t);
  }
};

Spark heartSpark(const HeartContour &outline, unsigned index, Random &rng) {
  // Low-discrepancy sampling leaves both lobes and the tip visible at low particle counts.
  const float position = std::fmod(.034f + index * .618033989f, 1.f);
  const bool inner = index % 5 == 4;
  const auto point = outline.at(position) * (inner ? rng.between(.86f, .93f) : rng.between(.981f, 1.019f));
  const Vec3 tint = mix(cherry, rose, rng.between(.20f, .94f));
  auto spark = star(rng, {}, 0, point,
                    {mix(tint, cream, .27f), tint, inner ? 1.52f : 1.97f, 3.2f,
                     inner ? .073f : .145f, inner ? .93f : 1.43f, inner ? 1.12f : 2.12f,
                     0, .25f, inner ? .81f : .49f, inner ? 1u : (index % 6 == 0 ? 2u : 0u)},
                    inner ? heartEmber : heartContour);
  // Every point travels on the same expansion curve, including the first visible flash.
  spark.burn[1] = 3.2f;
  return spark;
}

Spark palmSpark(unsigned index, Random &rng) {
  constexpr Vec3 fronds[]{{-1.37f, -.38f, -.12f}, {-1.13f, -.85f, .06f},
                         {-.65f, -1.21f, -.14f}, {0, -1.42f, -.10f},
                         {.57f, -1.21f, .16f}, {1.15f, -.84f, -.03f},
                         {1.39f, -.36f, .10f}};
  const unsigned arm = (index * 3) % 8;
  if (arm == 7) {
    return star(rng, {}, rng.between(.005f, .072f),
                {rng.between(-.046f, .046f), rng.between(.21f, .53f), 0},
                {cream, amber, rng.between(1.29f, 2.0f), 2.3f, rng.between(.36f, .63f),
                 1.52f, 1.61f, .25f, .40f, .62f, 2});
  }
  const auto direction = fronds[arm] * rng.between(.89f, 1.13f) +
                         Vec3{rng.between(-.037f, .037f), rng.between(-.042f, .042f), 0};
  const bool leading = index < 32 || index % 5 == 0;
  return star(rng, {}, rng.between(0, .041f), direction,
              {mix(ice, cream, .43f), leading ? ice : silver, rng.between(1.44f, 2.30f),
               1.92f, rng.between(.49f, .86f), leading ? 2.02f : 1.15f,
               leading ? 2.2f : 1.34f, rng.between(.31f, .46f), .31f, .68f,
               leading ? 0u : 1u});
}

Spark primary(unsigned shape, unsigned index, Random &rng, const Children &children) {
  if (shape == 3)
    return palmSpark(index, rng);
  Vec3 direction = sphere(index, .18f, 1, rng) * rng.between(.87f, 1.08f);
  const float delay = rng.between(0, .033f);
  if (shape == 8 || shape == 9) {
    const float t = rng.between(.12f, 1.f);
    const bool wheel = shape == 8;
    const float angle = wheel ? (index % 5) * (2 * pi / 5) + t * 1.55f : index * goldenAngle;
    const float radius = wheel ? t : (.63f + .34f * std::cos(angle * 8)) * rng.between(.88f, 1.02f);
    direction = {radius * std::cos(angle), radius * std::sin(angle), rng.between(-.09f, .09f)};
    return star(rng, {}, delay, direction,
                {wheel ? cream : lilac, wheel ? amber : rose, 1.70f, 2.22f,
                 .15f, 1.61f, 1.70f, .10f, .29f, .50f, index % 3});
  }
  if (shape == 0) { // Kamuro
    direction.y = direction.y * .86f - .18f;
    return star(rng, {}, delay, direction,
                {mix(copper, gold, rng.between(.43f, .92f)), mix(gold, cream, rng.between(.52f, 1)),
                 rng.between(1.85f, 2.88f), rng.between(1.38f, 1.95f), rng.between(.70f, 1.12f),
                 1.40f, 1.77f, rng.between(.14f, .34f), .39f, .55f, index % 2});
  }
  if (shape == 1) { // Three pistils with staggered color changes
    const unsigned layer = index % 7 < 4 ? 0 : (index % 7 < 6 ? 1 : 2);
    constexpr float radii[]{1, .655f, .36f};
    constexpr Vec3 colors[]{cherry, lilac, mint};
    direction = sphere(index, .24f * layer, .72f, rng) *
                (radii[layer] * rng.between(.91f, 1.09f));
    return star(rng, {}, delay + .018f * layer, direction,
                {mix(gold, colors[layer], layer ? .39f : 0), colors[layer],
                 1.92f - .21f * layer, 2.21f, .63f - .12f * layer, 1.63f, 1.72f,
                 .15f, .46f - .08f * layer, .34f, layer});
  }
  if (shape == 2) { // Two-tone short-lived peony with warm pistil
    const bool core = index % 7 >= 5;
    const unsigned side = direction.x + .71f * direction.y + .24f * direction.z > 0 ? 1 : 0;
    const Vec3 tint = core ? cream : (side ? ice : mint);
    return star(rng, {}, delay, direction * (core ? .305f : 1),
                {mix(tint, cream, .14f), tint, rng.between(1.25f, 1.94f), 2.36f,
                 rng.between(.018f, .087f), rng.between(1.30f, 2.06f), 1.89f, .124f,
                 .18f, .41f, core ? 2u : side});
  }
  if (shape == 4) { // Neighboring groups share color
    constexpr Vec3 colors[]{rose, amber, cream, jade, ice, blue, lilac};
    const float angle = std::atan2(direction.y, direction.x) + pi + direction.z * .61f + 2 * pi;
    const unsigned sector = static_cast<unsigned>(angle * (7 / (2 * pi))) % 7;
    const Vec3 tint = colors[sector];
    const float r = index % 9 == 0 ? .37f : 1;
    return star(rng, {}, delay, direction * r,
                {mix(tint, gold, .34f), tint, rng.between(1.41f, 2.13f), 2.17f,
                 rng.between(.19f, .46f), 1.53f, 1.74f, .14f, .31f, .53f, sector});
  }
  if (shape == 5) { // Senrin sub-bursts
    constexpr Vec3 colors[]{mint, rose, cream, lilac, ice, cherry, amber};
    const unsigned group = (index * 7) % static_cast<unsigned>(children.size());
    const auto &child = children[group];
    const Vec3 tint = colors[group % 7];
    return star(rng, child.position, child.delay + delay, direction * child.size,
                {cream, tint, rng.between(.93f, 1.49f), 2.73f, rng.between(.12f, .30f),
                 1.50f, 1.81f, .13f, .19f, .49f, group % 7});
  }
  const bool ring = index % 8 < 5;
  if (ring) {
    const float angle = index * goldenAngle;
    const float x = std::cos(angle), y = std::sin(angle);
    direction = Vec3{x * .93f + y * .16f, y * .37f - x * .36f, y * -.66f} *
                rng.between(.91f, 1.13f);
    return star(rng, {}, delay, direction,
                {ice, y > 0 ? mint : lilac, 1.77f, 2.34f, .21f, 1.62f, 1.81f,
                 .11f, .34f, .45f, y > 0 ? 0u : 1u});
  }
  return star(rng, {}, delay + .068f, direction * .43f,
              {mix(cream, rose, .48f), direction.x > 0 ? rose : lilac, 1.72f, 2.42f, .18f,
               1.57f, 1.76f, .11f, .29f, .49f, 2});
}

Spark ember(const Spark &parent, Random &rng) {
  const float t = rng.between(.14f, .86f) * parent.burn[0];
  const float reach = -std::expm1(-parent.burn[1] * t);
  const float fall = t * t / (1 + .38f * t);
  const auto velocity = xyz(parent.direction) * (parent.burn[1] * (1 - reach));
  const Vec3 point = xyz(parent.origin) + xyz(parent.direction) * reach +
                     Vec3{0, parent.direction[3] * fall, 0};
  const Vec3 spray{rng.between(-.15f, .15f), rng.between(-.13f, .12f), rng.between(-.16f, .16f)};
  const Vec3 tint = mix(xyz(parent.early), cream, .35f);
  return star(rng, point, parent.origin[3] + t, velocity * .20f + spray,
              {tint, copper, rng.between(.32f, .96f), 3.65f, rng.between(.04f, .14f),
               rng.between(.74f, 1.33f), rng.between(.58f, 1.18f), .31f, .27f, .89f,
               static_cast<unsigned>(parent.late[3])}, 1);
}

std::vector<Spark> generate() {
  std::vector<Spark> patterns;
  patterns.reserve(shapeCount * FireworksScene::patternSize);
  constexpr Vec3 flashColors[shapeCount]{gold, cream, mint, ice, lilac, cream, rose, rose, amber, lilac};
  for (unsigned shape = 0; shape < shapeCount; ++shape) {
    Random rng{0x12c3a317u ^ ((shape + 1) * 0x2849u)};
    if (shape + 1 == FireworksAppearance::heartShape) {
      const HeartContour outline;
      for (unsigned i = 0; i < FireworksScene::patternSize; ++i)
        patterns.push_back(heartSpark(outline, i, rng));
      continue;
    }
    Children children{};
    for (unsigned i = 0; i < children.size(); ++i) {
      const auto origin = sphere(i, .77f, .94f, rng) * rng.between(.53f, .84f);
      children[i] = {origin, rng.between(.21f, .36f), rng.between(.18f, .49f)};
    }
    const unsigned base = static_cast<unsigned>(patterns.size());
    const Vec3 flash = flashColors[shape];
    patterns.push_back({{0,0,0,0},{0,0,0,0},{.20f,2,0,4.5f},{3.5f,.9f,2,0},
                        pack(mix(flash, cream, .48f), .025f), pack(flash,0)});
    unsigned primaryIndex = 0;
    for (unsigned i = 1; i < FireworksScene::patternSize; ++i) {
      if (i > 30 && (i % 4 == 0 || (shape == 0 && i % 4 == 2))) {
        const unsigned parent = 1 + (i * 17) % 28;
        patterns.push_back(ember(patterns[base + parent], rng));
      } else if (shape == 5 && i <= children.size() * 2 && i % 2 == 0) {
        const auto &child = children[i / 2 - 1];
        const float distance = -std::expm1(-5.4f * child.delay);
        patterns.push_back({{0,0,0,0},pack(child.position * (1 / distance),0),
                            {child.delay,5.4f,.17f,1.43f},{1.25f,1.7f,0,.35f},
                            pack(gold,.12f),pack(cream,static_cast<float>(i % 7))});
      } else {
        patterns.push_back(primary(shape, primaryIndex++, rng, children));
      }
    }
  }
  return patterns;
}

float linear(unsigned value) {
  const float f = static_cast<float>(value) / 255;
  return f <= .04045f ? f / 12.92f : std::pow((f + .055f) / 1.055f, 2.4f);
}
void viewport(ID3D11DeviceContext *context, unsigned width, unsigned height) {
  const D3D11_VIEWPORT dimensions{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  context->RSSetViewports(1, &dimensions);
}
} // namespace

bool FireworksScene::load(ID3D11Device *device, unsigned width, unsigned height, std::string &error) {
  if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    error = "Fireworks requires D3D11 feature level 11.0";
    return false;
  }
  device_ = device;
  UINT support = 0;
  if (SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R11G11B10_FLOAT, &support)) &&
      (support & D3D11_FORMAT_SUPPORT_BLENDABLE) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
    format_ = DXGI_FORMAT_R11G11B10_FLOAT;

  D3D11_BUFFER_DESC buffer{};
  const auto patterns = generate();
  for (unsigned shape = 0; shape < shapeCount; ++shape) {
    for (unsigned index = 0; index < patternSize; ++index) {
      const auto &spark = patterns[shape * patternSize + index];
      timings_[shape][index] = {spark.origin[3], spark.burn[0], spark.burn[2], 0};
    }
  }
  buffer.ByteWidth = static_cast<UINT>(patterns.size() * sizeof(Spark));
  buffer.StructureByteStride = sizeof(Spark);
  buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  buffer.Usage = D3D11_USAGE_IMMUTABLE;
  buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA initial{patterns.data(), 0, 0};
  Microsoft::WRL::ComPtr<ID3D11Buffer> storage;
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  view.Buffer.NumElements = static_cast<UINT>(patterns.size());
  if (FAILED(device->CreateBuffer(&buffer, &initial, &storage)) ||
      FAILED(device->CreateShaderResourceView(storage.Get(), &view, &patterns_))) {
    error = "Could not upload fireworks patterns";
    return false;
  }

  buffer = {};
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.Usage = D3D11_USAGE_DYNAMIC;
  buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  buffer.ByteWidth = sizeof(Frame);
  static_assert(sizeof(Frame) % 16 == 0);
  if (FAILED(device->CreateBuffer(&buffer, nullptr, &frame_))) {
    error = "Could not create fireworks frame buffer";
    return false;
  }
  buffer.Usage = D3D11_USAGE_DEFAULT;
  buffer.CPUAccessFlags = 0;
  buffer.ByteWidth = sizeof(Vec4);

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend{};
  auto &add = blend.RenderTarget[0];
  add.BlendEnable = TRUE;
  add.SrcBlend = add.DestBlend = D3D11_BLEND_ONE;
  add.BlendOp = D3D11_BLEND_OP_ADD;
  add.SrcBlendAlpha = D3D11_BLEND_ONE;
  add.DestBlendAlpha = D3D11_BLEND_ZERO;
  add.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  add.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

  if (FAILED(device->CreateVertexShader(FireworksSparkVS, sizeof(FireworksSparkVS), nullptr, &sparkVs_)) ||
      FAILED(device->CreateVertexShader(FireworksScreenVS, sizeof(FireworksScreenVS), nullptr, &screenVs_)) ||
      FAILED(device->CreatePixelShader(FireworksSparkPS, sizeof(FireworksSparkPS), nullptr, &sparkPs_)) ||
      FAILED(device->CreatePixelShader(FireworksBlurPS, sizeof(FireworksBlurPS), nullptr, &blurPs_)) ||
      FAILED(device->CreatePixelShader(FireworksCompositePS, sizeof(FireworksCompositePS), nullptr, &compositePs_)) ||
      FAILED(device->CreatePixelShader(FireworksIdlePS, sizeof(FireworksIdlePS), nullptr, &idlePs_)) ||
      FAILED(device->CreateBuffer(&buffer, nullptr, &filterStep_)) ||
      FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
      FAILED(device->CreateRasterizerState(&raster, &rasterizer_)) ||
      FAILED(device->CreateBlendState(&blend, &additive_))) {
    error = "Could not create fireworks shaders or render state";
    return false;
  }
  configure(PackAppearance{});
  return resize(width, height, error);
}

bool FireworksScene::resize(unsigned width, unsigned height, std::string &error) {
  if (width == width_ && height == height_ && light_.view)
    return true;
  const unsigned smallWidth = std::max(1u, (width + 3) / 4);
  const unsigned smallHeight = std::max(1u, (height + 3) / 4);
  const auto make = [&](unsigned w, unsigned h, Target &target) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format_;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &texture)) &&
           SUCCEEDED(device_->CreateRenderTargetView(texture.Get(), nullptr, &target.view)) &&
           SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), nullptr, &target.srv));
  };
  Target light, blurA, blurB;
  if (!make(width, height, light) || !make(smallWidth, smallHeight, blurA) ||
      !make(smallWidth, smallHeight, blurB)) {
    error = "Could not allocate fireworks light buffers";
    return false;
  }
  light_ = std::move(light);
  blurA_ = std::move(blurA);
  blurB_ = std::move(blurB);
  width_ = width;
  height_ = height;
  blurWidth_ = smallWidth;
  blurHeight_ = smallHeight;
  return true;
}

void FireworksScene::configure(const PackAppearance &appearance) {
  if (appearance_.lifetime != appearance.fireworks.lifetime ||
      appearance_.trail != appearance.fireworks.trail || lifespan_[0] == 0) {
    for (unsigned shape = 0; shape < shapeCount; ++shape) {
      float duration = 0;
      for (const auto &timing : timings_[shape])
        duration = std::max(duration, timing[0] + timing[1] * appearance.fireworks.lifetime +
                            timing[2] * appearance.fireworks.trail);
      lifespan_[shape] = duration;
    }
  }
  playback_.configure(appearance.fireworks);
  appearance_ = appearance.fireworks;
  brightness_ = appearance.brightness;
  scale_ = appearance.scale;
  speed_ = appearance.speed;
  intensity_ = appearance.intensity;
  data_.background = {linear(appearance.backgroundColor[0]), linear(appearance.backgroundColor[1]),
                      linear(appearance.backgroundColor[2]), appearance.backgroundBrightness};
  data_.skyGrade = LayerColor::make(appearance.backgroundHue, appearance.backgroundSaturation);
  data_.skyGrade.rows[1][3] = static_cast<float>(appearance.backgroundStyle);
  data_.sparkGrade = LayerColor::make(appearance.hue, appearance.saturation);
  paletteSizes_.fill(0);
  const auto uploadPalette = [&](unsigned slot, const std::vector<EffectRgb> &colors) {
    paletteSizes_[slot] = static_cast<unsigned>(std::min<std::size_t>(colorsPerPalette, colors.size()));
    for (unsigned i = 0; i < paletteSizes_[slot]; ++i) {
      const auto &rgb = colors[i];
      data_.palette[slot * colorsPerPalette + i] = {linear(rgb[0]), linear(rgb[1]), linear(rgb[2]), 0};
    }
  };
  if (appearance.paletteOverride)
    uploadPalette(0, appearance.resolvedPaletteColors);
  paletteChoices_ = static_cast<unsigned>(std::min<std::size_t>(
      FireworksAppearance::paletteLimit, appearance_.paletteChoices.size()));
  for (unsigned i = 0; i < paletteChoices_; ++i)
    uploadPalette(i + 1, appearance_.paletteChoices[i]);
  if (brightness_ <= 0 || intensity_ <= 0)
    clear();
}

float FireworksScene::random() noexcept { return sample(randomState_); }

unsigned FireworksScene::choosePalette() noexcept {
  if (!appearance_.randomPalette || !paletteChoices_)
    return 0;
  const bool skipLast = paletteChoices_ > 1 && lastPalette_ > 0 && lastPalette_ <= paletteChoices_;
  const unsigned choices = paletteChoices_ - static_cast<unsigned>(skipLast);
  const unsigned candidate = 1 + static_cast<unsigned>(random() * choices);
  return candidate + static_cast<unsigned>(skipLast && candidate >= lastPalette_);
}

void FireworksScene::discardExpired(double now) noexcept {
  unsigned retained = 0;
  for (unsigned i = 0; i < count_; ++i)
    if (bursts_[i].expires > now)
      bursts_[retained++] = bursts_[i];
  count_ = retained;
}

bool FireworksScene::push(double now, float x, float y) noexcept {
  if (!std::isfinite(now) || !std::isfinite(x) || !std::isfinite(y) ||
      x < 0 || x > 1 || y < 0 || y > 1 || brightness_ <= 0 || intensity_ <= 0)
    return false;
  const unsigned shape = playback_.choose(
      [this](unsigned count) { return static_cast<unsigned>(random() * count); });
  if (!shape)
    return false;
  discardExpired(now);
  if (count_ == maxBursts) {
    // Oldest remnants leave first. The input being processed always owns a slot.
    std::move(bursts_.begin() + 1, bursts_.end(), bursts_.begin());
    --count_;
  }
  const float speed = std::max(.1f, speed_);
  const float angle = shape == FireworksAppearance::heartShape ? 0 :
                      (random() - .5f) * (shape == 4 || shape == 7 ? .19f : 1.3f);
  const float radius = scale_ * (.92f + random() * .16f);
  lastPalette_ = choosePalette();
  bursts_[count_++] = {now, now + lifespan_[shape - 1] / speed,
                      {{x, y, 0, radius},
                       {appearance_.trail, appearance_.gravity, appearance_.lifetime,
                        (random() - .40f) * .075f},
                       {std::cos(angle), std::sin(angle), static_cast<float>(shape - 1), 1},
                       {}},
                      speed, lastPalette_};
  lastShape_ = shape;
  return true;
}

void FireworksScene::clear() noexcept {
  playback_.rewind();
  count_ = visibleSparks_ = lastShape_ = lastPalette_ = 0;
}

void FireworksScene::filter(ID3D11DeviceContext *context, const Target &input, const Target &output,
                            const Float4 &step) {
  ID3D11RenderTargetView *target = output.view.Get();
  context->OMSetRenderTargets(1, &target, nullptr);
  context->UpdateSubresource(filterStep_.Get(), 0, nullptr, step.data(), 0, 0);
  ID3D11ShaderResourceView *source = input.srv.Get();
  context->PSSetShaderResources(0, 1, &source);
  context->Draw(3, 0);
  source = nullptr;
  context->PSSetShaderResources(0, 1, &source);
}

bool FireworksScene::draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
                          std::string &error) {
  if (!resize(width, height, error))
    return false;
  discardExpired(now);
  constexpr unsigned requested[]{152, 308, patternSize};
  constexpr unsigned budget[]{2200, 3800, maxVisibleSparks};
  const auto quality = std::min(appearance_.quality, 2u);
  const unsigned particles = count_ ? std::min(
      static_cast<unsigned>(static_cast<float>(requested[quality]) * appearance_.density),
      budget[quality] / count_) : 0;
  visibleSparks_ = particles * count_;
  const auto w = static_cast<float>(width), h = static_cast<float>(height);
  data_.screen = {w, h, static_cast<float>(std::fmod(now, 120.0)), appearance_.horizon};
  data_.exposure = {static_cast<float>(particles), brightness_ * intensity_,
                    appearance_.embers, appearance_.shimmer};
  data_.atmosphere = {appearance_.bloom, appearance_.smoke, appearance_.reflection, 0};
  const float pressure = 1 + .016f * static_cast<float>(count_ > 6 ? count_ - 6 : 0);
  for (unsigned i = 0; i < count_; ++i) {
    const auto &burst = bursts_[i];
    auto &record = data_.burst[i];
    record = burst.render;
    record.location[2] = static_cast<float>(now - burst.time) * burst.speed;
    const float edge = static_cast<float>(std::max(0.0, burst.expires - now));
    record.rotation[3] = std::min(1.f, edge * 5) / pressure;
    record.palette = {static_cast<float>(burst.paletteSlot * colorsPerPalette),
                      static_cast<float>(paletteSizes_[burst.paletteSlot]), 0, 0};
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(frame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    error = "Could not upload fireworks frame";
    return false;
  }
  const auto used = offsetof(Frame, burst) + count_ * sizeof(DrawBurst);
  std::memcpy(mapped.pData, &data_, used);
  context->Unmap(frame_.Get(), 0);

  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->RSSetState(rasterizer_.Get());
  context->OMSetDepthStencilState(nullptr, 0);
  ID3D11Buffer *constants = frame_.Get();
  context->VSSetConstantBuffers(0, 1, &constants);
  context->PSSetConstantBuffers(0, 1, &constants);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  if (!visibleSparks_) {
    context->VSSetShader(screenVs_.Get(), nullptr, 0);
    context->PSSetShader(idlePs_.Get(), nullptr, 0);
    context->Draw(3, 0);
    return true;
  }

  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> output;
  context->OMGetRenderTargets(1, &output, nullptr);
  if (!output) {
    error = "Fireworks render target is missing";
    return false;
  }
  ID3D11RenderTargetView *target = light_.view.Get();
  context->OMSetRenderTargets(1, &target, nullptr);
  const float dark[4]{};
  context->ClearRenderTargetView(target, dark);
  context->OMSetBlendState(additive_.Get(), nullptr, 0xffffffff);
  context->VSSetShader(sparkVs_.Get(), nullptr, 0);
  context->PSSetShader(sparkPs_.Get(), nullptr, 0);
  ID3D11ShaderResourceView *records = patterns_.Get();
  context->VSSetShaderResources(3, 1, &records);
  context->DrawInstanced(trailVertices, visibleSparks_, 0, 0);
  records = nullptr;
  context->VSSetShaderResources(3, 1, &records);

  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  ID3D11SamplerState *sampler = sampler_.Get();
  context->PSSetSamplers(0, 1, &sampler);
  const bool glow = appearance_.bloom > 0 || appearance_.smoke > 0;
  if (glow) {
    ID3D11Buffer *step = filterStep_.Get();
    context->PSSetConstantBuffers(1, 1, &step);
    context->PSSetShader(blurPs_.Get(), nullptr, 0);
    viewport(context, blurWidth_, blurHeight_);
    filter(context, light_, blurA_, {1.f / w, 1.f / h, 1, 0});
    filter(context, blurA_, blurB_, {.80f / static_cast<float>(blurWidth_), 0, 0, 0});
    filter(context, blurB_, blurA_, {0, .80f / static_cast<float>(blurHeight_), 0, 0});
  }
  target = output.Get();
  context->OMSetRenderTargets(1, &target, nullptr);
  viewport(context, width, height);
  context->PSSetShader(compositePs_.Get(), nullptr, 0);
  ID3D11ShaderResourceView *lighting[]{light_.srv.Get(), glow ? blurA_.srv.Get() : nullptr};
  context->PSSetShaderResources(0, 2, lighting);
  context->Draw(3, 0);
  ID3D11ShaderResourceView *unbound[]{nullptr, nullptr};
  context->PSSetShaderResources(0, 2, unbound);
  return true;
}
} // namespace action75
