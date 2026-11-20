#pragma once
#include "ProceduralBackground.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>

namespace action75 {
class KeycapEffect {
public:
  bool load(ID3D11Device*,const std::filesystem::path&,std::string&);
  void configure(const PackAppearance&);
  bool push(const std::string& code,double now);
  void clear();
  std::size_t count() const;
  const auto& clearColor() const {return background_.gpu.solid;}
  bool animated() const {
    // Without outlines or spectral color, only liquid ribbons and orbital stars
    // have time-dependent ambient detail. Key pulses are scheduled separately.
    return !background_.solid && flowSpeed_>0 && (outline_>0 ||
        (ambient_>0 && (chromatic_>0 || (design_>=.5f && detail_>0))));
  }
  bool draw(ID3D11DeviceContext*,unsigned,unsigned,double,std::string&);
private:
  ProceduralBackground background_;
  struct Key {std::string code;float x,y,w,h;double start=-1;float initial=0;};
  float level(const Key&,double) const;
  std::vector<Key> keys_;
  float attack_=.045f,hold_=.07f,release_=.65f;
  float line_=1.2f,radius_=8,outline_=.22f,gain_=1.1f,softness_=.65f;
  float offsetX_=0,offsetY_=0,scaleX_=1,scaleY_=1,gap_=1;
  float design_=0,flowSpeed_=.45f,detail_=.65f,chromatic_=.7f,ambient_=.35f;
  std::array<float,4> lineColor_{.24f,.55f,.65f,1},glowColor_{.25f,.85f,1,1};
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_,graded_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
};
}
