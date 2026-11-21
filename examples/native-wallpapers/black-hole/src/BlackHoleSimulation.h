#pragma once
#include "BlackHoleAppearance.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace action75 {
namespace black_hole {
inline constexpr unsigned SkyWidth = 2048, SkyHeight = 768, NoiseSize = 128;

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
  std::array<float,4> opacity;
};
struct StarConstants {
  float x, y, radius, rotation;
  float red, green, blue, envelope;
  float age, seed, temperature, lensMix;
  float echoRed, echoGreen, echoBlue, echoEnvelope;
  float echoRadius, padding[3];
};
static_assert(sizeof(SceneConstants) == 192 && sizeof(StarConstants) == 80);

std::array<std::uint8_t, NoiseSize * NoiseSize * 2> turbulenceTexture() noexcept;
} // namespace black_hole

class BlackHoleSimulation {
public:
  using Position = std::array<float, 2>;
  static constexpr std::size_t MaxStars = 96;

  void configure(const BlackHoleAppearance &, float starSize, float starBrightness, bool updateCenter = true);
  Position center() const noexcept { return appearance_.center; }
  void setCenter(Position center) noexcept;
  bool covers(Position point, unsigned width, unsigned height) const noexcept;
  bool push(double now, Position point, unsigned width, unsigned height) noexcept;
  void clear() noexcept { count_ = 0; }
  std::size_t count() const noexcept { return count_; }
  bool animates() const noexcept {
    return appearance_.skyOpacity > 0 || appearance_.holeOpacity > 0 || (appearance_.starOpacity > 0 && count_);
  }

protected:
  unsigned prepareFrame(unsigned width, unsigned height, unsigned bloomWidth, unsigned bloomHeight,
                        double now, bool paused, black_hole::SceneConstants &scene,
                        black_hole::StarConstants *instances);

private:
  struct HeatState {
    float weight = 0;
    std::array<float, 3> spectrum{};
    void decay(float factor) noexcept;
    void deposit(float amount, const std::array<float, 3> &color) noexcept;
  };
  struct PulseState {
    double time = 0;
    HeatState fast, retained;
    HeatState emission(double now) const noexcept;
  };
  struct Star {
    Position point{};
    double born = 0;
    float radius = 0, rotation = 0, seed = 0;
    PulseState pulse;
    std::array<PulseState, 16> history{};
    std::size_t nextHistory = 0, historyCount = 0;
    void addHeat(double now, const std::array<float, 3> &color) noexcept;
    HeatState emission(double now) const noexcept;
    bool expired(double now) const noexcept;
  };
  float shadowPixels(unsigned width, unsigned height) const noexcept;
  std::uint32_t random() noexcept;
  float randomUnit() noexcept;

  std::array<Star, MaxStars> stars_{};
  std::size_t count_ = 0;
  std::uint32_t rng_ = 0xdf93e507;
  std::size_t lastColor_ = 0;
  BlackHoleAppearance appearance_;
  float starSize_ = 1, starBrightness_ = 1;
  std::array<double, 4> skyPhase_{};
  double previousFrame_ = -1, phase_ = 0;
  bool wasPaused_ = false;
};
} // namespace action75
