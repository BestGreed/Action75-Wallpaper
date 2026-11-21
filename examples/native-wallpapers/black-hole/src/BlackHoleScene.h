#pragma once
#include "BlackHoleSimulation.h"
#include <d3d11.h>
#include <string>
#include <wrl/client.h>

namespace action75 {
class BlackHoleScene : public BlackHoleSimulation {
public:
  bool load(ID3D11Device *, ID3D11DeviceContext *, ID3D11ShaderResourceView *artwork,
            std::string &error);
  bool draw(ID3D11DeviceContext *, unsigned width, unsigned height, double now, bool paused,
            std::string &error);

private:
  struct Surface {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  };
  bool resize(ID3D11DeviceContext *, unsigned width, unsigned height, std::string &error);
  Microsoft::WRL::ComPtr<ID3D11VertexShader> screenVs_, starVs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> scenePs_, starPs_, lightSourcePs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> bloomHorizontal_, bloomVertical_, composite_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sky_, skyStars_, turbulence_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> sceneConstants_, starConstants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> skySampler_, turbulenceSampler_, emissionSampler_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> starBlend_;
  Surface hdr_, bloom_[2];
  DXGI_FORMAT hdrFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
  unsigned width_ = 0, height_ = 0, bloomWidth_ = 0, bloomHeight_ = 0;
};
} // namespace action75
