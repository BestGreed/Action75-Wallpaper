#include "GlassShatter.h"
#include "GlassShatterShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace action75 {
namespace {
constexpr float turn = 6.283185307f;
constexpr float referenceRadius = 205.f;
constexpr unsigned patternCount = 8;
constexpr unsigned rayCount = 5, segmentsPerRay = 7, segmentsPerBranch = 2;
constexpr unsigned mainCracks = rayCount * segmentsPerRay;
constexpr unsigned crackCount = mainCracks + segmentsPerBranch * GlassShatterAppearance::maxBranches;
constexpr unsigned paneCount = 1 + GlassShatterAppearance::maxChips;
constexpr unsigned contourEdges = 4;
constexpr unsigned attachedPaneVertices = 3 * rayCount * (2 * (segmentsPerRay + 1) + contourEdges);

struct Point {
  float x, y;
  Point operator+(Point rhs) const { return {x + rhs.x, y + rhs.y}; }
  Point operator-(Point rhs) const { return {x - rhs.x, y - rhs.y}; }
  Point operator*(float scale) const { return {x * scale, y * scale}; }
  float length() const { return std::sqrt(x * x + y * y); }
};
struct Crack {
  Point start, end, normalAtStart, normalAtEnd;
  float begin, endTime, widthAtStart, widthAtEnd;
  float phase, luster, minForce;
};
struct Pane {
  std::array<Point, 4> corners;
  Point pivot, drift, slope;
  float spin, delay, thickness, luster;
  float kind, reserved;
};
static_assert(sizeof(Crack) == 60 && sizeof(Pane) == 80);

float randomUnit(std::mt19937 &random) noexcept {
  return static_cast<float>(random() >> 8) * (1.f / 16777216.f);
}
float randomBetween(std::mt19937 &random, float lower, float upper) noexcept {
  return lower + (upper - lower) * randomUnit(random);
}
Point direction(float radians) { return {std::cos(radians), std::sin(radians)}; }
float dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
Point normal(Point edge) { return Point{-edge.y, edge.x} * (1.f / edge.length()); }

Point expandOuterCrack(Point point) {
  const float radius = point.length();
  const float outer = radius - .47f;
  if (outer <= 0)
    return point;
  const float extension = .84f * outer * outer / (outer + .23f);
  return point * (1.f + extension / radius);
}

float crackReach(double age, float start, float target) noexcept {
  return std::min(target, start + 11.2f * static_cast<float>(age));
}

void makePattern(std::mt19937 &random, Crack *cracks, Pane *panes) {
  using Ray = std::array<Point, segmentsPerRay + 1>;
  std::array<Ray, rayCount> nodes{};
  std::array<float, rayCount> angles{};
  const auto vary = [&](float lower, float upper) { return randomBetween(random, lower, upper); };

  // Build each joint once so adjacent quads share the same two ends.
  const auto addPath = [&](const auto &path, const auto &times, float width, float tip, float minForce) {
    auto projected = path;
    // Both the main path and connected branches expand around the same fixed center.
    for (auto &point : projected)
      point = expandOuterCrack(point);
    const std::size_t last = projected.size() - 1;
    Point left = normal(projected[1] - projected[0]);
    Point joined = left;
    float startWidth = width;
    for (std::size_t i = 0; i < last; ++i) {
      const bool final = i + 1 == last;
      const Point right = final ? left : normal(projected[i + 2] - projected[i + 1]);
      const Point mean = (left + right) * .5f;
      const Point next = mean * (1.f / std::max(.44f, dot(mean, left)));
      const float ratio = (times[i + 1] - times.front()) / (times.back() - times.front());
      const float endWidth = final ? tip : tip + (width - tip) * (1 - std::sqrt(ratio)) * vary(.80f, 1.22f);
      *cracks++ = {projected[i], projected[i + 1], joined, next, times[i], times[i + 1],
                   startWidth, endWidth, vary(0, 1), vary(.46f, 1.2f), minForce};
      left = right;
      joined = next;
      startWidth = endWidth;
    }
  };

  constexpr float bearings[rayCount]{0, .49f, .80f, .27f, .65f};
  constexpr float lengths[rayCount]{1.68f, 1.52f, 1.02f, .65f, .92f};
  constexpr float forceToStart[rayCount]{0, 0, 0, .58f, 1.49f};
  constexpr float sections[segmentsPerRay + 1]{.020f, .055f, .126f, .233f, .36f, .55f, .75f, 1.f};
  for (unsigned ray = 0; ray < rayCount; ++ray) {
    const float angle = (bearings[ray] + vary(-.049f, .049f)) * turn;
    angles[ray] = angle;
    const float end = lengths[ray] * vary(.77f, 1.18f);
    const float drift = vary(-.32f, .32f);
    std::array<float, segmentsPerRay + 1> times{};
    for (unsigned node = 0; node <= segmentsPerRay; ++node) {
      const float fraction = sections[node] * vary(.88f, 1.09f);
      // Leave additional path after the old force ceiling for the strongest impacts.
      const float tail = std::max(0.f, fraction - .64f);
      times[node] = end * (fraction + 1.58f * tail * tail);
      nodes[ray][node] = direction(angle + drift * fraction + vary(-.055f, .055f)) * times[node];
    }
    addPath(nodes[ray], times, vary(.68f, .88f), vary(.075f, .14f), forceToStart[ray]);
  }

  // Earlier branches are still available when the user lowers the branch limit.
  constexpr unsigned roots[]{2, 0, 1, 3, 4, 0, 2};
  constexpr float thresholds[]{.23f, .65f, 1.02f, 1.44f, 1.63f, 1.82f, 2.02f};
  static_assert(std::size(roots) == GlassShatterAppearance::maxBranches);
  for (unsigned branch = 0; branch < std::size(roots); ++branch) {
    const unsigned ray = roots[branch];
    const bool join = branch == 1 || branch == 4;
    const bool nearImpact = branch == 0 || branch == 2 || branch == 3;
    const auto root = nodes[ray][join || branch == 0 ? 1 : (branch == 5 ? 4 : 2)];
    const float side = branch % 2 ? -1.f : 1.f;
    const float angle = angles[ray] + side * vary(.33f, .76f);
    const float branchLength = nearImpact ? .80f : 1.f;
    Point bend = root + direction(angle) * (vary(.078f, .142f) * branchLength);
    Point tip = bend + direction(angle + side * vary(-.11f, .34f)) * (vary(.052f, .14f) * branchLength);
    float start = root.length() + .025f;
    if (join) {
      tip = nodes[branch == 1 ? 3 : 1][1];
      const Point span = tip - root;
      bend = root + span * vary(.31f, .65f) + Point{-span.y, span.x} * vary(.13f, .39f);
      start = std::max(root.length(), tip.length()) + .031f;
    }
    const std::array<Point, 3> path{root, bend, tip};
    const std::array<float, 3> times{start, start + .059f, start + .146f};
    addPath(path, times, vary(.33f, .52f) + (nearImpact ? .055f : 0), join ? .20f : .06f, thresholds[branch]);
  }

  constexpr float hub = .095f;
  *panes++ = {{{{-hub, -hub}, {hub, -hub}, {hub, hub}, {-hub, hub}}},
              {}, {}, {}, 0, 0, 1, 1, 2, 0};
  for (unsigned chip = 0; chip < GlassShatterAppearance::maxChips; ++chip) {
    const float angle = angles[chip] + vary(-.49f, .52f);
    const auto outward = direction(angle), tangent = Point{-outward.y, outward.x};
    const auto origin = outward * vary(.022f, .047f);
    const float size = vary(.012f, .037f);
    const auto a = origin + outward * (size * vary(.63f, 1.16f));
    const auto b = origin + tangent * (size * vary(.36f, .72f));
    const auto c = origin - outward * (size * vary(.54f, .98f)) - tangent * (size * .22f);
    const auto d = origin + outward * (size * .13f) - tangent * (size * vary(.34f, .65f));
    *panes++ = {{a, b, c, d}, origin, outward * vary(.44f, .91f),
                 direction(angle + vary(-.55f, .76f)), vary(-8.f, 8.f),
                 vary(.016f, .061f), vary(.48f, 1.05f), vary(.47f, 1.f), 1, 0};
  }
}

template <typename Geometry, std::size_t Size>
bool uploadPatterns(ID3D11Device *device, const std::array<Geometry, Size> &data,
                    ID3D11ShaderResourceView **view) {
  D3D11_BUFFER_DESC buffer{};
  buffer.ByteWidth = static_cast<UINT>(sizeof(data));
  buffer.Usage = D3D11_USAGE_IMMUTABLE;
  buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  buffer.StructureByteStride = static_cast<UINT>(sizeof(Geometry));
  D3D11_SUBRESOURCE_DATA source{data.data(), 0, 0};
  ComPtr<ID3D11Buffer> geometry;
  if (FAILED(device->CreateBuffer(&buffer, &source, &geometry)))
    return false;
  return SUCCEEDED(device->CreateShaderResourceView(geometry.Get(), nullptr, view));
}
} // namespace

GlassShatter::GlassShatter()
    : GlassShatter(static_cast<std::uint32_t>(std::chrono::steady_clock::now()
                                                 .time_since_epoch().count())) {}

GlassShatter::GlassShatter(std::uint32_t seed) : random_(seed) {}

bool GlassShatter::load(ID3D11Device *device, std::string &error) {
  // Keep the CPU layout in step with the HLSL Frame constant buffer.
  static_assert(maxImpacts == 24 && sizeof(Frame) == 16 * (9 + 3 * maxImpacts));
  if (FAILED(device->CreateVertexShader(GlassBackdropVS, sizeof GlassBackdropVS, nullptr, &backdropVs_)) ||
      FAILED(device->CreatePixelShader(GlassBackdropPS, sizeof GlassBackdropPS, nullptr, &backdropPs_)) ||
      FAILED(device->CreateVertexShader(GlassCrackVS, sizeof GlassCrackVS, nullptr, &crackVs_)) ||
      FAILED(device->CreatePixelShader(GlassCrackPS, sizeof GlassCrackPS, nullptr, &crackPs_)) ||
      FAILED(device->CreateVertexShader(GlassPaneVS, sizeof GlassPaneVS, nullptr, &paneVs_)) ||
      FAILED(device->CreatePixelShader(GlassPanePS, sizeof GlassPanePS, nullptr, &panePs_))) {
    error = "Could not create precompiled glass shaders";
    return false;
  }

  std::array<Crack, patternCount * crackCount> cracks{};
  std::array<Pane, patternCount * paneCount> panes{};
  std::mt19937 patternRandom(0xa751d38au);
  for (unsigned i = 0; i < patternCount; ++i)
    makePattern(patternRandom, cracks.data() + i * crackCount, panes.data() + i * paneCount);

  D3D11_BUFFER_DESC frame{};
  frame.ByteWidth = sizeof(Frame);
  frame.Usage = D3D11_USAGE_DYNAMIC;
  frame.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  frame.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  D3D11_RASTERIZER_DESC rasterizer{};
  rasterizer.FillMode = D3D11_FILL_SOLID;
  rasterizer.CullMode = D3D11_CULL_NONE;
  rasterizer.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend{};
  auto &target = blend.RenderTarget[0];
  target.BlendEnable = TRUE;
  target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
  target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  target.BlendOp = D3D11_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D11_BLEND_ZERO;
  target.DestBlendAlpha = D3D11_BLEND_ONE;
  target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (!uploadPatterns(device, cracks, &cracks_) || !uploadPatterns(device, panes, &panes_) ||
      FAILED(device->CreateBuffer(&frame, nullptr, &frame_)) ||
      FAILED(device->CreateRasterizerState(&rasterizer, &rasterizer_)) ||
      FAILED(device->CreateBlendState(&blend, &alphaBlend_))) {
    error = "Could not create glass geometry and render state";
    return false;
  }
  data_.geometry = {crackCount, paneCount, referenceRadius, 0};
  configure(PackAppearance{});
  return true;
}

void GlassShatter::configure(const PackAppearance &appearance) noexcept {
  const auto &glass = appearance.glassShatter;
  minStrength_ = std::min(glass.impactMin, glass.impactMax);
  maxStrength_ = std::max(glass.impactMin, glass.impactMax);
  duration_ = glass.duration;
  crackVertices_ = 6 * (mainCracks + segmentsPerBranch * std::min(glass.branches, glass.maxBranches));
  paneVertices_ = attachedPaneVertices + 6 * (1 + std::min(glass.chips, glass.maxChips));
  data_.optics = {glass.spread, glass.seamWidth, glass.glints, glass.refraction};
  data_.geometry[3] = appearance.scale;
  struct Lighting { Float4 shadow, reflection, rim, prism; };
  static constexpr Lighting looks[]{
      {{.024f, .048f, .083f, 0}, {.18f, .41f, .57f, 0}, {.73f, .95f, 1.f, 0}, {.24f, .67f, .96f, 0}},
      {{.045f, .025f, .031f, 0}, {.65f, .33f, .15f, 0}, {1.f, .91f, .72f, 0}, {.91f, .43f, .24f, 0}},
      {{.033f, .043f, .044f, 0}, {.39f, .49f, .47f, 0}, {.88f, .97f, .93f, 0}, {.47f, .74f, .73f, 0}},
  };
  static_assert(std::size(looks) == 1 + static_cast<unsigned>(GlassBackdrop::Silver));
  const auto mode = static_cast<unsigned>(glass.backdrop);
  const auto &look = looks[mode < std::size(looks) ? mode : 0];
  data_.screen[3] = static_cast<float>(mode);
  const auto fg = LayerColor::make(appearance.hue, appearance.saturation, appearance.color, appearance.brightness);
  const auto bg = LayerColor::make(appearance.backgroundHue, appearance.backgroundSaturation, appearance.backgroundColor);
  data_.baseShadow = bg.apply(glass.backdrop == GlassBackdrop::Solid ? Float4{1,1,1,0} : look.shadow);
  data_.baseReflection = bg.apply(look.reflection);
  data_.shadow = fg.apply(look.shadow);
  data_.reflection = fg.apply(look.reflection);
  data_.rim = fg.apply(look.rim);
  data_.prism = fg.apply(look.prism);
  data_.reflection[3] = appearance.backgroundBrightness;
}

void GlassShatter::discardExpired(double now) noexcept {
  const auto begin = impacts_.begin(), end = begin + count_;
  const auto alive = std::remove_if(begin, end, [now, duration = duration_](const Impact &impact) {
    return now - impact.lastHit >= duration;
  });
  count_ = static_cast<unsigned>(alive - begin);
}

bool GlassShatter::push(double now, float x, float y) noexcept {
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1 || maxStrength_ <= 0)
    return false;
  discardExpired(now);
  lastStrength_ = randomBetween(random_, minStrength_, maxStrength_);
  const float target = .14f + lastStrength_ * (.48f + .083f * lastStrength_);
  const auto end = impacts_.begin() + count_;
  const auto match = std::find_if(impacts_.begin(), end, [x, y](const Impact &impact) {
    return impact.draw.position[0] == x && impact.draw.position[1] == y;
  });
  if (match != end) {
    // Keep the visible fracture and extend it on a harder press.
    Impact impact = *match;
    impact.reachOnHit = crackReach(now - impact.lastHit, impact.reachOnHit, impact.targetReach);
    impact.targetReach = std::max(target, impact.targetReach);
    impact.lastHit = now;
    impact.draw.motion[2] = std::max(lastStrength_, impact.draw.motion[2]);
    impact.draw.reaction[1] = lastStrength_;
    std::move(match + 1, end, match);
    *(end - 1) = impact;
    return true;
  }

  const float angle = randomUnit(random_) * turn;
  const float pattern = static_cast<float>(random_() % patternCount);
  if (count_ == maxImpacts) {
    std::move(impacts_.begin() + 1, impacts_.end(), impacts_.begin());
    --count_;
  }
  impacts_[count_++] = {now, .10f, target,
                        {{x, y, 0, 0}, {std::cos(angle), std::sin(angle), lastStrength_, pattern},
                         {0, lastStrength_, 0, 0}}};
  return true;
}

bool GlassShatter::draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
                        std::string &error) {
  discardExpired(now);
  data_.screen[0] = static_cast<float>(width);
  data_.screen[1] = static_cast<float>(height);
  data_.screen[2] = static_cast<float>(std::min(width, height)) / 720.f;
  for (unsigned i = 0; i < count_; ++i) {
    const auto &impact = impacts_[i];
    auto &frame = data_.impacts[i];
    frame = impact.draw;
    const float sinceHit = static_cast<float>(now - impact.lastHit);
    const float tail = std::clamp((sinceHit / duration_ - .20f) / .80f, 0.f, 1.f);
    frame.position[2] = crackReach(sinceHit, impact.reachOnHit, impact.targetReach);
    frame.position[3] = 1 - tail * tail * (3 - 2 * tail);
    frame.reaction[0] = sinceHit;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(frame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    error = "Could not update glass frame";
    return false;
  }
  std::memcpy(mapped.pData, &data_, offsetof(Frame, impacts) + count_ * sizeof(DrawImpact));
  context->Unmap(frame_.Get(), 0);

  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
  context->RSSetState(rasterizer_.Get());
  context->OMSetDepthStencilState(nullptr, 0);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetConstantBuffers(0, 1, frame_.GetAddressOf());
  context->PSSetConstantBuffers(0, 1, frame_.GetAddressOf());
  context->VSSetShader(backdropVs_.Get(), nullptr, 0);
  context->PSSetShader(backdropPs_.Get(), nullptr, 0);
  context->Draw(3, 0);
  if (count_ == 0)
    return true;

  ID3D11ShaderResourceView *views[]{cracks_.Get(), panes_.Get()};
  context->VSSetShaderResources(0, 2, views);
  context->OMSetBlendState(alphaBlend_.Get(), nullptr, 0xffffffff);
  context->VSSetShader(crackVs_.Get(), nullptr, 0);
  context->PSSetShader(crackPs_.Get(), nullptr, 0);
  context->DrawInstanced(crackVertices_, count_, 0, 0);
  // The attached panes meet the principal cracks at sampled points along their paths.
  context->VSSetShader(paneVs_.Get(), nullptr, 0);
  context->PSSetShader(panePs_.Get(), nullptr, 0);
  context->DrawInstanced(paneVertices_, count_, 0, 0);
  ID3D11ShaderResourceView *empty[2]{};
  context->VSSetShaderResources(0, 2, empty);
  return true;
}
} // namespace action75
