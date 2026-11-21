#include "GlassShatter.h"
#include "LayerColor.h"

#include "GlassBackdropVS.h"
#include "GlassBackdropPS.h"
#include "GlassCrackVS.h"
#include "GlassCrackPS.h"
#include "GlassPaneVS.h"
#include "GlassPanePS.h"

#include <chrono>

namespace surface {
namespace {
constexpr float turn = 6.283185307f;
constexpr float referenceRadius = 205.f;
constexpr unsigned patternCount = 8;
constexpr unsigned rayCount = 5, segmentsPerRay = 7, segmentsPerBranch = 2;
constexpr unsigned mainCracks = rayCount * segmentsPerRay;
constexpr unsigned crackCount = mainCracks + segmentsPerBranch * GlassShatter::maxBranches;
constexpr unsigned paneCount = 1 + GlassShatter::maxChips;
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
  static_assert(std::size(roots) == GlassShatter::maxBranches);
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
  for (unsigned chip = 0; chip < GlassShatter::maxChips; ++chip) {
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

 } // namespace

GlassShatter::GlassShatter(ID3D12Device *device, unsigned, unsigned)
    : gpu_(device, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
      background_(gpu_.graphics(shader(GlassBackdropVS), shader(GlassBackdropPS))),
      cracks_(gpu_.graphics(shader(GlassCrackVS), shader(GlassCrackPS), Gpu::outputFormat, Blend::Alpha)),
      panes_(gpu_.graphics(shader(GlassPaneVS), shader(GlassPanePS), Gpu::outputFormat, Blend::Alpha)),
      random_(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
  static_assert(maxImpacts == 24 && sizeof(Frame) == 16 * (9 + 3 * maxImpacts));
  std::array<Crack, patternCount * crackCount> cracks{};
  std::array<Pane, patternCount * paneCount> panes{};
  std::mt19937 patternRandom(0xa751d38au);
  for (unsigned i = 0; i < patternCount; ++i)
    makePattern(patternRandom, cracks.data() + i * crackCount, panes.data() + i * paneCount);
  crackGeometry_ = gpu_.structured(cracks.data(), static_cast<unsigned>(cracks.size()), sizeof(Crack));
  paneGeometry_ = gpu_.structured(panes.data(), static_cast<unsigned>(panes.size()), sizeof(Pane));
  data_.geometry = {crackCount, paneCount, referenceRadius, 0};
}

void GlassShatter::configure(const Settings &settings) {
  const float low = settings.scalar("impactMin"), high = settings.scalar("impactMax");
  minStrength_ = std::min(low, high);
  maxStrength_ = std::max(low, high);
  duration_ = settings.scalar("duration");
  crackVertices_ = 6 * (mainCracks + segmentsPerBranch * std::min(settings.index("branches"), maxBranches));
  paneVertices_ = attachedPaneVertices + 6 * (1 + std::min(settings.index("chips"), maxChips));
  data_.optics = {settings.scalar("spread"), settings.scalar("seamWidth"),
                 settings.scalar("glints"), settings.scalar("refraction")};
  data_.geometry[3] = settings.scalar("coreSpread");
  struct Lighting { Float4 shadow, reflection, rim, prism; };
  static constexpr Lighting looks[]{
      {{.024f, .048f, .083f, 0}, {.18f, .41f, .57f, 0}, {.73f, .95f, 1.f, 0}, {.24f, .67f, .96f, 0}},
      {{.045f, .025f, .031f, 0}, {.65f, .33f, .15f, 0}, {1.f, .91f, .72f, 0}, {.91f, .43f, .24f, 0}},
      {{.033f, .043f, .044f, 0}, {.39f, .49f, .47f, 0}, {.88f, .97f, .93f, 0}, {.47f, .74f, .73f, 0}},
  };
  const auto mode = settings.index("backdrop");
  const auto &look = looks[mode < std::size(looks) ? mode : 0];
  data_.screen[3] = static_cast<float>(mode);
  const auto fg = LayerColor::make(settings.scalar("effectHue"), settings.scalar("effectSaturation"),
                                   settings.color("effectColor"), settings.scalar("effectBrightness"));
  const auto bg = LayerColor::make(settings.scalar("backgroundHue"), settings.scalar("backgroundSaturation"),
                                   settings.color("backgroundColor"));
  data_.baseShadow = bg.apply(mode == 3 ? Float4{1,1,1,0} : look.shadow);
  data_.baseShadow[3] = settings.scalar("backgroundOpacity") / 100.f;
  data_.baseReflection = bg.apply(look.reflection);
  data_.shadow = fg.apply(look.shadow);
  data_.reflection = fg.apply(look.reflection);
  data_.rim = fg.apply(look.rim);
  data_.prism = fg.apply(look.prism);
  data_.reflection[3] = settings.scalar("brightness");
  fractureOpacity_ = settings.scalar("fractureOpacity") / 100.f;
  if (fractureOpacity_ == 0)
    clear();
  if (data_.baseShadow[3] < 1 && fractureOpacity_ > 0 && !transparentCracks_) {
    auto cracks = gpu_.graphics(shader(GlassCrackVS), shader(GlassCrackPS), Gpu::outputFormat, Blend::Coverage);
    auto panes = gpu_.graphics(shader(GlassPaneVS), shader(GlassPanePS), Gpu::outputFormat, Blend::Coverage);
    transparentCracks_ = std::move(cracks);
    transparentPanes_ = std::move(panes);
  }
}

void GlassShatter::discardExpired(double now) noexcept {
  const auto begin = impacts_.begin(), end = begin + count_;
  const auto alive = std::remove_if(begin, end, [now, duration = duration_](const Impact &impact) {
    return now - impact.lastHit >= duration;
  });
  count_ = static_cast<unsigned>(alive - begin);
}

void GlassShatter::push(double now, float x, float y) noexcept {
  if (!std::isfinite(now) || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 ||
      y < 0 || y > 1 || maxStrength_ <= 0 || fractureOpacity_ == 0)
    return;
  discardExpired(now);
  const float strength = randomBetween(random_, minStrength_, maxStrength_);
  const float target = .14f + strength * (.48f + .083f * strength);
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
    impact.draw.motion[2] = std::max(strength, impact.draw.motion[2]);
    impact.draw.reaction[1] = strength;
    std::move(match + 1, end, match);
    *(end - 1) = impact;
    return;
  }

  const float angle = randomUnit(random_) * turn;
  const float pattern = static_cast<float>(random_() % patternCount);
  if (count_ == maxImpacts) {
    std::move(impacts_.begin() + 1, impacts_.end(), impacts_.begin());
    --count_;
  }
  impacts_[count_++] = {now, .10f, target,
                        {{x, y, 0, 0}, {std::cos(angle), std::sin(angle), strength, pattern},
                         {0, strength, 0, 0}}};
}

void GlassShatter::draw(const A75Frame12 &frame, bool) {
  discardExpired(frame.seconds);
  data_.screen[0] = static_cast<float>(frame.width);
  data_.screen[1] = static_cast<float>(frame.height);
  data_.screen[2] = static_cast<float>(std::min(frame.width, frame.height)) / 720.f;
  for (unsigned i = 0; i < count_; ++i) {
    const auto &impact = impacts_[i];
    auto &data = data_.impacts[i];
    data = impact.draw;
    const float sinceHit = static_cast<float>(frame.seconds - impact.lastHit);
    const float tail = std::clamp((sinceHit / duration_ - .20f) / .80f, 0.f, 1.f);
    data.position[2] = crackReach(sinceHit, impact.reachOnHit, impact.targetReach);
    data.position[3] = (1 - tail * tail * (3 - 2 * tail)) * fractureOpacity_;
    data.reaction[0] = sinceHit;
  }
  auto *commands = frame.commands;
  gpu_.begin(commands);
  Gpu::prepare(commands, crackGeometry_);
  Gpu::prepare(commands, paneGeometry_);
  Gpu::target(commands, frame.rtv, frame.width, frame.height);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->SetGraphicsRootConstantBufferView(0,
      gpu_.constantBytes(&data_, offsetof(Frame, impacts) + count_ * sizeof(DrawImpact)));
  commands->SetPipelineState(background_.Get());
  commands->DrawInstanced(3, 1, 0, 0);
  if (!count_)
    return;

  commands->SetGraphicsRootDescriptorTable(Gpu::srvSlot(0), crackGeometry_.srv.gpu);
  commands->SetGraphicsRootDescriptorTable(Gpu::srvSlot(1), paneGeometry_.srv.gpu);
  const bool transparent = data_.baseShadow[3] < 1;
  commands->SetPipelineState(transparent ? transparentCracks_.Get() : cracks_.Get());
  commands->DrawInstanced(crackVertices_, count_, 0, 0);
  commands->SetPipelineState(transparent ? transparentPanes_.Get() : panes_.Get());
  commands->DrawInstanced(paneVertices_, count_, 0, 0);
}

} // namespace surface
