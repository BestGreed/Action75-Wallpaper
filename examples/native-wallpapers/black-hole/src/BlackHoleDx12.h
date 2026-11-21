#pragma once
#include "Action75Wallpaper12.h"
#include "BlackHoleSimulation.h"
#include <wrl/client.h>

namespace action75 {
class BlackHoleDx12 : public BlackHoleSimulation {
public:
  BlackHoleDx12(ID3D12Device *, const wchar_t *directory);
  void draw(const A75Frame12 &, bool paused);

private:
  template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  static constexpr unsigned MipLevels = 12; // 2048 wide, down to 1.
  enum Srv : UINT {
    NullSrv, ArtworkSrv, NoiseSrv, HdrSrv, BloomASrv, BloomBSrv, SkySrv, StarsSrv,
    SkyMipSrv, StarsMipSrv = SkyMipSrv + MipLevels, SrvCount = StarsMipSrv + MipLevels
  };
  enum Rtv : UINT {
    HdrRtv, BloomARtv, BloomBRtv, SkyRtv, StarsRtv = SkyRtv + MipLevels,
    RtvCount = StarsRtv + MipLevels
  };
  struct Upload {
    ComPtr<ID3D12Resource> texture, buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    void record(ID3D12GraphicsCommandList *) const;
  };

  void createRootSignature();
  void createPipelineStates(bool translucent = false);
  void resize(unsigned width, unsigned height, DXGI_FORMAT sceneFormat);
  void upload(Upload &, DXGI_FORMAT, unsigned width, unsigned height, unsigned rowBytes,
              const void *, Srv);
  void createSky(ID3D12Resource *, DXGI_FORMAT, Rtv firstRtv, Srv all, Srv firstMip);
  void bakeSky(ID3D12GraphicsCommandList *, ID3D12Resource *, Rtv firstRtv, Srv firstMip,
               ID3D12PipelineState *bake, ID3D12PipelineState *mip);
  void bindTexture(ID3D12GraphicsCommandList *, UINT shaderRegister, UINT index) const;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT index) const;
  void writeSrv(ID3D12Resource *, DXGI_FORMAT, UINT slot, UINT firstMip, UINT mips);
  void writeRtv(ID3D12Resource *, DXGI_FORMAT, UINT slot, UINT mip);

  ID3D12Device *device_;
  ComPtr<ID3D12DescriptorHeap> rtvs_, srvs_;
  UINT rtvStride_ = 0, srvStride_ = 0;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> scene_, star_, lightSource_, blurHorizontal_, blurVertical_, composite_;
  ComPtr<ID3D12PipelineState> translucentScene_, translucentStar_, translucentComposite_;
  ComPtr<ID3D12PipelineState> bakeArtwork_, bakeStars_, mipArtwork_, mipStars_;
  ComPtr<ID3D12Resource> constants_, sky_, stars_, hdr_, bloomA_, bloomB_;
  unsigned char *mapped_ = nullptr;
  Upload artwork_, noise_;
  bool baked_ = false;
  DXGI_FORMAT hdrFormat_, starFormat_;
  DXGI_FORMAT sceneFormat_ = DXGI_FORMAT_UNKNOWN;
  unsigned width_ = 0, height_ = 0;
};
} // namespace action75
