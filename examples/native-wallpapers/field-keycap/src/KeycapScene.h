#pragma once
#include "Action75Wallpaper12.h"
#include "Background.h"
#include "Gpu12.h"

namespace wallpaper {
class KeycapScene {
public:
  explicit KeycapScene(const A75Create12 &host);
  void configure(const Json &values);
  void press(const A75Event &event);
  void clear();
  int32_t render(const A75Frame12 &frame, bool paused);

private:
  struct Key {
    std::string code;
    float x = 0, y = 0, width = 0, height = 0;
    double start = -1;
    float initial = 0;
  };

  float level(const Key &key, double now) const;
  bool animated() const;

  Graphics12 graphics_;
  ComPtr<ID3D12PipelineState> normal_, graded_;
  Background background_;
  std::array<Key, 84> keys_;
  float attack_ = .045f, hold_ = .07f, release_ = .65f;
  float line_ = 1.2f, radius_ = 8, outline_ = .22f, gain_ = 1.1f, softness_ = .65f;
  float offsetX_ = 0, offsetY_ = 0, scaleX_ = 1, scaleY_ = 1, gap_ = 1;
  float design_ = 0, flowSpeed_ = .45f, detail_ = .65f, chromatic_ = .7f, ambient_ = .35f;
  ColorRow lineColor_{.24f, .55f, .65f, 1}, glowColor_{.25f, .85f, 1, 1};
};
} // namespace wallpaper
