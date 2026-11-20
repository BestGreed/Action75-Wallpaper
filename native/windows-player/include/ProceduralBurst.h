#pragma once
#include "PackSettings.h"
#include "PixelFluid.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace action75 {
// Time-continuous, bounded, single-pass keypress effects. Pixel geometry keeps
// its grid-shaped silhouette while travelling at subpixel positions.
class ProceduralBurst {
public:
 bool load(ID3D11Device*,unsigned style,float diameter,float duration,std::string&,const std::filesystem::path& shapeFile={});
 void configure(const PackAppearance&);
 bool push(double now,float x,float y);
 bool draw(ID3D11DeviceContext*,unsigned,unsigned,double now,bool paused,std::string&);
 void clear();
 size_t count()const{return hits_.size();}
 size_t particleCount()const{return style_==0?fluid_.particleCount():0;}
 bool isFluid()const{return style_==0;}
private:
 struct Pixel {FluidParticle body;float size,shade,opacity,lifeRand,fadeRand,tx,ty;};
 struct Hit {double born;float x,y,seed;unsigned palette;std::vector<Pixel> pixels;double fluidBorn=0;};
 struct Instance {float center[4],shape[4],motion[4],color[4],data[4];};
 bool loadFilmShapes(const std::filesystem::path&,std::string&);
 bool resizeFilm(unsigned,unsigned,std::string&);
 std::array<std::vector<float>,6> filmShapes_;
 std::array<float,6> filmCoverage_{};
 float filmShape_=0,filmSpread_=1.75f,filmStretch_=1,filmTear_=.65f;
 Microsoft::WRL::ComPtr<ID3D11VertexShader> filmVs_;
 Microsoft::WRL::ComPtr<ID3D11PixelShader> filmPs_;
 Microsoft::WRL::ComPtr<ID3D11BlendState> filmBlend_;
 Microsoft::WRL::ComPtr<ID3D11SamplerState> filmSampler_;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> filmTexture_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> filmView_;
 Microsoft::WRL::ComPtr<ID3D11RenderTargetView> filmTarget_;
 unsigned filmWidth_=0,filmHeight_=0;
 void advanceFluid(double now);
 void syncFluidClock(double now,bool stopped);
 PixelFluid fluid_;
 std::vector<FluidParticle*> activeFluid_;
 double fluidClock_=0,fluidWallLast_=-1;unsigned long long fluidTicks_=0;float fluidFraction_=0;
 float viscosity_=.45f,cohesion_=.8f,pressure_=1,gravity_=.75f,variety_=1;
 float life()const{return (appearance_.spriteDuration>0?appearance_.spriteDuration:duration_)/appearance_.speed;}
 float fluidLife()const{return appearance_.spriteDuration>0?appearance_.spriteDuration:duration_;}
 std::array<float,3> hitColor(unsigned)const;
 Microsoft::WRL::ComPtr<ID3D11Device> device_;
 Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
 Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
 Microsoft::WRL::ComPtr<ID3D11Buffer> constants_,instances_;
 Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
 Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
 Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
 Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
 std::vector<Hit> hits_;
 std::vector<Instance> upload_;
 PackAppearance appearance_;
 unsigned style_=0,serial_=0,random_=0x48db71a5;
 float diameter_=240,duration_=1.35f,pixelSize_=4,density_=.58f,force_=1,drift_=.5f,opacity_=.8f;
 float arcCount_=2,arcWidth_=1,arcSweep_=1,arcEccentricity_=.56f;
 double previous_=-1;bool wasPaused_=false;
};
}
