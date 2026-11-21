#include "FieldScene.h"
#include "FieldVertex.h"
#include "blurHorizontalShader.h"
#include "blurVerticalShader.h"
#include "compositeShader.h"
#include "graded_compositeShader.h"
#include "graded_shadeShader.h"
#include "shadeShader.h"
#include "simShader.h"

#include <cstdint>
#include <stdexcept>

namespace wallpaper {
namespace {
constexpr int Theme = FIELD_THEME;
static_assert(Theme == 0 || Theme == 1 || Theme == 3);
constexpr auto SurfaceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr auto PaletteFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr auto NoiseFormat = DXGI_FORMAT_R16_UNORM;
constexpr unsigned TileColumns = 32, TileRows = 12, TableStride = TextureSlots;
using TileMasks = std::array<std::array<std::uint32_t, 4>, TileColumns * TileRows / 4>;

struct alignas(16) Constants {
  float clock[4];       // Simulation time, step, output aspect, theme.
  float dynamics[4];    // Persistence, turbulence, support radius, propagation.
  float look[4];        // Brightness, intensity, ambient energy, detail.
  float finish[4];      // Bloom, sparks, quality, compact arc count.
  float geometry[4];    // Compact hit count, blur texel sizes, shade texel height.
  float hits[24][4];
  // Preserve the original HLSL cbuffer offsets for the shared field shaders.
  float hitInfo[24][4], unusedSnowConstants[31][4];
  float arcs[32][4], arcInfo[32][4], arcAxis[32][4], arcShape[32][4];
  TileMasks simulationTiles, imprintTiles, arcTiles;
  Background::Constants background;
};
static_assert(sizeof(Constants) == 504 * 16 && offsetof(Constants, background) == 500 * 16);

void markTiles(TileMasks &masks, unsigned element, float left, float top, float right, float bottom) {
  const auto tile = [](float value, unsigned dimension) {
    return static_cast<unsigned>(std::clamp(value * static_cast<float>(dimension), 0.f,
                                            static_cast<float>(dimension - 1)));
  };
  const auto x0 = tile(left, TileColumns), x1 = tile(right, TileColumns);
  const auto y0 = tile(top, TileRows), y1 = tile(bottom, TileRows);
  const std::uint32_t mask = std::uint32_t{1} << element;
  for (unsigned y = y0; y <= y1; ++y)
    for (unsigned x = x0; x <= x1; ++x) {
      const unsigned index = y * TileColumns + x;
      masks[index / 4][index % 4] |= mask;
    }
}

float smooth(float x) {
  x = std::clamp(x, 0.f, 1.f);
  return x * x * (3 - 2 * x);
}

using Color = std::array<float, 3>;
float linear(float value) {
  value /= 255;
  return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
}

Color toLab(const Rgb &rgb) {
  const float r = linear(rgb[0]), g = linear(rgb[1]), b = linear(rgb[2]);
  const float l = std::cbrt(.4122214708f * r + .5363325363f * g + .0514459929f * b);
  const float m = std::cbrt(.2119034982f * r + .6806995451f * g + .1073969566f * b);
  const float s = std::cbrt(.0883024619f * r + .2817188376f * g + .6299787005f * b);
  return {.2104542553f * l + .793617785f * m - .0040720468f * s,
          1.9779984951f * l - 2.428592205f * m + .4505937099f * s,
          .0259040371f * l + .7827717662f * m - .808675766f * s};
}

Color fromLab(const Color &color) {
  float l = color[0] + .3963377774f * color[1] + .2158037573f * color[2];
  float m = color[0] - .1055613458f * color[1] - .0638541728f * color[2];
  float s = color[0] - .0894841775f * color[1] - 1.291485548f * color[2];
  l = l * l * l;
  m = m * m * m;
  s = s * s * s;
  return {4.0767416621f * l - 3.3077115913f * m + .2309699292f * s,
          -1.2684380046f * l + 2.6097574011f * m - .3413193965f * s,
          -.0041960863f * l - .7034186147f * m + 1.707614701f * s};
}

Color gamutMapped(const Color &color) {
  const auto within = [](const Color &rgb) {
    return *std::min_element(rgb.begin(), rgb.end()) >= 0 && *std::max_element(rgb.begin(), rgb.end()) <= 1;
  };
  const auto rgb = fromLab(color);
  if (within(rgb))
    return rgb;
  float low = 0, high = 1;
  for (int step = 0; step < 16; ++step) {
    const float chroma = (low + high) * .5f;
    if (within(fromLab({color[0], color[1] * chroma, color[2] * chroma})))
      low = chroma;
    else
      high = chroma;
  }
  return fromLab({color[0], color[1] * low, color[2] * low});
}
} // namespace

FieldScene::FieldScene(const A75Create12 &host)
    : device_(host.device), graphics_(device_, sizeof(Constants), true),
      noise_(device_, 256, 256, NoiseFormat), palette_(device_, 256, 1, PaletteFormat) {
  const auto folder = std::filesystem::path(host.pack_dir);
  const auto layout = readJson(folder / L"layout.json");
  const auto &keys = layout.at("keys");
  if (keys.size() != centers_.size())
    throw std::runtime_error("Field needs 84 key centers");
  for (unsigned index = 0; index < centers_.size(); ++index) {
    const auto &center = keys.at(index).at("centerPx");
    const float x = center.at("x").get<float>(), y = center.at("y").get<float>();
    if (!std::isfinite(x) || !std::isfinite(y))
      throw std::runtime_error("Invalid field key center");
    centers_[index] = {x / 1920, y / 720};
  }
  const auto settings = readJson(folder / L"settings.json");
  for (const auto &group : settings.at("groups"))
    for (const auto &control : group.at("controls"))
      if (control.at("id") == "palette")
        presets_ = control.at("palettes");

  std::vector<std::uint16_t> noisePixels(256 * 256);
  std::uint32_t seed = 0x5a17d3u;
  for (auto &pixel : noisePixels) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    pixel = static_cast<std::uint16_t>(seed >> 16);
  }
  noise_.stage(noisePixels.data(), 256 * sizeof(std::uint16_t));

  const auto vertex = shader(FieldVertex);
  sim_ = graphics_.pipeline(vertex, shader(simShader), SurfaceFormat);
  shade_ = graphics_.pipeline(vertex, shader(shadeShader), SurfaceFormat);
  blurHorizontal_ = graphics_.pipeline(vertex, shader(blurHorizontalShader), SurfaceFormat);
  blurVertical_ = graphics_.pipeline(vertex, shader(blurVerticalShader), SurfaceFormat);
  composite_ = graphics_.pipeline(vertex, shader(compositeShader), OutputFormat);
  D3D12_DESCRIPTOR_HEAP_DESC renderTargets{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, SurfaceCount};
  D3D12_DESCRIPTOR_HEAP_DESC shaderTextures{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                           TableCount * TableStride,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  check(device_->CreateDescriptorHeap(&renderTargets, IID_PPV_ARGS(&targets_)), "Create field RTV heap");
  check(device_->CreateDescriptorHeap(&shaderTextures, IID_PPV_ARGS(&textures_)), "Create field SRV heap");
  targetStride_ = device_->GetDescriptorHandleIncrementSize(renderTargets.Type);
  textureStride_ = device_->GetDescriptorHandleIncrementSize(shaderTextures.Type);
}

void FieldScene::configure(const Json &values) {
  background_.configure(values);
  lifetime_ = values.value("persistence", 3.f);
  turbulence_ = values.value("turbulence", .9f);
  propagation_ = values.value("propagation", 1.f);
  ambient_ = values.value("ambient", .35f);
  detail_ = values.value("detail", .8f);
  bloom_ = values.value("bloom", .6f);
  sparks_ = values.value("sparks", .5f);
  quality_ = values.value("quality", 1.f);
  const float brightness = values.value("brightness", 1.f);
  if (brightness_ > 0 && brightness <= 0)
    clear();
  brightness_ = brightness;
  intensity_ = values.value("intensity", 1.f);
  scale_ = values.value("size", 1.f);
  speed_ = values.value("speed", 1.f);

  std::vector<Rgb> colors;
  if (const auto palette = values.find("palette"); palette != values.end()) {
    colors = palette->at("colors").get<std::vector<Rgb>>();
    for (const auto &preset : presets_)
      if (preset.at("id") == palette->at("preset")) {
        colors = preset.at("colors").get<std::vector<Rgb>>();
        break;
      }
  }
  if (colors.empty())
    colors = {{20, 24, 56}, {64, 53, 128}, {34, 145, 140}, {184, 222, 195}};
  if (colors != colors_) {
    colors_ = std::move(colors);
    palettePending_ = true;
  }
}

void FieldScene::press(const A75Event &event) {
  const auto [x, y, now] = Hit{event.x, event.y, event.seconds};
  if (brightness_ <= 0 || !std::isfinite(now) || !std::isfinite(x) || !std::isfinite(y) ||
      x < 0 || x > 1 || y < 0 || y > 1)
    return;
  if constexpr (Theme == 1) {
    const auto last = hits_[(nextHit_ + hits_.size() - 1) % hits_.size()];
    const bool recent = last.time >= 0 && now - last.time < 1;
    std::vector<std::array<float, 2>> targets(centers_.begin(), centers_.end());
    if (recent)
      targets.push_back({last.x, last.y});
    const auto distance = [&](float u, float v) { return std::hypot((u - x) * 2.667f, v - y); };
    std::sort(targets.begin(), targets.end(), [&](const auto &a, const auto &b) {
      return distance(a[0], a[1]) < distance(b[0], b[1]);
    });
    const auto link = [&](float u, float v) {
      arcs_[nextArc_ % arcs_.size()] = {x, y, u, v, now, static_cast<float>((nextArc_ * 37) % 101) / 101};
      ++nextArc_;
    };
    unsigned linked = 0;
    if (recent && distance(last.x, last.y) > .04f) {
      link(last.x, last.y);
      ++linked;
    }
    for (const auto &center : targets) {
      if (distance(center[0], center[1]) < .04f)
        continue;
      link(center[0], center[1]);
      if (++linked == 3)
        break;
    }
  }
  hits_[nextHit_++ % hits_.size()] = {x, y, now};
}

void FieldScene::clear() {
  for (auto &hit : hits_)
    hit.time = -1;
  for (auto &arc : arcs_)
    arc.time = -1;
  reset_ = true;
  previous_ = -1;
}

D3D12_CPU_DESCRIPTOR_HANDLE FieldScene::target(unsigned index) const {
  auto handle = targets_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += index * targetStride_;
  return handle;
}

void FieldScene::bindTable(ID3D12GraphicsCommandList *commands, unsigned index) const {
  auto handle = textures_->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += index * TableStride * textureStride_;
  commands->SetGraphicsRootDescriptorTable(1, handle);
}

void FieldScene::updateTables() {
  const auto table = [&](unsigned index, std::array<ID3D12Resource *, TableStride> resources) {
    auto handle = textures_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += index * TableStride * textureStride_;
    constexpr DXGI_FORMAT formats[]{SurfaceFormat, PaletteFormat, SurfaceFormat, NoiseFormat,
                                    SurfaceFormat, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    for (unsigned slot = 0; slot < TableStride; ++slot) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = formats[slot];
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(resources[slot], &view, handle);
      handle.ptr += textureStride_;
    }
  };
  for (unsigned front = 0; front < 2; ++front) {
    table(Sim0 + front, {surfaces_[State0 + front].Get(), nullptr, nullptr, noise_.get()});
    table(Shade0 + front, {surfaces_[State0 + front].Get(), palette_.get(), nullptr, noise_.get()});
  }
  table(Horizontal, {nullptr, nullptr, surfaces_[Image].Get()});
  table(Vertical, {nullptr, nullptr, surfaces_[Blur0].Get()});
  table(Composite, {nullptr, nullptr, surfaces_[Image].Get(), nullptr, surfaces_[Blur1].Get()});
}

void FieldScene::resize(unsigned width, unsigned height) {
  const unsigned simDivisor = quality_ < .5f ? 3 : quality_ > 1.5f ? 1 : 2;
  const unsigned imageDivisor = Theme == 0 ? simDivisor : (quality_ < .5f ? 2 : 1);
  const unsigned simWidth = std::max(1u, width / simDivisor), simHeight = std::max(1u, height / simDivisor);
  const unsigned imageWidth = std::max(1u, width / imageDivisor), imageHeight = std::max(1u, height / imageDivisor);
  const unsigned blurWidth = std::max(1u, width / 4), blurHeight = std::max(1u, height / 4);
  const auto allocate = [&](unsigned index, unsigned w, unsigned h) {
    D3D12_CLEAR_VALUE clear{};
    clear.Format = SurfaceFormat;
    surfaces_[index] = texture(device_, w, h, SurfaceFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    device_->CreateRenderTargetView(surfaces_[index].Get(), nullptr, target(index));
  };
  bool changed = false;
  if (simWidth != simWidth_ || simHeight != simHeight_ || imageWidth != imageWidth_ || imageHeight != imageHeight_) {
    allocate(State0, simWidth, simHeight);
    allocate(State1, simWidth, simHeight);
    allocate(Image, imageWidth, imageHeight);
    simWidth_ = simWidth;
    simHeight_ = simHeight;
    imageWidth_ = imageWidth;
    imageHeight_ = imageHeight;
    front_ = 0;
    changed = reset_ = true;
  }
  if (bloom_ <= 0) {
    if (surfaces_[Blur0]) {
      surfaces_[Blur0].Reset();
      surfaces_[Blur1].Reset();
      changed = true;
    }
  } else if (!surfaces_[Blur0] || blurWidth != blurWidth_ || blurHeight != blurHeight_) {
    allocate(Blur0, blurWidth, blurHeight);
    allocate(Blur1, blurWidth, blurHeight);
    changed = true;
  }
  blurWidth_ = blurWidth;
  blurHeight_ = blurHeight;
  if (changed)
    updateTables();
}

void FieldScene::recordPalette(ID3D12GraphicsCommandList *commands) {
  std::array<ColorRow, 256> pixels{};
  for (unsigned index = 0; index < pixels.size(); ++index) {
    const auto end = static_cast<unsigned>(colors_.size() - 1);
    const float position = index / 255.f * end;
    const unsigned first = std::min(static_cast<unsigned>(position), end), last = std::min(first + 1, end);
    const auto a = toLab(colors_[first]), b = toLab(colors_[last]);
    const float t = position - first;
    const auto rgb = gamutMapped({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                                  a[2] + (b[2] - a[2]) * t});
    pixels[index] = {rgb[0], rgb[1], rgb[2], 1};
  }
  palette_.stage(pixels.data(), sizeof(pixels));
  palette_.record(commands);
  palettePending_ = false;
}

int32_t FieldScene::render(const A75Frame12 &frame, bool paused) {
  auto *commands = frame.commands;
  if (brightness_ <= 0) {
    commands->ClearRenderTargetView(frame.rtv, background_.gpu.solid.data(), 0, nullptr);
    return -1;
  }
  if (background_.edited && !gradedShade_) {
    const auto vertex = shader(FieldVertex);
    gradedShade_ = graphics_.pipeline(vertex, shader(graded_shadeShader), SurfaceFormat);
    gradedComposite_ = graphics_.pipeline(vertex, shader(graded_compositeShader), OutputFormat);
  }
  resize(frame.width, frame.height);
  if (noisePending_) {
    noise_.record(commands);
    noisePending_ = false;
  }
  if (palettePending_)
    recordPalette(commands);

  const double now = frame.seconds;
  const double elapsed = previous_ < 0 ? 1. / 60 : std::max(0., now - previous_);
  previous_ = now;
  if (paused) {
    for (auto &hit : hits_)
      if (hit.time >= 0)
        hit.time += elapsed;
    for (auto &arc : arcs_)
      if (arc.time >= 0)
        arc.time += elapsed;
  }
  const float step = paused ? 0 : static_cast<float>(std::min(elapsed, .1)) * speed_;
  simulationTime_ += step;
  const float aspect = static_cast<float>(frame.width) / frame.height;
  Constants v{};
  v.background = background_.gpu;
  v.clock[0] = static_cast<float>(simulationTime_);
  v.clock[1] = step;
  v.clock[2] = aspect;
  v.clock[3] = static_cast<float>(Theme);
  v.dynamics[0] = lifetime_;
  v.dynamics[1] = turbulence_;
  v.dynamics[2] = scale_;
  v.dynamics[3] = propagation_;
  v.look[0] = brightness_;
  v.look[1] = intensity_;
  v.look[2] = ambient_;
  v.look[3] = detail_;
  v.finish[0] = bloom_;
  v.finish[1] = sparks_;
  v.finish[2] = quality_;
  v.geometry[1] = 1.f / blurWidth_;
  v.geometry[2] = 1.f / blurHeight_;
  v.geometry[3] = 1.f / imageHeight_;

  // Past 10.5 Gaussian widths the brush is below the smallest float.
  const float brush = 10.5f * std::max(.033f * scale_, .00031623f) + .0001f;
  const float imprint = .2f * scale_ + .0001f;
  unsigned live = 0;
  for (auto &hit : hits_) {
    if (hit.time < 0)
      continue;
    const float age = std::max(0.f, static_cast<float>(now - hit.time));
    if (age >= lifetime_ * 2) {
      hit.time = -1;
      continue;
    }
    const float gain = (1 - std::exp(-age * 35)) * std::exp(-age / lifetime_) *
                       smooth((lifetime_ * 2 - age) / (lifetime_ * .5f));
    if (gain < .0001f)
      continue;
    if (step > 0)
      markTiles(v.simulationTiles, live, hit.x - brush / aspect, hit.y - brush,
                hit.x + brush / aspect, hit.y + brush);
    if constexpr (Theme == 1)
      markTiles(v.imprintTiles, live, hit.x - imprint / aspect, hit.y - imprint,
                hit.x + imprint / aspect, hit.y + imprint);
    v.hits[live][0] = hit.x;
    v.hits[live][1] = hit.y;
    v.hits[live][2] = age;
    v.hits[live][3] = gain;
    ++live;
  }
  v.geometry[0] = static_cast<float>(live);
  if constexpr (Theme == 1) {
    unsigned count = 0;
    for (auto &arc : arcs_) {
      if (arc.time < 0)
        continue;
      const float age = std::max(0.f, static_cast<float>(now - arc.time));
      if (age >= lifetime_) {
        arc.time = -1;
        continue;
      }
      const float gain = (1 - std::exp(-age * 40)) * std::exp(-age * 3 / lifetime_) *
                         smooth((lifetime_ - age) / (lifetime_ * .3f));
      if (gain < .0001f)
        continue;
      v.arcs[count][0] = arc.x;
      v.arcs[count][1] = arc.y;
      v.arcs[count][2] = arc.z;
      v.arcs[count][3] = arc.w;
      v.arcInfo[count][0] = age;
      v.arcInfo[count][1] = gain;
      v.arcInfo[count][2] = arc.seed;
      v.arcInfo[count][3] = .8f + .2f * std::sin(arc.seed * 43 + static_cast<float>(simulationTime_) * 11);
      const float dx = (arc.z - arc.x) * aspect, dy = arc.w - arc.y;
      const float length = std::max(std::hypot(dx, dy), .001f);
      v.arcAxis[count][0] = dx / length;
      v.arcAxis[count][1] = dy / length;
      v.arcAxis[count][2] = -dy / length;
      v.arcAxis[count][3] = dx / length;
      v.arcShape[count][0] = 1 / length;
      v.arcShape[count][1] = .034f * std::sin(arc.seed * 29) * propagation_;
      v.arcShape[count][2] = .0175f * turbulence_ + .034f * propagation_ + .055f * scale_;
      const float halfWidth = v.arcShape[count][2] + .0001f;
      const float xPad = .051f * std::abs(arc.z - arc.x) + halfWidth * std::abs(dy / length) / aspect;
      const float yPad = .051f * std::abs(arc.w - arc.y) + halfWidth * std::abs(dx / length);
      markTiles(v.arcTiles, count, std::min(arc.x, arc.z) - xPad, std::min(arc.y, arc.w) - yPad,
                std::max(arc.x, arc.z) + xPad, std::max(arc.y, arc.w) + yPad);
      ++count;
    }
    v.finish[3] = static_cast<float>(count);
  }

  const auto beforeWrite = [&](unsigned index) {
    transition(commands, surfaces_[index].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
  };
  const auto afterWrite = [&](unsigned index) {
    transition(commands, surfaces_[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  };
  if (reset_) {
    constexpr float zero[4]{};
    for (const auto index : {State0, State1}) {
      beforeWrite(index);
      commands->ClearRenderTargetView(target(index), zero, 0, nullptr);
      afterWrite(index);
    }
    reset_ = false;
  }
  graphics_.bind(commands, &v);
  auto *descriptors = textures_.Get();
  commands->SetDescriptorHeaps(1, &descriptors);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const auto pass = [&](unsigned surface, unsigned table, unsigned width, unsigned height,
                        ID3D12PipelineState *pipeline) {
    beforeWrite(surface);
    setTarget(commands, target(surface), width, height);
    bindTable(commands, table);
    commands->SetPipelineState(pipeline);
    commands->DrawInstanced(3, 1, 0, 0);
    afterWrite(surface);
  };
  if (step > 0) {
    pass(State0 + 1 - front_, Sim0 + front_, simWidth_, simHeight_, sim_.Get());
    front_ = 1 - front_;
  }
  pass(Image, Shade0 + front_, imageWidth_, imageHeight_, background_.edited ? gradedShade_.Get() : shade_.Get());
  if (bloom_ > 0) {
    pass(Blur0, Horizontal, blurWidth_, blurHeight_, blurHorizontal_.Get());
    pass(Blur1, Vertical, blurWidth_, blurHeight_, blurVertical_.Get());
  }
  setTarget(commands, frame.rtv, frame.width, frame.height);
  bindTable(commands, Composite);
  commands->SetPipelineState(background_.edited ? gradedComposite_.Get() : composite_.Get());
  commands->DrawInstanced(3, 1, 0, 0);
  return paused ? -1 : 0;
}
} // namespace wallpaper
