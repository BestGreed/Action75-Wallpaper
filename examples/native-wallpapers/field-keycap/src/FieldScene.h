#pragma once
#include "Action75Wallpaper12.h"
#include "Background.h"
#include "Gpu12.h"

namespace wallpaper {
class FieldScene {
public:
  explicit FieldScene(const A75Create12 &host);
  void configure(const Json &values);
  void press(const A75Event &event);
  void clear();
  int32_t render(const A75Frame12 &frame, bool paused);

private:
  struct Hit { float x = 0, y = 0; double time = -1; };
  struct Arc { float x = 0, y = 0, z = 0, w = 0; double time = -1; float seed = 0; };
  enum Surface : unsigned { State0, State1, Image, Blur0, Blur1, SurfaceCount };
  enum Table : unsigned { Sim0, Sim1, Shade0, Shade1, Horizontal, Vertical, Composite, TableCount };

  void resize(unsigned width, unsigned height);
  void updateTables();
  void recordPalette(ID3D12GraphicsCommandList *commands);
  D3D12_CPU_DESCRIPTOR_HANDLE target(unsigned index) const;
  void bindTable(ID3D12GraphicsCommandList *commands, unsigned index) const;

  ID3D12Device *device_;
  Graphics12 graphics_;
  ComPtr<ID3D12PipelineState> sim_, shade_, composite_, blurHorizontal_, blurVertical_;
  ComPtr<ID3D12PipelineState> gradedShade_, gradedComposite_;
  ComPtr<ID3D12PipelineState> backgroundShade_, foregroundShade_, weightedComposite_, foregroundComposite_;
  ComPtr<ID3D12DescriptorHeap> targets_, textures_;
  unsigned targetStride_ = 0, textureStride_ = 0;
  std::array<ComPtr<ID3D12Resource>, SurfaceCount> surfaces_;
  TextureUpload noise_, palette_;
  bool noisePending_ = true, palettePending_ = true, reset_ = true;

  Background background_;
  Json presets_ = Json::array();
  std::array<std::array<float, 2>, 84> centers_{};
  std::vector<Rgb> colors_;
  std::array<Hit, 24> hits_{};
  std::array<Arc, 32> arcs_{};
  unsigned simWidth_ = 0, simHeight_ = 0, imageWidth_ = 0, imageHeight_ = 0;
  unsigned blurWidth_ = 0, blurHeight_ = 0, front_ = 0, nextHit_ = 0, nextArc_ = 0;
  float lifetime_ = 3, turbulence_ = .9f, propagation_ = 1;
  float brightness_ = 1, intensity_ = 1, scale_ = 1, speed_ = 1;
  float ambient_ = .35f, detail_ = .8f, bloom_ = .6f, sparks_ = .5f, quality_ = 1;
  double previous_ = -1, simulationTime_ = 0, lastImpulse_ = -1;
};
} // namespace wallpaper
