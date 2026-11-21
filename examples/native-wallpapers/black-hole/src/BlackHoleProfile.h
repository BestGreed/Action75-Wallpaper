#pragma once
#include "BlackHoleAppearance.h"
#include <json.hpp>
#include <algorithm>
#include <string>

namespace action75 {
struct BlackHoleProfile {
  BlackHoleAppearance blackHole;
  float starSize, starBrightness;

  static BlackHoleProfile fromSettings(const nlohmann::json &profile) {
    BlackHoleProfile result;
    auto &appearance = result.blackHole;
    appearance.sky = static_cast<BlackHoleAppearance::Sky>(std::clamp(profile.value("skyMode", 0), 0, 3));
    appearance.skyBrightness = profile.value("backgroundBrightness", 1.f);
    appearance.skyHue = profile.value("backgroundHue", 0.f);
    appearance.skySaturation = profile.value("backgroundSaturation", 1.f);
    appearance.skyTint = profile.value("backgroundColor", appearance.skyTint);
    appearance.starHue = profile.value("starHue", 0.f);
    appearance.starSaturation = profile.value("starSaturation", 1.f);
    appearance.starTint = profile.value("starColor", appearance.starTint);
    appearance.diskPalette = static_cast<BlackHoleAppearance::DiskPalette>(std::clamp(profile.value("diskPalette", 0), 0, 2));
    appearance.starFlare = profile.value("starFlare", appearance.starFlare);
    appearance.size = profile.value("holeSize", appearance.size);
    appearance.lensStrength = profile.value("lensStrength", appearance.lensStrength);
    appearance.diskBrightness = profile.value("diskBrightness", appearance.diskBrightness);
    appearance.skyOpacity = std::clamp(profile.value("backgroundOpacity", 100.f), 0.f, 100.f) / 100.f;
    appearance.holeOpacity = std::clamp(profile.value("holeOpacity", 100.f), 0.f, 100.f) / 100.f;
    appearance.starOpacity = std::clamp(profile.value("starOpacity", 100.f), 0.f, 100.f) / 100.f;
    appearance.center = {profile.value("centerX", 58.f) / 100.f, profile.value("centerY", 46.f) / 100.f};
    const auto setSky = [&](BlackHoleSkyStyle &style, std::string prefix) {
      style.brightness = profile.value(prefix + "Brightness", style.brightness);
      style.speed = profile.value(prefix + "Speed", style.speed);
      style.tint = profile.value(prefix + "Tint", style.tint);
    };
    setSky(appearance.nebula, "nebula");
    setSky(appearance.voidSky, "void");
    setSky(appearance.starfield, "starfield");
    result.starSize = profile.value("starSize", 1.f);
    result.starBrightness = profile.value("starBrightness", 1.f);
    return result;
  }
};
}
