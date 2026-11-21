#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace action75 {
// Three constant-buffer rows. Build the transform when settings change.
struct LayerColor {
  using Row = std::array<float, 4>;
  std::array<Row, 3> rows{};

  static LayerColor make(float hue, float saturation,
                         std::array<unsigned char, 3> tint = {255, 255, 255}, float gain = 1) {
    LayerColor result;
    if (std::fmod(hue, 360.f) == 0 && saturation == 1 && gain == 1 &&
        tint == std::array<unsigned char, 3>{255, 255, 255}) return result;
    const float radians = hue * .01745329252f;
    const float c = std::cos(radians) * saturation, s = std::sin(radians) * saturation;
    constexpr float luma[]{.2126f, .7152f, .0722f};
    for (int i = 0; i < 3; ++i) {
      const float u = (i == 2 ? 1.f : 0.f) - luma[i];
      const float v = (i == 0 ? 1.f : 0.f) - luma[i];
      const float blue = luma[i] + c*u - s*v, red = luma[i] + s*u + c*v;
      const float scale = gain * tint[i] / 255.f;
      result.rows[0][i] = red * scale;
      result.rows[1][i] = (luma[i] - red*luma[0] - blue*luma[2]) / luma[1] * scale;
      result.rows[2][i] = blue * scale;
    }
    result.rows[0][3] = 1;
    return result;
  }

  Row apply(Row color) const {
    if (!rows[0][3]) return color;
    Row out = color;
    for (int i = 0; i < 3; ++i)
      out[i] = std::max(0.f, rows[i][0]*color[0] + rows[i][1]*color[1] + rows[i][2]*color[2]);
    return out;
  }
};
}
