#include "BlackHoleSimulation.h"
#include <algorithm>
#include <cmath>

namespace action75 {
using namespace black_hole;
namespace {
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

std::array<std::uint8_t, NoiseSize * NoiseSize * 2> black_hole::turbulenceTexture() noexcept {
  std::array<std::uint8_t, NoiseSize * NoiseSize * 2> noise{};
  std::uint32_t seed = 0xc41c64ad;
  for (auto &sample : noise) {
    seed = seed * 1664525u + 1013904223u;
    sample = static_cast<std::uint8_t>(seed >> 24);
  }
  return noise;
}

void BlackHoleSimulation::configure(const BlackHoleAppearance &profile, float starSize, float starBrightness, bool updateCenter) {
  const Position previous = center();
  appearance_ = profile;
  setCenter(updateCenter ? appearance_.center : previous);
  starSize_ = starSize;
  starBrightness_ = starBrightness;
}

void BlackHoleSimulation::setCenter(Position point) noexcept {
  for (auto &axis : point)
    axis = std::clamp(axis, 0.f, 1.f);
  appearance_.center = point;
}

float BlackHoleSimulation::shadowPixels(unsigned width, unsigned height) const noexcept {
  return ShadowFraction * appearance_.size * static_cast<float>(std::min(width, height));
}

void BlackHoleSimulation::HeatState::decay(float factor) noexcept {
  weight *= factor;
  for (auto &channel : spectrum)
    channel *= factor;
}

void BlackHoleSimulation::HeatState::deposit(float amount, const std::array<float, 3> &color) noexcept {
  weight += amount;
  for (std::size_t channel = 0; channel < spectrum.size(); ++channel)
    spectrum[channel] += amount * color[channel];
}

void BlackHoleSimulation::Star::addHeat(double now, const std::array<float, 3> &color) noexcept {
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

BlackHoleSimulation::HeatState BlackHoleSimulation::PulseState::emission(double now) const noexcept {
  const float elapsed = static_cast<float>(now - time);
  const float cooled = std::exp(-elapsed / CoolingSeconds);
  const float transferred = std::exp(-elapsed / HeatTransferSeconds);
  HeatState result{retained.weight * cooled - fast.weight * transferred};
  for (std::size_t channel = 0; channel < result.spectrum.size(); ++channel)
    result.spectrum[channel] = retained.spectrum[channel] * cooled - fast.spectrum[channel] * transferred;
  return result;
}

BlackHoleSimulation::HeatState BlackHoleSimulation::Star::emission(double now) const noexcept {
  if (now >= pulse.time)
    return pulse.emission(now);
  for (std::size_t offset = 0; offset < historyCount; ++offset) {
    const auto index = (nextHistory + history.size() - 1 - offset) % history.size();
    if (history[index].time <= now)
      return history[index].emission(now);
  }
  return {};
}

bool BlackHoleSimulation::Star::expired(double now) const noexcept {
  return now - pulse.time >= StarLifetime + MaxSecondaryDelay;
}

bool BlackHoleSimulation::covers(Position point, unsigned width, unsigned height) const noexcept {
  const auto position = center();
  const float x = (point[0] - position[0]) * static_cast<float>(width);
  const float y = (point[1] - position[1]) * static_cast<float>(height);
  const float radius = shadowPixels(width, height);
  return x * x + y * y <= radius * radius;
}

std::uint32_t BlackHoleSimulation::random() noexcept {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

float BlackHoleSimulation::randomUnit() noexcept {
  return static_cast<float>(random() >> 8) / 16777216.f;
}

bool BlackHoleSimulation::push(double now, Position point, unsigned width, unsigned height) noexcept {
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


unsigned BlackHoleSimulation::prepareFrame(unsigned width, unsigned height, unsigned bloomWidth, unsigned bloomHeight,
                                           double now, bool paused, SceneConstants &scene, StarConstants *instances) {
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
  const auto position = center();
  const float einsteinRadiusSquared = 2.92f * appearance_.lensStrength;
  const float lensEnabled = smooth(0, .42f, appearance_.lensStrength);
  scene = {canvasWidth, canvasHeight, position[0] * canvasWidth,
                       position[1] * canvasHeight, shadowPixels(width, height),
                       appearance_.lensStrength, appearance_.diskBrightness, static_cast<float>(phase_),
                       SkyWidth * skyScale, SkyHeight * skyScale, appearance_.starFlare, 0,
                       1.f / static_cast<float>(bloomWidth), 1.f / static_cast<float>(bloomHeight), einsteinRadiusSquared, 0,
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

  const auto end = std::remove_if(stars_.begin(), stars_.begin() + count_, [&](const Star &star) {
    return star.expired(now);
  });
  count_ = static_cast<std::size_t>(end - stars_.begin());
  unsigned visibleStars = 0;
  if (!paused && count_ && starBrightness_ > 0) {
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
  }
  return visibleStars;
}
} // namespace action75
