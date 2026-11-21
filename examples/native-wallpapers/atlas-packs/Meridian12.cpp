#include "Wallpaper12.h"
#include "MeridianShaders12.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace atlas_packs {
namespace {
constexpr unsigned maxHits = 24;
constexpr float pi = 3.14159265359f;

struct Constants {
  std::array<float, 4> display, look, motion;
};

using Vec4 = std::array<float, 4>;
struct Instance {
  Vec4 center, shape, motion, color, data;
};
static_assert(sizeof(Instance) == 80);

constexpr D3D12_INPUT_ELEMENT_DESC elements[]{
    {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"INSTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"INSTANCE", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"INSTANCE", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    {"INSTANCE", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
};

float smooth(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3 - 2 * t);
}

float linear(float x) {
  x /= 255;
  return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}

Vec4 colorForHit(unsigned index, const Appearance& appearance) {
  static constexpr Rgb orbit[]{{255, 197, 106}, {255, 152, 102}, {255, 224, 163}};
  const auto& colors = appearance.palette;
  const auto rgb = colors.empty() ? orbit[index % std::size(orbit)] : colors[index % colors.size()];
  std::array<float, 3> c{float(rgb[0]), float(rgb[1]), float(rgb[2])};
  if (appearance.hue != 0) {
    const float radians = appearance.hue * .01745329252f;
    const float cosine = std::cos(radians), sine = std::sin(radians) * .5773502692f;
    const float diagonal = (1 + 2 * cosine) / 3, side = (1 - cosine) / 3;
    for (unsigned i = 0; i < 3; ++i)
      c[i] = std::clamp(diagonal * rgb[i] + (side - sine) * rgb[(i + 1) % 3] +
                       (side + sine) * rgb[(i + 2) % 3], 0.f, 255.f);
  }
  const float luma = c[0] * .2126f + c[1] * .7152f + c[2] * .0722f;
  Vec4 result{};
  for (unsigned i = 0; i < 3; ++i)
    result[i] = linear(std::clamp(luma + (c[i] - luma) * appearance.saturation, 0.f, 255.f)) *
                appearance.color[i] / 255.f;
  return result;
}
} // namespace

class Meridian {
public:
  static constexpr bool canvas = false;
  Meridian(const A75Create12& create, const json& recipe)
      : diameter_(recipe.at("diameter")), duration_(recipe.at("duration")),
        constants_(create.device, sizeof(Constants)), instances_(create.device, sizeof(Instance) * maxHits),
        pipeline_(create.device, {kMeridianVS, sizeof(kMeridianVS)}, {kMeridianPS, sizeof(kMeridianPS)},
                  Blend::Add, false, {elements, UINT(std::size(elements))}) {
    if (recipe.at("proceduralStyle") != 1 || !std::isfinite(diameter_) || diameter_ < 48 ||
        diameter_ > 500 || !std::isfinite(duration_) || duration_ < .12f || duration_ > 3)
      throw std::runtime_error("Invalid orbital burst recipe");
    hits_.reserve(maxHits);
  }

  void configure(const Appearance& appearance, double) {
    appearance_ = appearance;
    auto field = [&](const char* name, float defaultValue) {
      const auto item = appearance.fields.find(name);
      return item == appearance.fields.end() ? defaultValue : item->second;
    };
    orbitOpacity_ = appearance.foregroundOpacity;
    haloOpacity_ = std::clamp(field("haloOpacity", 100) / 100.f, 0.f, 1.f);
    coreOpacity_ = std::clamp(field("coreOpacity", 100) / 100.f, 0.f, 1.f);
    arcCount_ = std::clamp(field("arcCount", 2), 1.f, 3.f);
    arcWidth_ = field("arcWidth", 1);
    arcSweep_ = field("arcSweep", 1);
    arcEccentricity_ = field("arcEccentricity", .56f);
  }

  void clear() {
    hits_.clear();
    previous_ = -1;
    wasPaused_ = false;
  }

  void key(const A75Event& event) {
    if (!std::isfinite(event.seconds) || !std::isfinite(event.x) || !std::isfinite(event.y) ||
        event.x < 0 || event.x > 1 || event.y < 0 || event.y > 1)
      return;
    expire(event.seconds, life());
    if (hits_.size() == maxHits)
      hits_.erase(hits_.begin());
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    const float seed = float(random_ & 0xffffff) / 16777216.f;
    hits_.push_back({event.seconds, event.x, event.y, seed, serial_++});
  }

  int draw(const A75Frame12& frame, bool paused) {
    const double elapsed = previous_ < 0 ? 0 : std::max(0., frame.seconds - previous_);
    previous_ = frame.seconds;
    if (paused || wasPaused_)
      for (auto& hit : hits_)
        hit.born += elapsed;
    wasPaused_ = paused;
    const float duration = life();
    expire(frame.seconds, duration);
    if (hits_.empty())
      return -1;

    const float budget = hits_.size() > 8 ? std::sqrt(8.f / float(hits_.size())) : 1.f;
    const float diameter = diameter_ * appearance_.scale * frame.height / 720;
    std::array<Instance, maxHits> data{};
    for (unsigned j = 0; j < hits_.size(); ++j) {
      const auto& hit = hits_[j];
      auto& instance = data[j];
      const float t = std::clamp(float((frame.seconds - hit.born) / duration), 0.f, 1.f);
      instance.center = {hit.x, hit.y, t, hit.seed};
      instance.color = colorForHit(hit.palette, appearance_);
      const float growth = std::clamp(t / .82f, 0.f, 1.f);
      const float radius = .018f + .39f * smooth(growth);
      const float angle = hit.seed * 2 * pi + t * .48f * arcSweep_;
      const float head = hit.seed * 2 * pi + t * 4.1f * arcSweep_;
      instance.motion = {radius, std::cos(angle), std::sin(angle), arcEccentricity_};
      instance.data = {std::cos(head), std::sin(head),
                      growth < 1 ? .39f * 6 * growth * (1 - growth) / (.82f * duration) : 0, budget};

      // Tight projected bounds leave room for the filtered glow at all radii.
      float ex = 0, ey = 0;
      for (int k = 0; k < int(arcCount_); ++k) {
        const float a = angle - k * .65f, co = std::cos(a), si = std::sin(a);
        const float ecc = std::max(.25f, arcEccentricity_ - k * .04f), r = radius * (1 - k * .12f);
        ex = std::max(ex, r * std::sqrt(co * co + ecc * ecc * si * si));
        ey = std::max(ey, r * std::sqrt(si * si + ecc * ecc * co * co));
      }
      const float pad = 10.f / diameter, core = .12f * std::exp(-t * 6);
      instance.shape = {std::max(ex, core) + pad, std::max(ey, core) + pad, 0, 0};
    }
    const Constants constants{{float(frame.width), float(frame.height), diameter, coreOpacity_},
                              {appearance_.brightness * appearance_.intensity, orbitOpacity_, 1, haloOpacity_},
                              {1, .5f, arcCount_, arcWidth_}};
    constants_.write(&constants, sizeof(constants));
    const auto bytes = hits_.size() * sizeof(Instance);
    instances_.write(data.data(), bytes);
    pipeline_.bind(frame.commands, constants_);
    D3D12_VERTEX_BUFFER_VIEW view{instances_.address(), UINT(bytes), sizeof(Instance)};
    frame.commands->IASetVertexBuffers(0, 1, &view);
    frame.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    frame.commands->DrawInstanced(4, UINT(hits_.size()), 0, 0);
    return paused ? -1 : 0;
  }

private:
  struct Hit {
    double born;
    float x, y, seed;
    unsigned palette;
  };

  float life() const {
    return (appearance_.duration > 0 ? appearance_.duration : duration_) / appearance_.speed;
  }
  void expire(double time, float duration) {
    hits_.erase(std::remove_if(hits_.begin(), hits_.end(),
                              [&](const Hit& hit) { return time - hit.born >= duration; }), hits_.end());
  }

  const float diameter_, duration_;
  Upload constants_, instances_;
  Pipeline pipeline_;
  std::vector<Hit> hits_;
  Appearance appearance_;
  unsigned serial_ = 0, random_ = 0x48db71a5;
  float orbitOpacity_ = 1, haloOpacity_ = 1, coreOpacity_ = 1;
  float arcCount_ = 2, arcWidth_ = 1, arcSweep_ = 1, arcEccentricity_ = .56f;
  double previous_ = -1;
  bool wasPaused_ = false;
};
} // namespace atlas_packs

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
  return atlas_packs::api12<atlas_packs::Meridian>(abi);
}
