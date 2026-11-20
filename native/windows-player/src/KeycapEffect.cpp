#include "KeycapEffect.h"
#include "KeycapShaders.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
using Microsoft::WRL::ComPtr;
namespace action75 {
namespace {
struct alignas(16) Constants {
  float display[4]; // width, height, line width, corner radius
  float style[4]; // outline brightness, glow intensity, softness, unused
  float motion[4]; // time, detail, chromatic dispersion, ambient radiance
  std::array<float,4> lineColor,glowColor;
  float rects[96][4];
  float pulses[96][4];
  ProceduralBackground::Constants background;
};
bool good(HRESULT hr,const char* operation,std::string& error) {
 if(SUCCEEDED(hr))return true;error=operation;return false;
}
}
bool KeycapEffect::load(ID3D11Device* device,const std::filesystem::path& file,std::string& error) {
 try {
  std::ifstream in(file);auto data=nlohmann::json::parse(in);
  if(data.at("coordinateSpace")!=nlohmann::json::array({1920,720}) || data.at("keys").size()!=84)throw std::runtime_error("Expected 84-key 1920x720 layout");
  std::set<std::string> codes;
  for(const auto& k:data.at("keys")) {
   Key a;a.code=k.at("code");a.x=k.at("centerPx").at("x");a.y=k.at("centerPx").at("y");a.w=k.at("sizePx").at("width");a.h=k.at("sizePx").at("height");
   // Caps may extend behind the physical display bezel. Keep their centers and
   // dimensions intact; D3D clips the quads at the viewport, not the layout.
   if(!codes.insert(a.code).second || !std::isfinite(a.x+a.y+a.w+a.h) ||
      a.w<=0 || a.h<=0 || a.w>1920 || a.h>720 ||
      a.x+a.w/2<=0 || a.y+a.h/2<=0 || a.x-a.w/2>=1920 || a.y-a.h/2>=720)
     throw std::runtime_error("Invalid cap bounds");
   keys_.push_back(a);
  }
 }catch(const std::exception& e){error=e.what();return false;}
 if(!good(device->CreateVertexShader(Keycap_vs,sizeof(Keycap_vs),nullptr,&vs_),"Create keycap vertex shader",error) ||
    !good(device->CreatePixelShader(Keycap_ps,sizeof(Keycap_ps),nullptr,&ps_),"Create keycap pixel shader",error))return false;
 D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DYNAMIC;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
 if(!good(device->CreateBuffer(&cb,nullptr,&constants_),"Create keycap constants",error))return false;
 D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
 if(!good(device->CreateRasterizerState(&rs,&raster_),"Create keycap raster",error))return false;
 D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
 return good(device->CreateDepthStencilState(&ds,&depth_),"Create keycap depth",error);
}
void KeycapEffect::configure(const PackAppearance& a) {
 background_.configure(a.keycap);
 auto get=[&](const char* key,float fallback){return a.keycap.value(key,fallback);};
 attack_=get("attack",.045f);hold_=get("hold",.07f);release_=get("release",.65f);
 line_=get("lineWidth",1.2f);radius_=get("radius",8);outline_=get("outline",.22f);
 gain_=a.brightness*a.intensity;softness_=get("softness",.65f);
 design_=get("design",0);flowSpeed_=get("flowSpeed",.45f);detail_=get("detail",.65f);chromatic_=get("chromatic",.7f);ambient_=get("ambient",.35f);
 offsetX_=get("offsetX",0);offsetY_=get("offsetY",0);scaleX_=get("scaleX",1);scaleY_=get("scaleY",1);gap_=get("capScale",1);
 for(int i=0;i<3;++i){glowColor_[i]=a.color[i]/255.f;lineColor_[i]=a.keycapOutlineColor[i]/255.f;}
}
float KeycapEffect::level(const Key& k,double now)const {
 if(k.start<0)return 0;
 const float age=static_cast<float>(std::max(0.,now-k.start));
 auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
 if(age<attack_)return k.initial+(1-k.initial)*smooth(age/attack_);
 if(age<attack_+hold_)return 1;
 return 1-smooth((age-attack_-hold_)/release_);
}
bool KeycapEffect::push(const std::string& code,double now) {
 for(auto& k:keys_)if(k.code==code){k.initial=level(k,now);k.start=now;return true;}return false;
}
void KeycapEffect::clear(){for(auto& k:keys_){k.start=-1;k.initial=0;}}
std::size_t KeycapEffect::count()const {return std::count_if(keys_.begin(),keys_.end(),[](const Key& k){return k.start>=0;});}
bool KeycapEffect::draw(ID3D11DeviceContext* c,unsigned width,unsigned height,double now,std::string& error) {
 if(background_.edited && !graded_){
  ComPtr<ID3D11Device> device;c->GetDevice(&device);
  if(!good(device->CreatePixelShader(Keycap_graded,sizeof(Keycap_graded),nullptr,&graded_),"Create graded keycap shader",error))return false;
 }
 Constants v{};v.background=background_.gpu;v.display[0]=float(width);v.display[1]=float(height);v.display[2]=line_*height/720.f;v.display[3]=radius_*height/720.f;
 v.style[0]=outline_;v.style[1]=gain_;v.style[2]=softness_;v.style[3]=design_;
 v.motion[0]=static_cast<float>(std::fmod(now,4096.))*flowSpeed_;v.motion[1]=detail_;v.motion[2]=chromatic_;v.motion[3]=ambient_;
 v.lineColor=lineColor_;v.glowColor=glowColor_;
 for(std::size_t i=0;i<keys_.size();++i){auto& k=keys_[i];float pulse=level(k,now);if(pulse<=0 && now-k.start>=attack_+hold_+release_)k.start=-1;
  v.rects[i][0]=((k.x-960)*scaleX_+960+offsetX_)*width/1920.f;v.rects[i][1]=((k.y-360)*scaleY_+360+offsetY_)*height/720.f;
  v.rects[i][2]=k.w*scaleX_*gap_*width/1920.f;v.rects[i][3]=k.h*scaleY_*gap_*height/720.f;v.pulses[i][0]=pulse;
  v.pulses[i][1]=k.start>=0?static_cast<float>(now-k.start):0;
 }
 D3D11_MAPPED_SUBRESOURCE mapped{};if(!good(c->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map keycap constants",error))return false;
 std::memcpy(mapped.pData,&v,sizeof(v));c->Unmap(constants_.Get(),0);
 c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(depth_.Get(),0);c->RSSetState(raster_.Get());
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
 c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);
 c->VSSetShader(vs_.Get(),nullptr,0);c->PSSetShader(background_.edited?graded_.Get():ps_.Get(),nullptr,0);auto* cb=constants_.Get();c->VSSetConstantBuffers(0,1,&cb);c->PSSetConstantBuffers(0,1,&cb);
 c->DrawInstanced(4,static_cast<UINT>(keys_.size()),0,0);return true;
}
}
