#include "ProceduralBurst.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace action75 {namespace {
constexpr unsigned MaxHits=24,MaxPixels=256,MaxInstances=MaxHits*MaxPixels;
constexpr float Pi=3.14159265359f;
struct Constants {float display[4],look[4],motion[4];};
const char Shader[]=R"HLSL(
cbuffer Burst:register(b0){float4 display,look,motion;};
struct I {float4 center:INSTANCE0;float4 shape:INSTANCE1;float4 velocity:INSTANCE2;float4 color:INSTANCE3;float4 data:INSTANCE4;};
struct V {float4 position:SV_POSITION;float2 local:TEXCOORD0;nointerpolation float4 center:TEXCOORD1;nointerpolation float4 velocity:TEXCOORD2;nointerpolation float4 color:TEXCOORD3;nointerpolation float4 data:TEXCOORD4;};
V vs(uint vertex:SV_VertexID,I input){
 float2 uv=float2(vertex&1,(vertex>>1)&1)*2-1;
 float t=input.center.z;float2 p;
 if(look.z<.5){
  // Actual particle positions are integrated by the shared fluid solver.
  // Only pixel-sized geometry is emitted here; no radial animation shortcut.
  float2 axis=input.shape.xy,normal=float2(-axis.y,axis.x);
  p=(axis*uv.x*input.shape.w+normal*uv.y/max(input.shape.w,.5))*input.shape.z*.5;
 }else p=uv*input.shape.xy;
 V o;o.position=float4((input.center.xy+p*display.z/display.xy)*float2(2,-2)+float2(-1,1),0,1);
 o.local=look.z<.5?uv:p;o.center=input.center;o.velocity=input.velocity;o.color=input.color;o.data=input.data;return o;
}
float3 toSrgb(float3 c){return lerp(c*12.92,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float filteredLine(float distance,float width){float aa=max(fwidth(distance),1/display.z);float filtered=sqrt(width*width+aa*aa*.25);return exp(-distance*distance/(filtered*filtered))*width/filtered;}
float4 ps(V v):SV_TARGET{
 float t=v.center.z,alpha,energy=1;float3 color=v.color.rgb;
 if(look.z<.5){
  float kernel=pow(saturate(1-dot(v.local,v.local)),2);
  float end=.70+.3*v.data.x,start=.28+.24*v.data.y;
  float weight=kernel*smoothstep(0,.026,t)*(1-smoothstep(start,end,t))*v.velocity.w*v.data.w;
  return float4(color*v.velocity.z*weight,weight);
 }else{
  float2 q=float2(v.local.x*v.velocity.y+v.local.y*v.velocity.z,-v.local.x*v.velocity.z+v.local.y*v.velocity.y);
  float2 head=v.data.xy;float arcEnergy=0,glow=0;
  [unroll]for(int k=0;k<3;++k){
   if(k>=(int)motion.z)break;
   float eccentricity=max(.25,v.velocity.w-k*.04);
   float2 ellipse=q/float2(1,eccentricity);
   float r=length(ellipse),radius=v.velocity.x*(1-k*.12);
   float d=(r-radius)*eccentricity;
   float alignment=dot(ellipse/max(r,.00001),head);
   float arc=smoothstep(-.72,.45,alignment);
   float width=(.85-.18*k)*motion.w/display.z;
   float temporalFootprint=v.data.z*eccentricity*.0012;
   width=sqrt(width*width+temporalFootprint*temporalFootprint);
   float glint=pow(saturate(.5+.5*alignment),70);
   arcEnergy+=filteredLine(d,width)*arc*(.52+glint*1.3)/(1+k*.65);
   glow+=exp(-d*d*display.z*display.z/22)*arc*.028/(1+k);
   q=mul(float2x2(.7960838,-.6051864,.6051864,.7960838),q);
   head=mul(float2x2(.8253356,.5646425,-.5646425,.8253356),head);
  }
  float core=exp(-dot(v.local,v.local)*800)*exp(-t*18)*.2;
  energy=(arcEnergy+glow+core)*smoothstep(0,.028,t)*(1-smoothstep(.35,1,t));
  alpha=saturate(energy*v.data.w);color*=look.x;
 }
 float peak=max(color.r,max(color.g,color.b));color/=1+peak*.32;
 color=toSrgb(color);
 return float4(color*alpha,look.z<.5?alpha:0);
}
Texture2D waterField:register(t0);
SamplerState fieldLinear:register(s0);
struct Screen {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
Screen screenVS(uint id:SV_VertexID){Screen o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float fieldHash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float fieldNoise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);return lerp(lerp(fieldHash(i),fieldHash(i+float2(1,0)),f.x),lerp(fieldHash(i+float2(0,1)),fieldHash(i+1),f.x),f.y);}
float4 waterComposite(Screen input):SV_TARGET{
 // Pixelate the reconstructed sheet, not its simulation particles. Each
 // screen pixel cell contains one coherent pigment value and transmission.
 float cell=max(display.w,2);float2 grid=floor(input.uv*display.xy/cell);
 float2 uv=(grid+.5)*cell/display.xy;
 float4 film=waterField.SampleLevel(fieldLinear,uv,0);float density=film.a;
 if(density<.025)return 0;
 float3 pigment=film.rgb/max(density,.00001);
 float coarse=fieldNoise(grid*.19+4.7),fine=fieldNoise(grid*.72+13.1);
 float strata=floor((coarse*.65+fine*.35)*7)/7;
 float wash=.73+.30*saturate(density*.50)+strata*.13;
 pigment*=wash;
 // Broad shared highlights belong to the thin sheet. Individual kernels
 // have no bevel, outline or specular highlight that could look like stones.
 float pigmentPeak=max(pigment.r,max(pigment.g,pigment.b));
 float fold=pow(saturate(pigmentPeak*1.15),4)*.45;
 pigment+=float3(1,.96,.88)*fold;
 pigment*=look.x;
 float peak=max(pigment.r,max(pigment.g,pigment.b));pigment/=1+peak;
 float alpha=smoothstep(.06,.32,density)*(1-exp(-density*1.30))*motion.z;
 return float4(toSrgb(pigment)*alpha,alpha);
}

)HLSL";
bool good(HRESULT hr,const char* what,std::string& error){if(SUCCEEDED(hr))return true;error=what;return false;}
float smooth(float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);}
float linear(float x){x/=255;return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
}
bool ProceduralBurst::load(ID3D11Device* d,unsigned style,float diameter,float duration,std::string& error,const std::filesystem::path& shapeFile){
 if(!d||style>1||!std::isfinite(diameter)||diameter<48||diameter>500||!std::isfinite(duration)||duration<.12f||duration>3){error="Invalid procedural burst parameters";return false;}
 device_=d;style_=style;diameter_=diameter;duration_=duration;
 if(style_==0&&!loadFilmShapes(shapeFile,error))return false;hits_.reserve(MaxHits);upload_.reserve(MaxInstances);activeFluid_.reserve(PixelFluid::capacity);
 ComPtr<ID3DBlob> vertex,pixel,message;
 auto compile=[&](const char* entry,const char* target,ComPtr<ID3DBlob>& blob){auto hr=D3DCompile(Shader,sizeof(Shader)-1,"ProceduralBurst",nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&message);if(FAILED(hr)){error=message?std::string((char*)message->GetBufferPointer(),message->GetBufferSize()):"Compile procedural burst";return false;}return true;};
 if(!compile("vs","vs_5_0",vertex)||!compile("ps","ps_5_0",pixel))return false;
 if(!good(d->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs_),"Create procedural vertex shader",error)||!good(d->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps_),"Create procedural pixel shader",error))return false;
 D3D11_INPUT_ELEMENT_DESC elements[5]{};for(unsigned i=0;i<5;++i)elements[i]={"INSTANCE",i,DXGI_FORMAT_R32G32B32A32_FLOAT,0,i*16,D3D11_INPUT_PER_INSTANCE_DATA,1};
 if(!good(d->CreateInputLayout(elements,5,vertex->GetBufferPointer(),vertex->GetBufferSize(),&layout_),"Create procedural instance layout",error))return false;
 D3D11_BUFFER_DESC buffer{};buffer.Usage=D3D11_USAGE_DYNAMIC;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.ByteWidth=sizeof(Constants);
 if(!good(d->CreateBuffer(&buffer,nullptr,&constants_),"Create procedural constants",error))return false;
 buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;buffer.ByteWidth=sizeof(Instance)*MaxInstances;
 if(!good(d->CreateBuffer(&buffer,nullptr,&instances_),"Create bounded procedural instances",error))return false;
 D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_ONE;rt.DestBlend=D3D11_BLEND_ONE;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=style_?D3D11_BLEND_ZERO:D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ONE;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
 if(!good(d->CreateBlendState(&blend,&blend_),"Create procedural blend",error))return false;
 if(style_==0){
  ComPtr<ID3DBlob> screen,surface;
  if(!compile("screenVS","vs_5_0",screen)||!compile("waterComposite","ps_5_0",surface))return false;
  if(!good(d->CreateVertexShader(screen->GetBufferPointer(),screen->GetBufferSize(),nullptr,&filmVs_),"Create water-film vertex shader",error)||!good(d->CreatePixelShader(surface->GetBufferPointer(),surface->GetBufferSize(),nullptr,&filmPs_),"Create water-film compositor",error))return false;
  rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.SrcBlendAlpha=D3D11_BLEND_ZERO;rt.DestBlendAlpha=D3D11_BLEND_ONE;
  if(!good(d->CreateBlendState(&blend,&filmBlend_),"Create water-film composition",error))return false;
  D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.MaxLOD=D3D11_FLOAT32_MAX;
  if(!good(d->CreateSamplerState(&sampler,&filmSampler_),"Create water-film sampler",error))return false;
 }
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
 if(!good(d->CreateRasterizerState(&raster,&raster_),"Create procedural rasterizer",error))return false;
 D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;return good(d->CreateDepthStencilState(&depth,&depth_),"Create procedural depth",error);
}
bool ProceduralBurst::loadFilmShapes(const std::filesystem::path& file,std::string& error){
 ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
 HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
 if(SUCCEEDED(hr))hr=factory->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
 if(SUCCEEDED(hr))hr=decoder->GetFrame(0,&frame);UINT width=0,height=0;if(SUCCEEDED(hr))hr=frame->GetSize(&width,&height);
 if(!good(hr,"Open water-film shape atlas",error))return false;
 if(width<192||height<128||width>4096||height>4096||width%3||height%2){error="Water film atlas must contain a 3 by 2 shape sheet";return false;}
 hr=factory->CreateFormatConverter(&converter);if(SUCCEEDED(hr))hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
 std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4);if(SUCCEEDED(hr))hr=converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data());
 if(!good(hr,"Decode water-film shapes",error))return false;
 for(unsigned shape=0;shape<6;++shape){auto& samples=filmShapes_[shape];samples.resize(64*64);unsigned visible=0;
  for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){unsigned px=(shape%3)*(width/3)+unsigned((x+.5f)/64*(width/3)),py=(shape/3)*(height/2)+unsigned((y+.5f)/64*(height/2));auto index=(static_cast<size_t>(py)*width+px)*4;
   float pigment=std::max({pixels[index],pixels[index+1],pixels[index+2]})/255.f;
   samples[y*64+x]=pigment;if(pigment>.08f)++visible;
  }
  if(visible<32){error="Water-film atlas contains an empty shape";return false;}
  filmCoverage_[shape]=visible/4096.f;
 }
 return true;
}
bool ProceduralBurst::resizeFilm(unsigned width,unsigned height,std::string& error){
 unsigned w=std::max(1u,width/2),h=std::max(1u,height/2);if(w==filmWidth_&&h==filmHeight_)return true;
 ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11RenderTargetView> target;
 D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
 if(!good(device_->CreateTexture2D(&td,nullptr,&texture),"Allocate water-film field",error)||!good(device_->CreateShaderResourceView(texture.Get(),nullptr,&view),"Create water-film view",error)||!good(device_->CreateRenderTargetView(texture.Get(),nullptr,&target),"Create water-film target",error))return false;
 filmTexture_=texture;filmView_=view;filmTarget_=target;filmWidth_=w;filmHeight_=h;return true;
}

void ProceduralBurst::configure(const PackAppearance& a){appearance_=a;auto get=[&](const char* k,float v){return a.field.value(k,v);};viscosity_=get("fluidViscosity",.45f);cohesion_=get("fluidCohesion",.8f);pressure_=get("fluidPressure",1.f);gravity_=get("fluidGravity",.75f);variety_=get("fluidVariation",1.f);filmShape_=get("filmShape",0);filmSpread_=get("filmSpread",2.15f);filmStretch_=get("filmStretch",1.f);filmTear_=get("filmTear",.65f);pixelSize_=get("pixelSize",4);density_=get("fragmentDensity",.58f);force_=get("shatterForce",1);drift_=get("fragmentDrift",.5f);opacity_=get("fragmentOpacity",.8f);arcCount_=get("arcCount",2);arcWidth_=get("arcWidth",1);arcSweep_=get("arcSweep",1);arcEccentricity_=get("arcEccentricity",.56f);}
std::array<float,3> ProceduralBurst::hitColor(unsigned index)const{
 static const std::vector<EffectRgb> orbit{{255,197,106},{255,152,102},{255,224,163}},pigment{{239,118,106},{69,174,169},{151,118,186}};
 const auto& colors=appearance_.resolvedPaletteColors.empty()?(style_?orbit:pigment):appearance_.resolvedPaletteColors;
 const auto rgb=colors[index%colors.size()];
 std::array<float,3> c{float(rgb[0]),float(rgb[1]),float(rgb[2])};
 if (appearance_.hue != 0) {
   const float radians=appearance_.hue*.01745329252f;
   const float cosine=std::cos(radians), sine=std::sin(radians)*.5773502692f;
   const float diagonal=(1+2*cosine)/3, side=(1-cosine)/3;
   for (unsigned i=0;i<3;++i)
     c[i]=std::clamp(diagonal*rgb[i]+(side-sine)*rgb[(i+1)%3]+(side+sine)*rgb[(i+2)%3],0.f,255.f);
 }
 std::array<float,3> out{};float luma=(c[0]*.2126f+c[1]*.7152f+c[2]*.0722f);
 for(unsigned i=0;i<3;++i)out[i]=linear(std::clamp(luma+(c[i]-luma)*appearance_.saturation,0.f,255.f))*appearance_.color[i]/255.f;
 return out;
}
void ProceduralBurst::clear(){hits_.clear();previous_=-1;wasPaused_=false;fluidClock_=0;fluidWallLast_=-1;fluidTicks_=0;fluidFraction_=0;fluid_.clear();}
bool ProceduralBurst::push(double now,float x,float y){
 if(!std::isfinite(now)||!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)return false;
 auto random=[&](){random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;return float(random_&0xffffff)/16777216.f;};
 if(style_==0)syncFluidClock(now,wasPaused_);
 hits_.erase(std::remove_if(hits_.begin(),hits_.end(),[&](const Hit& h){return style_==0?fluidClock_-h.fluidBorn>=fluidLife():now-h.born>=life();}),hits_.end());
 if(hits_.size()==MaxHits)hits_.erase(hits_.begin());
 Hit hit{now,x,y,random(),serial_++,{}};
 if(style_==0){
  if(hits_.empty()){fluidClock_=0;fluidWallLast_=now;fluidTicks_=0;fluidFraction_=0;fluid_.clear();}
  hit.fluidBorn=fluidClock_;
  const unsigned habit=filmShape_>.5f?unsigned(filmShape_-1)%6:(variety_>.15f?(serial_+unsigned(hit.seed*5))%6:0);
  const auto& source=filmShapes_[habit];if(source.empty())return false;
  const float coverage=std::max(.08f,filmCoverage_[habit]);
  const int requested=std::clamp(int(diameter_/pixelSize_*.62f),22,48);
  const int n=std::clamp(std::min(requested,int(std::sqrt(MaxPixels/(coverage*(.75f+.25f*density_))))),22,48);
  hit.pixels.reserve(MaxPixels);
  const float angle=random()*2*Pi*variety_,ca=std::cos(angle),sa=std::sin(angle);
  const float mirrorX=random()<.5f&&variety_>.2f?-1.f:1.f,mirrorY=random()<.35f&&variety_>.2f?-1.f:1.f;
  const float sx=.88f+random()*.30f*variety_,sy=.83f+random()*.34f*variety_;
  const float worldSize=diameter_*appearance_.scale,initial=.66f,cell=1.f/n;
  const float impulse=(.8f+random()*.45f)*force_*filmStretch_;
  const float spin=(random()-.5f)*1.6f*variety_,bend=(random()-.5f)*.12f*variety_;
  const float driftAngle=random()*2*Pi,driftSpeed=(18+random()*28)*std::sqrt(appearance_.scale);
  auto sample=[&](float u,float v){int x=std::clamp(int(u*64),0,63),y=std::clamp(int(v*64),0,63);return source[y*64+x];};
  unsigned accepted=0;
  for(int iy=0;iy<n;++iy)for(int ix=0;ix<n;++ix){
   float u=(ix+.5f)/n,v=(iy+.5f)/n,shade=sample(u,v),mask=smooth((shade-.045f)/.08f);
   if(random()>mask*(.75f+.25f*density_))continue;
   float px=(u-.5f)*sx*mirrorX,py=(v-.5f)*sy*mirrorY;py+=bend*std::sin(px*6+hit.seed*4);
   float lx=px*worldSize*initial,ly=py*worldSize*initial;
   float wx=ca*lx-sa*ly,wy=sa*lx+ca*ly;
   float expansionX=habit==0?1.5f:habit==1?1.25f:habit==2?.62f:habit==3?1.05f:habit==4?1.2f:.95f;
   float expansionY=habit==0?.70f:habit==1?.65f:habit==2?.58f:habit==3?1.28f:habit==4?.72f:1.12f;
   float localVx=lx*expansionX*impulse-ly*spin,localVy=ly*expansionY*impulse+lx*spin;
   if(habit==1)localVx+=std::sin(py*7)*55*impulse;
   if(habit==2){localVx-=ly*.85f;localVy+=lx*.85f;}
   if(habit==4){localVx+=std::sin(py*9)*55*impulse;localVy+=std::sin(px*7)*35*impulse;}
   float vx=ca*localVx-sa*localVy+std::cos(driftAngle)*driftSpeed;
   float vy=sa*localVx+ca*localVy+std::sin(driftAngle)*driftSpeed-18;
   float spacing=cell*worldSize*initial*std::sqrt(sx*sy),mass=spacing*spacing*(.9f+random()*.2f),cr=random();
   float gx=sample(u+cell,v)-sample(u-cell,v),gy=sample(u,v+cell)-sample(u,v-cell);
   float tx=-gy*sx*mirrorX,ty=gx*sy*mirrorY,len=std::hypot(tx,ty);if(len<.001f){tx=px;ty=py;len=std::max(std::hypot(tx,ty),.001f);}tx/=len;ty/=len;
   FluidParticle body{x*1920+wx,y*720+wy,x*1920+wx,y*720+wy,vx,vy,spacing,mass};
   Pixel pixel{body,spacing,.08f+1.22f*shade*shade,.85f+random()*.15f,cr,.2f+random()*.6f,ca*tx-sa*ty,sa*tx+ca*ty};
   ++accepted;if(hit.pixels.size()<MaxPixels)hit.pixels.push_back(pixel);else{unsigned slot=unsigned(random()*accepted);if(slot<MaxPixels)hit.pixels[slot]=pixel;}
  }
  size_t total=hit.pixels.size();for(const auto& older:hits_)total+=older.pixels.size();
  while(total>PixelFluid::capacity&&!hits_.empty()){total-=hits_.front().pixels.size();hits_.erase(hits_.begin());}

 }
 hits_.push_back(std::move(hit));return true;
}
void ProceduralBurst::syncFluidClock(double now,bool stopped){
 if(fluidWallLast_>=0&&!stopped)fluidClock_+=std::max(0.,now-fluidWallLast_)*appearance_.speed;
 fluidWallLast_=now;
}
void ProceduralBurst::advanceFluid(double){
 if(fluidWallLast_<0)return;constexpr double Step=1./120;
 auto target=static_cast<unsigned long long>(std::max(0.,std::floor(fluidClock_/Step+1e-6)));
 if(target>fluidTicks_+64)fluidTicks_=target-64;
 PixelFluid::Params params;params.viscosity=viscosity_*.35f;params.cohesion=cohesion_*.12f;params.pressure=pressure_*.60f;params.gravity=155*gravity_;params.drag=.25f+.10f*drift_;
 while(fluidTicks_<target){double at=(fluidTicks_+1)*Step;activeFluid_.clear();
  for(auto& hit:hits_){float age=float((at-hit.fluidBorn)/fluidLife());if(age<0||age>=1)continue;
   for(auto& pixel:hit.pixels)if(age<.70f+.30f*pixel.lifeRand)activeFluid_.push_back(&pixel.body);
  }
  fluid_.step(activeFluid_,float(Step),params);++fluidTicks_;
 }
 fluidFraction_=std::clamp(float(fluidClock_/Step-fluidTicks_),0.f,1.f);
}
bool ProceduralBurst::draw(ID3D11DeviceContext* c,unsigned width,unsigned height,double now,bool paused,std::string& error){
 const double elapsed=previous_<0?0:std::max(0.,now-previous_);previous_=now;
 if(style_==0)syncFluidClock(now,paused||wasPaused_);
 if(paused||wasPaused_)for(auto& hit:hits_)hit.born+=elapsed;wasPaused_=paused;
 if(style_==0)advanceFluid(now);
 const float duration=life();hits_.erase(std::remove_if(hits_.begin(),hits_.end(),[&](const Hit& h){return style_==0?fluidClock_-h.fluidBorn>=fluidLife():now-h.born>=duration;}),hits_.end());upload_.clear();
 float budget=hits_.size()>8?std::sqrt(8.f/float(hits_.size())):1;
 for(const auto& h:hits_){
  const float t=std::clamp(float(style_==0?(fluidClock_-h.fluidBorn)/fluidLife():(now-h.born)/duration),0.f,1.f);auto color=hitColor(h.palette);
  Instance base{};base.center[0]=h.x;base.center[1]=h.y;base.center[2]=t;base.center[3]=h.seed;
  for(unsigned k=0;k<3;++k)base.color[k]=color[k];base.data[3]=budget;
  if(style_==0){for(const auto& p:h.pixels){
   if(t>=.70f+.30f*p.lifeRand)continue;const auto& body=p.body;Instance i=base;
   i.center[0]=(body.previousX+(body.x-body.previousX)*fluidFraction_)/1920;
   i.center[1]=(body.previousY+(body.y-body.previousY)*fluidFraction_)/720;
   float vx=body.vx,vy=body.vy,speed=std::hypot(vx,vy),tx=p.tx,ty=p.ty;
   if(speed>1){float sign=tx*vx+ty*vy<0?-1.f:1.f;tx=tx*.7f+sign*vx/speed*.3f;ty=ty*.7f+sign*vy/speed*.3f;}
   float length=std::max(std::hypot(tx,ty),.001f);i.shape[0]=tx/length;i.shape[1]=ty/length;
   const float thinning=1-filmTear_*.45f*smooth((t-.42f)/.55f);
   i.shape[2]=p.size*filmSpread_*2*thinning/(diameter_*appearance_.scale);i.shape[3]=1+std::min(.8f,speed/250)*.55f*filmStretch_;
   i.motion[2]=p.shade;i.motion[3]=p.opacity;
   i.data[0]=p.lifeRand;i.data[1]=p.fadeRand;i.data[2]=std::clamp((-.6f*body.nx-.8f*body.ny)*.55f,0.f,.55f);
   upload_.push_back(i);
  }}
  else{
   const float growth=std::clamp(t/.82f,0.f,1.f);
   float radius=.018f+.39f*smooth(growth),angle=h.seed*2*Pi+t*.48f*arcSweep_,head=h.seed*2*Pi+t*4.1f*arcSweep_;
   base.motion[0]=radius;base.motion[1]=std::cos(angle);base.motion[2]=std::sin(angle);base.motion[3]=arcEccentricity_;
   base.data[0]=std::cos(head);base.data[1]=std::sin(head);
   base.data[2]=growth<1?.39f*6*growth*(1-growth)/(.82f*duration):0;
   // Tight projected ellipse bounds make early hits cheap without clipping glow.
   float ex=0,ey=0;for(int k=0;k<int(arcCount_);++k){float a=angle-k*.65f,co=std::cos(a),si=std::sin(a),ecc=std::max(.25f,arcEccentricity_-k*.04f),r=radius*(1-k*.12f);ex=std::max(ex,r*std::sqrt(co*co+ecc*ecc*si*si));ey=std::max(ey,r*std::sqrt(si*si+ecc*ecc*co*co));}
   const float pad=10.f/(diameter_*appearance_.scale*height/720),core=.12f*std::exp(-t*6);base.shape[0]=std::max(ex,core)+pad;base.shape[1]=std::max(ey,core)+pad;upload_.push_back(base);
  }
 }
 if(upload_.empty())return true;
 Constants constants{{float(width),float(height),diameter_*appearance_.scale*height/720,std::max(2.f,pixelSize_*appearance_.scale*height/720)},{appearance_.brightness*appearance_.intensity,0,float(style_),0},{force_,drift_,style_?arcCount_:opacity_,arcWidth_}};
 auto write=[&](ID3D11Buffer* b,const void* data,size_t bytes){D3D11_MAPPED_SUBRESOURCE mapped{};if(!good(c->Map(b,0,D3D11_MAP_WRITE_DISCARD,0,&mapped),"Map procedural data",error))return false;std::memcpy(mapped.pData,data,bytes);c->Unmap(b,0);return true;};
 if(!write(constants_.Get(),&constants,sizeof(constants))||!write(instances_.Get(),upload_.data(),upload_.size()*sizeof(Instance)))return false;
 ComPtr<ID3D11RenderTargetView> output;ComPtr<ID3D11DepthStencilView> oldDepth;D3D11_VIEWPORT oldViewport{};
 if(style_==0){
  if(!resizeFilm(width,height,error))return false;UINT count=1;c->OMGetRenderTargets(1,&output,&oldDepth);c->RSGetViewports(&count,&oldViewport);
  ID3D11ShaderResourceView* empty=nullptr;c->PSSetShaderResources(0,1,&empty);const float clear[4]={};c->ClearRenderTargetView(filmTarget_.Get(),clear);auto* target=filmTarget_.Get();c->OMSetRenderTargets(1,&target,nullptr);
  D3D11_VIEWPORT vp{};vp.Width=float(filmWidth_);vp.Height=float(filmHeight_);vp.MaxDepth=1;c->RSSetViewports(1,&vp);
 }
 UINT stride=sizeof(Instance),offset=0;auto* buffer=instances_.Get();c->IASetInputLayout(layout_.Get());c->IASetVertexBuffers(0,1,&buffer,&stride,&offset);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
 c->RSSetState(raster_.Get());c->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);c->OMSetDepthStencilState(depth_.Get(),0);c->GSSetShader(nullptr,nullptr,0);c->VSSetShader(vs_.Get(),nullptr,0);c->PSSetShader(ps_.Get(),nullptr,0);buffer=constants_.Get();c->VSSetConstantBuffers(0,1,&buffer);c->PSSetConstantBuffers(0,1,&buffer);c->DrawInstanced(4,UINT(upload_.size()),0,0);
 if(style_==0){
  auto* target=output.Get();c->OMSetRenderTargets(1,&target,oldDepth.Get());c->RSSetViewports(1,&oldViewport);c->OMSetBlendState(filmBlend_.Get(),nullptr,0xffffffff);
  c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(filmVs_.Get(),nullptr,0);c->PSSetShader(filmPs_.Get(),nullptr,0);
  auto* source=filmView_.Get();auto* sampler=filmSampler_.Get();c->PSSetShaderResources(0,1,&source);c->PSSetSamplers(0,1,&sampler);c->Draw(3,0);source=nullptr;c->PSSetShaderResources(0,1,&source);
 }
 return good(device_->GetDeviceRemovedReason(),"Procedural burst GPU render",error);
}
}
