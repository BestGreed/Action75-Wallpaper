#include "FieldEffect.h"
#include "FieldShaders.h"
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace action75 {
namespace {
constexpr unsigned TileColumns = 32, TileRows = 12;
constexpr unsigned TileVectors = TileColumns * TileRows / 4;
using TileMasks = std::array<std::array<std::uint32_t, 4>, TileVectors>;
// Keep the grid dimensions in the shader's tileIndex and constant buffer in sync.
static_assert(TileColumns == 32 && TileRows == 12 && TileVectors == 96);

void markTiles(TileMasks &masks, unsigned element, float left, float top, float right, float bottom) {
 const auto tile = [](float value, unsigned dimension) {
  return static_cast<unsigned>(std::clamp(value * static_cast<float>(dimension),
                                         0.f, static_cast<float>(dimension - 1)));
 };
 const unsigned x0=tile(left,TileColumns),x1=tile(right,TileColumns);
 const unsigned y0=tile(top,TileRows),y1=tile(bottom,TileRows);
 const std::uint32_t mask=std::uint32_t{1}<<element;
 for(unsigned y=y0;y<=y1;++y)
  for(unsigned x=x0;x<=x1;++x){
   const unsigned index=y*TileColumns+x;
   masks[index/4][index%4]|=mask;
  }
}

struct alignas(16) Constants {
 float clock[4]; // time, dt, aspect, theme
 float dynamics[4]; // persistence, turbulence, radius, propagation
 float look[4]; // brightness, intensity, ambient, detail
 float finish[4]; // bloom, sparks, quality, arc count
 float geometry[4]; // compact hit count, bloom texel x/y, unused
 float hits[24][4]; // uv, age, envelope
 float hitInfo[24][4]; // seed, cos(rotation), sin(rotation), maximum growth radius
 float wordmark[4]; // enabled, brightness, flow, reserved
 float wordmarkRect[2][4]; // destination origin and size in normalized screen coordinates
 float wordmarkUv[2][4]; // source origin and size
 float variety[4]; // morphology diversity, branch density, opacity variation
 float material[4]; // thickness, refraction, variation, melt softness
 float snowInfo[24][4]; // normalized lifetime, reserved
 float arcs[32][4];
 float arcInfo[32][4];
 float arcAxis[32][4];
 float arcShape[32][4];
 TileMasks simulationTiles, imprintTiles, arcTiles;
 ProceduralBackground::Constants background;
};
bool good(HRESULT hr,const char* where,std::string& error){if(SUCCEEDED(hr))return true;error=where;return false;}
using Color=std::array<float,3>;
float linear(float c){c/=255;return c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f);}
Color lab(const EffectRgb& rgb){float r=linear(rgb[0]),g=linear(rgb[1]),b=linear(rgb[2]);float l=std::cbrt(.4122214708f*r+.5363325363f*g+.0514459929f*b),m=std::cbrt(.2119034982f*r+.6806995451f*g+.1073969566f*b),s=std::cbrt(.0883024619f*r+.2817188376f*g+.6299787005f*b);return {.2104542553f*l+.793617785f*m-.0040720468f*s,1.9779984951f*l-2.428592205f*m+.4505937099f*s,.0259040371f*l+.7827717662f*m-.808675766f*s};}
Color fromLab(Color c){float l=c[0]+.3963377774f*c[1]+.2158037573f*c[2],m=c[0]-.1055613458f*c[1]-.0638541728f*c[2],s=c[0]-.0894841775f*c[1]-1.291485548f*c[2];l=l*l*l;m=m*m*m;s=s*s*s;return {4.0767416621f*l-3.3077115913f*m+.2309699292f*s,-1.2684380046f*l+2.6097574011f*m-.3413193965f*s,-.0041960863f*l-.7034186147f*m+1.707614701f*s};}
Color mapped(Color c){auto within=[](Color v){return *std::min_element(v.begin(),v.end())>=0 && *std::max_element(v.begin(),v.end())<=1;};auto rgb=fromLab(c);if(within(rgb))return rgb;float lo=0,hi=1;for(int i=0;i<16;++i){float k=(lo+hi)*.5f;if(within(fromLab({c[0],c[1]*k,c[2]*k})))lo=k;else hi=k;}return fromLab({c[0],c[1]*lo,c[2]*lo});}

struct FieldProgram {
 ComPtr<ID3D11VertexShader> vs;
 ComPtr<ID3D11PixelShader> sim, shade, bloom, blurH, blurV;
};
struct FieldProgramCache {
 ComPtr<ID3D11Device> device;
 std::array<FieldProgram, 4> themes;
 ComPtr<ID3D11ShaderResourceView> noise;
};
FieldProgramCache &fieldPrograms() {
 static FieldProgramCache cache;
 return cache;
}
}

void FieldEffect::releaseDevicePrograms(ID3D11Device *device) noexcept {
 auto &cache = fieldPrograms();
 if (cache.device.Get() == device)
  cache = {};
}

bool FieldEffect::load(ID3D11Device* device,const std::filesystem::path& file,unsigned theme,std::string& error){
 if(theme>3 || !device){error="Invalid field theme";return false;}device_=device;theme_=theme;
 try{std::ifstream in(file);auto layout=nlohmann::json::parse(in);for(const auto& key:layout.at("keys")){centers_.push_back({key.at("centerPx").at("x").get<float>()/1920,key.at("centerPx").at("y").get<float>()/720});if(key.at("code")=="Space")wordmarkSpace_={key.at("centerPx").at("x").get<float>(),key.at("centerPx").at("y").get<float>(),key.at("sizePx").at("width").get<float>(),key.at("sizePx").at("height").get<float>()};}if(centers_.size()!=84)throw std::runtime_error("Field needs 84 key centers");}catch(const std::exception& e){error=e.what();return false;}
 if(theme_==2 && !loadWordmark(file.parent_path()/"wordmark.png",error))return false;
 // Pack switches share identical programs on the render-owner thread. Reuse
 // the driver objects across loads on the same device.
 auto &cache = fieldPrograms();
 if(cache.device.Get()!=device){cache=FieldProgramCache{};cache.device=device;}
 if(!cache.noise){
  std::vector<std::uint16_t> pixels(256*256);std::uint32_t seed=0x5a17d3u;
  for(auto& pixel:pixels){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;pixel=static_cast<std::uint16_t>(seed>>16);}
  D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=256;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R16_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=256*sizeof(std::uint16_t);ComPtr<ID3D11Texture2D> texture;
  if(!good(device->CreateTexture2D(&td,&data,&texture),"Create shared noise",error)||!good(device->CreateShaderResourceView(texture.Get(),nullptr,&cache.noise),"Upload shared noise",error))return false;
 }
 noise_=cache.noise;
 auto& program=cache.themes[theme];
 if(program.vs && program.sim && program.shade && program.bloom && program.blurH && program.blurV){
  vs_=program.vs;simulate_=program.sim;shade_=program.shade;bloom_=program.bloom;blurH_=program.blurH;blurV_=program.blurV;
 } else {
  const auto &code = FieldShaders[theme];
  if(!good(device->CreateVertexShader(code.vs.data,code.vs.size,nullptr,&vs_),"Create field vertex shader",error))return false;
  auto pixel=[&](FieldBytecode shader,ComPtr<ID3D11PixelShader>& result){
   return good(device->CreatePixelShader(shader.data,shader.size,nullptr,&result),"Create field pixel shader",error);
  };
  if(!pixel(code.sim,simulate_) || !pixel(code.shade,shade_) || !pixel(code.composite,bloom_) ||
     !pixel(code.blurHorizontal,blurH_) || !pixel(code.blurVertical,blurV_))return false;
  program={vs_,simulate_,shade_,bloom_,blurH_,blurV_};
 }
 D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DYNAMIC;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
 if(!good(device->CreateBuffer(&cb,nullptr,&constants_),"Create field constants",error))return false;
 D3D11_SAMPLER_DESC ss{};ss.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;ss.AddressU=ss.AddressV=ss.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;ss.MaxLOD=D3D11_FLOAT32_MAX;
 if(!good(device->CreateSamplerState(&ss,&sampler_),"Create field sampler",error))return false;
 ss.AddressU=ss.AddressV=ss.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;if(!good(device->CreateSamplerState(&ss,&repeat_),"Create noise sampler",error))return false;
 D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;if(!good(device->CreateRasterizerState(&rs,&raster_),"Create field raster",error))return false;
 D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;return good(device->CreateDepthStencilState(&ds,&depth_),"Create field depth",error);
}
bool FieldEffect::loadWordmark(const std::filesystem::path& file,std::string& error){
 ComPtr<IWICImagingFactory> wic;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic));
 if(SUCCEEDED(hr))hr=wic->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);
 UINT width=0,height=0;if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
 if(!good(hr,"Open snow wordmark PNG",error))return false;
 if(!width||!height||width>8192||height>8192){error="Invalid snow wordmark dimensions";return false;}
 hr=wic->CreateFormatConverter(&converter);
 if(SUCCEEDED(hr))hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4);
 if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data());
 if(!good(hr,"Decode snow wordmark",error))return false;
 for(unsigned part=0;part<2;++part){
  UINT left=width,top=height,right=0,bottom=0;
  for(UINT y=0;y<height;++y)for(UINT x=part?width/2:0;x<(part?width:width/2);++x){
   if(pixels[(static_cast<size_t>(y)*width+x)*4+3]<=8)continue;
   left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
  }
  if(left==width){error="Snow wordmark needs a visible word on each side";return false;}
  left=left>2?left-2:0;top=top>2?top-2:0;right=std::min(width,right+3);bottom=std::min(height,bottom+3);
  wordmarkBounds_[part]={float(left),float(top),float(right-left),float(bottom-top)};
 }
 wordmarkSize_={float(width),float(height)};
 D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=width*4;ComPtr<ID3D11Texture2D> texture;
 return good(device_->CreateTexture2D(&td,&data,&texture),"Create snow wordmark texture",error)&&good(device_->CreateShaderResourceView(texture.Get(),nullptr,&wordmark_),"Upload snow wordmark",error);
}
void FieldEffect::configure(const PackAppearance& a){background_.configure(a.field);auto get=[&](const char* k,float fallback){return a.field.value(k,fallback);};wordmarkEnabled_=get("wordmarkEnabled",1.f);wordmarkBrightness_=get("wordmarkBrightness",.65f);wordmarkFlow_=get("wordmarkFlow",.6f);wordmarkScale_=get("wordmarkScale",.95f);wordmarkOffsetY_=get("wordmarkOffsetY",0.f);diversity_=get("diversity",1.f);density_=get("density",.45f);opacityVariation_=get("opacityVariation",.85f);thickness_=get("thickness",.8f);refraction_=get("refraction",.7f);variation_=get("variation",.8f);melt_=get("melt",.7f);lifetime_=get("persistence",3);turbulence_=get("turbulence",.9f);propagation_=get("propagation",1);ambient_=get("ambient",.35f);detail_=get("detail",.8f);bloomAmount_=get("bloom",.6f);sparks_=get("sparks",.5f);quality_=get("quality",1);brightness_=a.brightness;intensity_=a.intensity;scale_=a.scale;speed_=a.speed;
 auto colors=a.resolvedPaletteColors;if(colors.empty())colors={{20,24,56},{64,53,128},{34,145,140},{184,222,195}};if(colors!=paletteColors_){paletteColors_=std::move(colors);paletteDirty_=true;}
}
bool FieldEffect::uploadPalette(std::string& error){std::array<std::array<float,4>,256> pixels{};for(unsigned i=0;i<256;++i){float p=i/255.f*(paletteColors_.size()-1);unsigned first=std::min(static_cast<unsigned>(p),static_cast<unsigned>(paletteColors_.size()-1)),last=std::min(first+1,static_cast<unsigned>(paletteColors_.size()-1));auto a=lab(paletteColors_[first]),b=lab(paletteColors_[last]);float f=p-first;auto c=mapped({a[0]+(b[0]-a[0])*f,a[1]+(b[1]-a[1])*f,a[2]+(b[2]-a[2])*f});pixels[i]={c[0],c[1],c[2],1};}
 D3D11_TEXTURE2D_DESC td{};td.Width=256;td.Height=1;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=sizeof(pixels);ComPtr<ID3D11Texture2D> texture;if(!good(device_->CreateTexture2D(&td,&data,&texture),"Create Oklab LUT",error))return false;palette_.Reset();if(!good(device_->CreateShaderResourceView(texture.Get(),nullptr,&palette_),"Upload Oklab LUT",error))return false;paletteDirty_=false;return true;}
bool FieldEffect::resize(unsigned width,unsigned height,std::string& error){
 unsigned simDivisor=quality_<.5f?3:quality_>1.5f?1:2;
 unsigned imageDivisor=theme_==0?(quality_<.5f?3:quality_>1.5f?1:2):(quality_<.5f?2:1);
 const unsigned sw=std::max(1u,width/simDivisor),sh=std::max(1u,height/simDivisor);
 const unsigned iw=std::max(1u,width/imageDivisor),ih=std::max(1u,height/imageDivisor);
 const unsigned bw=std::max(1u,width/4),bh=std::max(1u,height/4);
 auto make=[&](Surface& surface,unsigned w,unsigned h){
  D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.ArraySize=td.MipLevels=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  return good(device_->CreateTexture2D(&td,nullptr,&surface.texture),"Allocate field surface",error)&&good(device_->CreateRenderTargetView(surface.texture.Get(),nullptr,&surface.target),"Create field target",error)&&good(device_->CreateShaderResourceView(surface.texture.Get(),nullptr,&surface.view),"Create field view",error);
 };
 if(sw!=width_ || sh!=height_ || iw!=imageWidth_ || ih!=imageHeight_){
  Surface front,back,image;
  if(!make(front,sw,sh)||!make(back,sw,sh)||!make(image,iw,ih))return false;
  state_[0]=std::move(front);state_[1]=std::move(back);image_=std::move(image);
  width_=sw;height_=sh;imageWidth_=iw;imageHeight_=ih;front_=0;reset_=true;
 }
 if(bloomAmount_<=0){
  if(blur_[0].texture){blur_[0]={};blur_[1]={};}
 }else if(!blur_[0].texture || bw!=blurWidth_ || bh!=blurHeight_){
  Surface horizontal,vertical;
  if(!make(horizontal,bw,bh)||!make(vertical,bw,bh))return false;
  blur_[0]=std::move(horizontal);blur_[1]=std::move(vertical);
 }
 blurWidth_=bw;blurHeight_=bh;
 return true;
}
bool FieldEffect::push(double now,float x,float y){if(!std::isfinite(now)||!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)return false;Hit hit{x,y,now};
 if(theme_==2){snowRandom_^=snowRandom_<<13;snowRandom_^=snowRandom_>>17;snowRandom_^=snowRandom_<<5;hit.seed=float(snowRandom_&0xffffff)/16777216.f;}
 if(theme_!=1){
  hits_[nextHit_++%24]=hit;
  return true;
 }
 std::vector<std::array<float,2>> targets=centers_;auto last=hits_[(nextHit_+23)%24];if(last.time>=0 && now-last.time<1)targets.push_back({last.x,last.y});
 std::sort(targets.begin(),targets.end(),[&](auto a,auto b){return std::hypot((a[0]-x)*2.667f,a[1]-y)<std::hypot((b[0]-x)*2.667f,b[1]-y);});
 unsigned linked=0;
 if(last.time>=0 && now-last.time<1 && std::hypot((last.x-x)*2.667f,last.y-y)>.04f){arcs_[nextArc_%32]={x,y,last.x,last.y,now,float((nextArc_*37)%101)/101};++nextArc_;++linked;}
 for(auto target:targets){float distance=std::hypot((target[0]-x)*2.667f,target[1]-y);if(distance<.04f)continue;arcs_[nextArc_%32]={x,y,target[0],target[1],now,float((nextArc_*37)%101)/101};++nextArc_;if(++linked==3)break;}
 hits_[nextHit_++%24]=hit;return true;}
void FieldEffect::clear(){for(auto& h:hits_)h.time=-1;for(auto& a:arcs_)a.time=-1;reset_=true;previous_=-1;pausedAge_=0;}
std::size_t FieldEffect::count()const{return std::count_if(hits_.begin(),hits_.end(),[](const Hit& h){return h.time>=0;});}
bool FieldEffect::draw(ID3D11DeviceContext* c,unsigned width,unsigned height,double now,bool paused,std::string& error){
 if(background_.edited && !gradedShade_){
  const auto& code=FieldShaders[theme_];
  ComPtr<ID3D11PixelShader> shade,composite;
  if(!good(device_->CreatePixelShader(code.gradedShade.data,code.gradedShade.size,nullptr,&shade),"Create graded field shader",error) ||
     !good(device_->CreatePixelShader(code.gradedComposite.data,code.gradedComposite.size,nullptr,&composite),"Create graded field composite",error))return false;
  gradedShade_=std::move(shade);gradedComposite_=std::move(composite);
 }
 if(!resize(width,height,error)||(paletteDirty_&&!uploadPalette(error)))return false;
 const double elapsed=previous_<0?1./60:std::max(0.,now-previous_);previous_=now;
 if(paused){for(auto& h:hits_)if(h.time>=0)h.time+=elapsed;for(auto& a:arcs_)if(a.time>=0)a.time+=elapsed;}
 const float dt=paused?0:static_cast<float>(std::min(elapsed,.1))*speed_;simulationTime_+=dt;
 Constants v{};v.background=background_.gpu;v.clock[0]=float(simulationTime_);v.clock[1]=dt;v.clock[2]=float(width)/height;v.clock[3]=float(theme_);
 v.dynamics[0]=lifetime_;v.dynamics[1]=turbulence_;v.dynamics[2]=scale_;v.dynamics[3]=propagation_;
 v.look[0]=brightness_;v.look[1]=intensity_;v.look[2]=ambient_;v.look[3]=detail_;
 v.wordmark[0]=theme_==2 && wordmark_?wordmarkEnabled_:0;v.wordmark[1]=wordmarkBrightness_;v.wordmark[2]=wordmarkFlow_;
 if(wordmark_){
  const float cx=wordmarkSpace_[0],cy=wordmarkSpace_[1]+wordmarkOffsetY_,kw=wordmarkSpace_[2],kh=wordmarkSpace_[3];
  // Independent word destinations preserve the calibrated central switch hole.
  const float gap=std::max(kw*.18f,128.f),available=(kw-gap)*.5f-kw*.04f;
  const float inkHeight=std::max(wordmarkBounds_[0][3],wordmarkBounds_[1][3]);
  const float fit=std::min({available/wordmarkBounds_[0][2],available/wordmarkBounds_[1][2],kh*.64f/inkHeight})*wordmarkScale_;
  for(unsigned part=0;part<2;++part){const auto& box=wordmarkBounds_[part];const float dw=box[2]*fit,dh=box[3]*fit;
   const float center=cx+(part?1.f:-1.f)*(gap*.5f+available*.5f);
   v.wordmarkRect[part][0]=(center-dw*.5f)/1920;v.wordmarkRect[part][1]=(cy-dh*.5f)/720;
   v.wordmarkRect[part][2]=dw/1920;v.wordmarkRect[part][3]=dh/720;
   for(unsigned k=0;k<4;++k)v.wordmarkUv[part][k]=box[k]/wordmarkSize_[k%2];
  }
 }
 v.variety[0]=diversity_;v.variety[1]=density_;v.variety[2]=opacityVariation_;
 v.material[0]=thickness_;v.material[1]=refraction_;v.material[2]=variation_;v.material[3]=melt_;
 v.finish[0]=bloomAmount_;v.finish[1]=sparks_;v.finish[2]=quality_;
 v.geometry[1]=1.f/blurWidth_;v.geometry[2]=1.f/blurHeight_;v.geometry[3]=1.f/imageHeight_;
 auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
 // Past 10.5 widths the Gaussian brush is below the smallest float.
 // Pad the bounds for rounding and keep the original per-pixel kernels.
 const float brush=10.5f*std::max(.033f*scale_,.00031623f)+.0001f;
 const float imprint=.2f*scale_+.0001f;
 unsigned live=0;
 for(auto& h:hits_){
  if(h.time<0)continue;const float age=std::max(0.f,float(now-h.time));
  if(age>=lifetime_*2){h.time=-1;continue;}
  const float gain=theme_==2 ? smooth(age/.18f)*(1-.25f*smooth(age/lifetime_))*smooth((lifetime_*2-age)/(lifetime_*.35f)) : (1-std::exp(-age*35))*std::exp(-age/lifetime_)*smooth((lifetime_*2-age)/(lifetime_*.5f));
  if(gain<.0001f)continue;
  if(dt>0)
   markTiles(v.simulationTiles,live,h.x-brush/v.clock[2],h.y-brush,
             h.x+brush/v.clock[2],h.y+brush);
  if(theme_==1)
   markTiles(v.imprintTiles,live,h.x-imprint/v.clock[2],h.y-imprint,
             h.x+imprint/v.clock[2],h.y+imprint);
  v.hits[live][0]=h.x;v.hits[live][1]=h.y;v.hits[live][2]=theme_==2?age*speed_:age;v.hits[live][3]=gain;
  if(theme_==2){
   const float seed=h.seed;
   auto randomChannel=[seed](float salt){float n=std::sin(seed*173.31f+salt)*43758.5453f;return n-std::floor(n);};
   const float sizeSample=randomChannel(17.71f);
   const float radius=.157f+((.052f+.2f*std::pow(sizeSample,1.65f))-.157f)*variation_;
   v.snowInfo[live][0]=age/lifetime_;
   v.snowInfo[live][1]=radius;
   v.snowInfo[live][2]=1-opacityVariation_+opacityVariation_*(.2f+.8f*std::pow(randomChannel(71.13f),.85f));
   v.snowInfo[live][3]=randomChannel(31.41f);
   const float angle=std::fmod(seed*7.171f+.13f,1.f)*6.2831853f;
   v.hitInfo[live][0]=seed;v.hitInfo[live][1]=std::cos(angle);v.hitInfo[live][2]=std::sin(angle);
   v.hitInfo[live][3]=radius*propagation_*1.22f;
  }
  ++live;
 }
 v.geometry[0]=float(live);
 unsigned n=0;
 for(auto& arc:arcs_){
  if(arc.time<0)continue;const float age=std::max(0.f,float(now-arc.time));
  if(age>=lifetime_){arc.time=-1;continue;}
  const float gain=(1-std::exp(-age*40))*std::exp(-age*3/lifetime_)*smooth((lifetime_-age)/(lifetime_*.3f));
  if(gain<.0001f)continue;
  v.arcs[n][0]=arc.x;v.arcs[n][1]=arc.y;v.arcs[n][2]=arc.z;v.arcs[n][3]=arc.w;
  v.arcInfo[n][0]=age;v.arcInfo[n][1]=gain;v.arcInfo[n][2]=arc.seed;v.arcInfo[n][3]=.8f+.2f*std::sin(arc.seed*43+float(simulationTime_)*11);
  const float dx=(arc.z-arc.x)*v.clock[2],dy=arc.w-arc.y,len=std::max(std::hypot(dx,dy),.001f);
  v.arcAxis[n][0]=dx/len;v.arcAxis[n][1]=dy/len;v.arcAxis[n][2]=-dy/len;v.arcAxis[n][3]=dx/len;
  v.arcShape[n][0]=1/len;v.arcShape[n][1]=.034f*std::sin(arc.seed*29)*propagation_;
  v.arcShape[n][2]=.0175f*turbulence_+.034f*propagation_+.055f*scale_;
  // Project the corners of the exact oriented support of the existing arc.
  const float halfWidth=v.arcShape[n][2]+.0001f;
  const float xPad=.051f*std::abs(arc.z-arc.x)+halfWidth*std::abs(dy/len)/v.clock[2];
  const float yPad=.051f*std::abs(arc.w-arc.y)+halfWidth*std::abs(dx/len);
  markTiles(v.arcTiles,n,std::min(arc.x,arc.z)-xPad,std::min(arc.y,arc.w)-yPad,
            std::max(arc.x,arc.z)+xPad,std::max(arc.y,arc.w)+yPad);
  ++n;
 }
 v.finish[3]=float(n);
 D3D11_MAPPED_SUBRESOURCE mapped{};
 if(!good(c->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map field constants",error))return false;
 std::memcpy(mapped.pData,&v,sizeof(v));c->Unmap(constants_.Get(),0);
 ComPtr<ID3D11RenderTargetView> output;ComPtr<ID3D11DepthStencilView> oldDepth;
 c->OMGetRenderTargets(1,&output,&oldDepth);UINT viewportCount=1;D3D11_VIEWPORT oldViewport{};c->RSGetViewports(&viewportCount,&oldViewport);
 ID3D11ShaderResourceView* nulls[6]={};c->PSSetShaderResources(0,6,nulls);
 if(reset_){const float zero[4]={};for(auto& state:state_)c->ClearRenderTargetView(state.target.Get(),zero);reset_=false;}
 c->RSSetState(raster_.Get());c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(depth_.Get(),0);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->VSSetShader(vs_.Get(),nullptr,0);
 auto* buffer=constants_.Get();c->PSSetConstantBuffers(0,1,&buffer);
 ID3D11SamplerState* samplers[]={sampler_.Get(),repeat_.Get()};c->PSSetSamplers(0,2,samplers);
 auto viewport=[&](unsigned w,unsigned h){D3D11_VIEWPORT vp{};vp.Width=float(w);vp.Height=float(h);vp.MaxDepth=1;c->RSSetViewports(1,&vp);};
 auto target=[&](Surface& surface){auto* rtv=surface.target.Get();c->OMSetRenderTargets(1,&rtv,nullptr);};
 if(dt>0){
  viewport(width_,height_);target(state_[1-front_]);
  ID3D11ShaderResourceView* sources[]={state_[front_].view.Get(),nullptr,nullptr,noise_.Get(),nullptr,nullptr};c->PSSetShaderResources(0,6,sources);
  c->PSSetShader(simulate_.Get(),nullptr,0);c->Draw(3,0);c->PSSetShaderResources(0,6,nulls);front_=1-front_;
 }
 viewport(imageWidth_,imageHeight_);target(image_);
 ID3D11ShaderResourceView* sources[]={state_[front_].view.Get(),palette_.Get(),nullptr,noise_.Get(),nullptr,nullptr};
 c->PSSetShaderResources(0,6,sources);c->PSSetShader(background_.edited?gradedShade_.Get():shade_.Get(),nullptr,0);c->Draw(3,0);c->PSSetShaderResources(0,6,nulls);
 if(bloomAmount_>0){
  viewport(blurWidth_,blurHeight_);target(blur_[0]);auto* image=image_.view.Get();c->PSSetShaderResources(2,1,&image);
  c->PSSetShader(blurH_.Get(),nullptr,0);c->Draw(3,0);c->PSSetShaderResources(0,6,nulls);
  target(blur_[1]);image=blur_[0].view.Get();c->PSSetShaderResources(2,1,&image);
  c->PSSetShader(blurV_.Get(),nullptr,0);c->Draw(3,0);c->PSSetShaderResources(0,6,nulls);
 }
 auto* finalTarget=output.Get();c->OMSetRenderTargets(1,&finalTarget,oldDepth.Get());c->RSSetViewports(1,&oldViewport);
 sources[0]=nullptr;sources[1]=nullptr;sources[2]=image_.view.Get();sources[3]=nullptr;sources[4]=bloomAmount_>0?blur_[1].view.Get():nullptr;sources[5]=wordmark_.Get();
 c->PSSetShaderResources(0,6,sources);c->PSSetShader(background_.edited?gradedComposite_.Get():bloom_.Get(),nullptr,0);c->Draw(3,0);c->PSSetShaderResources(0,6,nulls);
 return good(device_->GetDeviceRemovedReason(),"Field GPU render",error);
}
}
