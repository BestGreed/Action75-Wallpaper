#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cmath>

namespace action75 {
using EffectRgb = std::array<std::uint8_t, 3>;
struct EffectPalettePreset {
  std::string id, name;
  std::vector<EffectRgb> colors;
};
inline const std::vector<EffectRgb>& defaultCustomPalette() {
  static const std::vector<EffectRgb> colors{{255,110,155},{77,208,225},{178,147,255}};
  return colors;
}
inline const std::vector<EffectPalettePreset>& effectPalettePresets() {
  static const std::vector<EffectPalettePreset> presets{
    {"original", u8"原作九色", {}},
    {"candy", u8"糖果", {{255,105,180},{84,217,235},{170,131,255},{255,210,92}}},
    {"ocean", u8"海盐", {{33,139,230},{0,188,192},{91,214,231},{77,126,226}}},
    {"sunset", u8"落日", {{255,107,84},{255,170,79},{230,91,144},{157,101,204}}},
    {"forest", u8"森野", {{60,160,112},{133,184,85},{33,130,128},{213,188,104}}},
    {"neon", u8"霓虹", {{255,47,146},{37,229,209},{158,91,255},{218,255,54}}},
    {"vintage", u8"复古", {{188,96,85},{201,155,103},{98,143,132},{132,117,153}}},
    {"ink", u8"墨色", {{36,50,72},{63,86,101},{106,127,135}}},
    {"custom", u8"自定义", defaultCustomPalette()},
  };
  return presets;
}
inline const EffectPalettePreset* findEffectPalette(std::string_view id) {
  for (const auto& palette : effectPalettePresets())
    if (palette.id == id) return &palette;
  return nullptr;
}
inline std::array<float, 3> linearPaletteColor(const EffectRgb& color) {
  std::array<float, 3> result{};
  for (std::size_t i=0; i<result.size(); ++i) {
    const float value=color[i]/255.0f;
    result[i]=value<=.04045f ? value/12.92f : std::pow((value+.055f)/1.055f, 2.4f);
  }
  return result;
}
}
