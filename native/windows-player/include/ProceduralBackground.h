#pragma once
#include "LayerColor.h"
#include "PackSettings.h"

namespace action75 {
struct ProceduralBackground {
  struct Constants { LayerColor grade; LayerColor::Row solid{}; } gpu;
  bool edited=false, solid=false;
  void configure(const nlohmann::json& values) {
    const auto tint=values.value("backgroundColor",EffectRgb{255,255,255});
    gpu.grade=LayerColor::make(values.value("backgroundHue",0.f),
        values.value("backgroundSaturation",1.f),tint,values.value("backgroundBrightness",1.f));
    solid=values.value("backgroundStyle",0)==1;
    edited=solid || gpu.grade.rows[0][3]!=0;
    gpu.grade.rows[1][3]=solid?1.f:0.f;
    gpu.solid=gpu.grade.apply(solid?LayerColor::Row{1,1,1,1}:LayerColor::Row{0,0,0,1});
    for(unsigned i=0;i<3;++i)gpu.solid[i]=std::min(gpu.solid[i],1.f);
  }
};
}
