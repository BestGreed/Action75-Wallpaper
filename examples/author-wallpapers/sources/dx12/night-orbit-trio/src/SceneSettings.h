#pragma once
// The original FireworksScene appearance defaults, kept package-side. No host headers.
#include "LayerColor.h"
#include "VariantPlayback.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace action75 {
using EffectRgb = std::array<std::uint8_t, 3>;
struct FireworksAppearance : VariantPlaybackRules {
  static constexpr unsigned shapeCount = maxVariants, heartShape = 8, paletteLimit = 8;
  static constexpr std::array<unsigned, shapeCount> legacyOrder{1,4,8,2,7,3,5,6};
  FireworksAppearance() : VariantPlaybackRules{0,0b10001111,1,legacyOrder,false} {}
  unsigned quality=1;
  bool randomPalette=false;
  std::vector<std::vector<EffectRgb>> paletteChoices;
  float density=.82f,trail=1,gravity=1,lifetime=1,shimmer=.85f;
  float embers=1,bloom=.82f,smoke=.55f,reflection=.56f,horizon=.87f;
};
struct PackAppearance {
  FireworksAppearance fireworks;
  float brightness=1,scale=1,speed=1,intensity=1,hue=0,saturation=1;
  unsigned backgroundStyle=0;
  float backgroundBrightness=1,backgroundHue=0,backgroundSaturation=1;
  float backgroundOpacity=1,foregroundOpacity=1;
  EffectRgb backgroundColor{255,255,255};
  bool paletteOverride=false;
  std::vector<EffectRgb> resolvedPaletteColors;
};
class Configuration {
  nlohmann::json values_={{"sky_program",0},{"willow_arc",true},{"palm_fan",true},
    {"saturn_ring",true},{"mix_stars",false},{"tap_cadence",2},
    {"backgroundStyle",0},{"backgroundBrightness",1},{"backgroundHue",0},
    {"backgroundSaturation",1},{"backgroundColor",{255,255,255}},
    {"backgroundOpacity",100},{"foregroundOpacity",100}};
public:
  PackAppearance appearance;
  Configuration() { apply("{}"); }
  void apply(const char* text) {
    auto next=values_;
    if(text && *text) next.update(nlohmann::json::parse(text));
    PackAppearance a;
    a.fireworks.shape=next.at("sky_program").get<unsigned>();
    if(a.fireworks.shape!=0 && a.fireworks.shape!=1 && a.fireworks.shape!=4 && a.fireworks.shape!=7)
      throw std::runtime_error("Invalid night-sky shape");
    a.fireworks.selected=(next.at("willow_arc").get<bool>()?1u:0u) |
      (next.at("palm_fan").get<bool>()?(1u<<3):0u) |
      (next.at("saturn_ring").get<bool>()?(1u<<6):0u);
    a.fireworks.order={7,1,4};
    a.fireworks.keyInterval=std::clamp(next.at("tap_cadence").get<unsigned>(),1u,99u);
    a.fireworks.shuffle=next.at("mix_stars").get<bool>();
    a.backgroundStyle=std::clamp(next.at("backgroundStyle").get<unsigned>(),0u,1u);
    a.backgroundBrightness=std::clamp(next.at("backgroundBrightness").get<float>(),0.f,2.f);
    a.backgroundHue=std::clamp(next.at("backgroundHue").get<float>(),0.f,360.f);
    a.backgroundSaturation=std::clamp(next.at("backgroundSaturation").get<float>(),0.f,2.f);
    a.backgroundOpacity=std::clamp(next.at("backgroundOpacity").get<float>(),0.f,100.f)*.01f;
    a.foregroundOpacity=std::clamp(next.at("foregroundOpacity").get<float>(),0.f,100.f)*.01f;
    const auto rgb=next.at("backgroundColor").get<std::array<int,3>>();
    for(unsigned i=0;i<3;++i) a.backgroundColor[i]=static_cast<std::uint8_t>(std::clamp(rgb[i],0,255));
    values_=std::move(next);appearance=std::move(a);
  }
};
}
