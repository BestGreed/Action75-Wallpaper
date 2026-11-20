#pragma once
#include "ProceduralBackground.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace action75 {
// Stateful half-resolution advection, layered field shading and HDR bloom.
// Each pack owns its state, palette LUT and settings independently.
class FieldEffect {
public:
 bool load(ID3D11Device*,const std::filesystem::path& layout,unsigned theme,std::string& error);
 static void releaseDevicePrograms(ID3D11Device*) noexcept;
 void configure(const PackAppearance&);
 bool push(double now,float x,float y);
 void clear();
 std::size_t count() const;
 bool draw(ID3D11DeviceContext*,unsigned width,unsigned height,double now,bool paused,std::string& error);
private:
 ProceduralBackground background_;
 struct Hit {float x=0,y=0;double time=-1;float seed=0;};
 struct Arc {float x=0,y=0,z=0,w=0;double time=-1;float seed=0;};
 struct Surface {Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;};
 bool loadWordmark(const std::filesystem::path&,std::string&);
 bool resize(unsigned,unsigned,std::string&);
 bool uploadPalette(std::string&);
 Microsoft::WRL::ComPtr<ID3D11Device> device_;
 Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
 Microsoft::WRL::ComPtr<ID3D11PixelShader> simulate_,shade_,bloom_,blurH_,blurV_,gradedShade_,gradedComposite_;
 Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
 Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
 Microsoft::WRL::ComPtr<ID3D11SamplerState> repeat_;
 Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
 Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> palette_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> noise_;
 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> wordmark_;
 std::array<float,4> wordmarkSpace_{813,672,768,113};
 std::array<float,2> wordmarkSize_{};
 std::array<std::array<float,4>,2> wordmarkBounds_{};
 float wordmarkEnabled_=1,wordmarkBrightness_=.65f,wordmarkFlow_=.6f,wordmarkScale_=.95f,wordmarkOffsetY_=0;
 Surface state_[2],image_,blur_[2];
 unsigned snowRandom_=0x6d2b79f5;
 unsigned theme_=0,width_=0,height_=0,front_=0,nextHit_=0,nextArc_=0;
 unsigned imageWidth_=0,imageHeight_=0,blurWidth_=0,blurHeight_=0;
 std::array<Hit,24> hits_{};
 std::array<Arc,32> arcs_{};
 std::vector<std::array<float,2>> centers_;
 std::vector<EffectRgb> paletteColors_;
 bool paletteDirty_=true,reset_=true;
 float lifetime_=3,turbulence_=.9f,propagation_=1,brightness_=1.4f,intensity_=1,scale_=1,speed_=.6f;
 float ambient_=.35f,detail_=.8f,bloomAmount_=.6f,sparks_=.5f,quality_=1;
 float diversity_=1,density_=.45f,opacityVariation_=.85f;
 float thickness_=.8f,refraction_=.7f,variation_=.8f,melt_=.7f;
 double previous_=-1,simulationTime_=0,pausedAge_=0;
};
}
