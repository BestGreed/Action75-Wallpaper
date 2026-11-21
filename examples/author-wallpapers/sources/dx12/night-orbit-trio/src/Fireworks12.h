#pragma once
#include "SceneSettings.h"
#include "Action75Wallpaper12.h"
#include <wrl/client.h>
#include <filesystem>
#include <string>
namespace action75 {
class Fireworks12 {
public:
  static constexpr unsigned maxBursts=112,patternSize=384,maxVisibleSparks=5600;
  bool load(ID3D12Device*,unsigned,unsigned,const std::filesystem::path&);
  void configure(const PackAppearance&);
  bool readyForKeyBurst() noexcept{return playback_.readyForKeyBurst();}
  bool push(double,float,float) noexcept;
  void clear() noexcept;
  unsigned count()const noexcept{return count_;}
  unsigned particleCount()const noexcept{return visibleSparks_;}
  unsigned lastShape()const noexcept{return lastShape_;}
  bool draw(const A75Frame12&,double);
private:
  using Float4=std::array<float,4>;
  static constexpr unsigned colorsPerPalette=9,paletteSlots=1+FireworksAppearance::paletteLimit;
  struct DrawBurst {Float4 location,motion,rotation,palette;};
  struct Burst {double time,expires;DrawBurst render;float speed;unsigned paletteSlot;};
  struct Frame {
    Float4 screen,exposure,atmosphere,background;
    LayerColor skyGrade,sparkGrade;
    std::array<Float4,paletteSlots*colorsPerPalette> palette;
    std::array<DrawBurst,maxBursts> burst;
  };
  bool resize(unsigned,unsigned);
  void discardExpired(double) noexcept;
  unsigned choosePalette() noexcept;
  float random() noexcept;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv(unsigned)const;
  D3D12_GPU_DESCRIPTOR_HANDLE srv(unsigned)const;
  void filter(ID3D12GraphicsCommandList*,unsigned,unsigned,const Float4&);
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> spark_,blur_,composite_,idle_;
  Microsoft::WRL::ComPtr<ID3D12Resource> patterns_,patternUpload_,frame_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> renderViews_,resources_;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,3> light_;
  unsigned char*constants_=nullptr;
  bool uploaded_=false;
  UINT rtvStride_=0,srvStride_=0;
  DXGI_FORMAT format_=DXGI_FORMAT_R16G16B16A16_FLOAT;
  unsigned width_=0,height_=0,blurWidth_=0,blurHeight_=0;
  std::array<Burst,maxBursts> bursts_{};
  std::array<std::array<Float4,patternSize>,FireworksAppearance::shapeCount> timings_{};
  std::array<float,FireworksAppearance::shapeCount> lifespan_{};
  Frame data_{};
  std::array<unsigned,paletteSlots> paletteSizes_{};
  FireworksAppearance appearance_;
  VariantPlaybackState playback_{appearance_};
  float brightness_=1,scale_=1,speed_=1,intensity_=1;
  unsigned paletteChoices_=0,count_=0,visibleSparks_=0,lastShape_=0,lastPalette_=0;
  std::uint32_t randomState_=0xc2ab615du;
};
}
