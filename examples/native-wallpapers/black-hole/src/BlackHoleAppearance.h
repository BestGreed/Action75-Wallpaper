#pragma once
#include <array>
#include <cstdint>
namespace action75 {
using EffectRgb = std::array<std::uint8_t,3>;
struct BlackHoleSkyStyle {
  float brightness, speed;
  EffectRgb tint{255,255,255};
};
struct BlackHoleAppearance {
  enum class DiskPalette : unsigned { Warm, Hot, Radio };
  enum class Sky : unsigned { Nebula, Void, Stars, Solid };
  std::array<float,2> center{.58f,.46f};
  float size=1, lensStrength=1, diskBrightness=1, starFlare=1.25f;
  DiskPalette diskPalette=DiskPalette::Warm;
  Sky sky=Sky::Nebula;
  BlackHoleSkyStyle nebula{1,.75f}, voidSky{1,.65f}, starfield{1,.8f};
  float skyBrightness=1, skyHue=0, skySaturation=1, starHue=0, starSaturation=1;
  EffectRgb skyTint{255,255,255}, starTint{255,255,255};
};
}
