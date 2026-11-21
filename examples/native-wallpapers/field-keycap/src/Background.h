#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace wallpaper {
using Json = nlohmann::json;
using Rgb = std::array<unsigned char, 3>;
using ColorRow = std::array<float, 4>;

inline Json readJson(const std::filesystem::path &path) {
  std::ifstream input(path);
  return Json::parse(input);
}

struct Background {
  struct Constants {
    std::array<ColorRow, 3> rows{};
    ColorRow solid{0, 0, 0, 1};
  } gpu;
  bool edited = false;
  bool solid = false;

  void configure(const Json &values) {
    const auto tint = values.value("backgroundColor", Rgb{255, 255, 255});
    const float hue = values.value("backgroundHue", 0.f);
    const float saturation = values.value("backgroundSaturation", 1.f);
    const float gain = values.value("backgroundBrightness", 1.f);
    solid = values.value("backgroundStyle", 0) == 1;
    const bool graded = std::fmod(hue, 360.f) != 0 || saturation != 1 || gain != 1 ||
                        tint != Rgb{255, 255, 255};
    const float foreground = std::clamp(values.value("foregroundOpacity", 100.f), 0.f, 100.f) / 100.f;
    const float background = std::clamp(values.value("backgroundOpacity", 100.f), 0.f, 100.f) / 100.f;
    edited = solid || graded || foreground < 1 || background < 1;
    gpu = {};
    gpu.solid = {solid ? 1.f : 0.f, solid ? 1.f : 0.f, solid ? 1.f : 0.f, 1.f};
    if (graded) {
      const float radians = hue * .01745329252f;
      const float cosine = std::cos(radians) * saturation;
      const float sine = std::sin(radians) * saturation;
      constexpr float luma[]{.2126f, .7152f, .0722f};
      for (int channel = 0; channel < 3; ++channel) {
        const float u = (channel == 2 ? 1.f : 0.f) - luma[channel];
        const float v = (channel == 0 ? 1.f : 0.f) - luma[channel];
        const float blue = luma[channel] + cosine * u - sine * v;
        const float red = luma[channel] + sine * u + cosine * v;
        const float scale = gain * tint[channel] / 255.f;
        gpu.rows[0][channel] = red * scale;
        gpu.rows[1][channel] = (luma[channel] - red * luma[0] - blue * luma[2]) / luma[1] * scale;
        gpu.rows[2][channel] = blue * scale;
      }
      gpu.rows[0][3] = 1;
      if (solid)
        for (unsigned channel = 0; channel < 3; ++channel) {
          const auto &row = gpu.rows[channel];
          gpu.solid[channel] = std::clamp(row[0] + row[1] + row[2], 0.f, 1.f);
        }
    }
    gpu.rows[1][3] = solid ? 1.f : 0.f;
    gpu.rows[2][3] = foreground;
    gpu.solid[3] = background;
  }
  ColorRow clearColor() const {
    auto color = gpu.solid;
    for (int i = 0; i < 3; ++i) color[i] *= color[3];
    return color;
  }
};
} // namespace wallpaper
