#include "NativeRenderer.h"
#include "SpriteBurst.h"
#include <wincodec.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
constexpr unsigned Width=128,Height=128;
using Rgb=std::array<std::uint8_t,3>;
using Pixels=std::vector<std::uint8_t>;
void require(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
}
void succeeded(HRESULT result,const char* operation) {
  require(SUCCEEDED(result),std::string(operation)+" failed (HRESULT "+std::to_string(static_cast<long>(result))+")");
}
struct ComLifetime {
  HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  ComLifetime() {succeeded(initialized,"CoInitializeEx");}
  ~ComLifetime() {if(SUCCEEDED(initialized)) CoUninitialize();}
};
struct Temporary {
  fs::path root;
  Temporary() {
    const auto parent=fs::weakly_canonical(fs::temp_directory_path());
    root=parent/("Action75-BackgroundAppearance-"+std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
    require(root.parent_path()==parent && fs::create_directory(root),"Cannot create isolated PNG fixture directory");
  }
  ~Temporary() {
    std::error_code error;const auto parent=fs::weakly_canonical(fs::temp_directory_path(),error);
    if(!error && root.parent_path()==parent && root.filename().string().find("Action75-BackgroundAppearance-")==0)
      fs::remove_all(root,error);
  }
};
Pixels solid(unsigned width,unsigned height,const Rgb& color) {
  Pixels rgba(static_cast<std::size_t>(width)*height*4,255);
  for(std::size_t offset=0;offset<rgba.size();offset+=4)
    for(unsigned channel=0;channel<3;++channel) rgba[offset+channel]=color[channel];
  return rgba;
}
Pixels quadrants() {
  auto rgba=solid(Width,Height,{0,0,0});
  for(unsigned y=0;y<Height;++y) for(unsigned x=0;x<Width;++x) {
    const Rgb color=y<Height/2 ? (x<Width/2 ? Rgb{255,255,255} : Rgb{255,0,0})
                              : (x<Width/2 ? Rgb{32,96,160} : Rgb{0,0,0});
    for(unsigned channel=0;channel<3;++channel) rgba[(y*Width+x)*4+channel]=color[channel];
  }
  return rgba;
}
void writePng(const fs::path& file,unsigned width,unsigned height,const Pixels& rgba) {
  require(rgba.size()==static_cast<std::size_t>(width)*height*4,"Invalid PNG fixture size");
  // WIC's PNG encoder accepts BGRA; the player's real decoder returns RGBA.
  auto bgra=rgba;
  for(std::size_t offset=0;offset<bgra.size();offset+=4) std::swap(bgra[offset],bgra[offset+2]);
  ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
  succeeded(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"Create WIC factory");
  succeeded(factory->CreateStream(&stream),"Create PNG stream");
  succeeded(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE),"Open temporary PNG");
  succeeded(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Create PNG encoder");
  succeeded(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Initialize PNG encoder");
  succeeded(encoder->CreateNewFrame(&frame,nullptr),"Create PNG frame");
  succeeded(frame->Initialize(nullptr),"Initialize PNG frame");
  succeeded(frame->SetSize(width,height),"Set PNG size");
  auto format=GUID_WICPixelFormat32bppBGRA;
  succeeded(frame->SetPixelFormat(&format),"Set PNG pixel format");
  require(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"Unexpected PNG encoder pixel format");
  succeeded(frame->WritePixels(height,width*4,static_cast<UINT>(bgra.size()),bgra.data()),"Write PNG pixels");
  succeeded(frame->Commit(),"Commit PNG frame");succeeded(encoder->Commit(),"Commit PNG");
}
Pixels capture(action75::NativeRenderer& renderer) {
  // Readback belongs only to this offline probe, never the product renderer.
  auto* context=renderer.context();ComPtr<ID3D11RenderTargetView> view;
  context->OMGetRenderTargets(1,&view,nullptr);require(view.Get()!=nullptr,"Missing WARP render target");
  ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> target;succeeded(resource.As(&target),"Get target texture");
  D3D11_TEXTURE2D_DESC descriptor{};target->GetDesc(&descriptor);
  require(descriptor.Width==Width && descriptor.Height==Height && descriptor.Format==DXGI_FORMAT_R8G8B8A8_UNORM,
    "Unexpected offscreen readback format");
  descriptor.Usage=D3D11_USAGE_STAGING;descriptor.BindFlags=0;descriptor.MiscFlags=0;
  descriptor.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;succeeded(renderer.device()->CreateTexture2D(&descriptor,nullptr,&staging),"Create probe staging texture");
  Pixels rgba(static_cast<std::size_t>(Width)*Height*4);
  context->CopyResource(staging.Get(),target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};succeeded(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map probe staging texture");
  for(unsigned y=0;y<Height;++y)
    std::memcpy(rgba.data()+static_cast<std::size_t>(y)*Width*4,
      static_cast<const std::uint8_t*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,Width*4);
  context->Unmap(staging.Get(),0);
  return rgba;
}
Rgb pixel(const Pixels& pixels,unsigned x,unsigned y) {
  const auto offset=(static_cast<std::size_t>(y)*Width+x)*4;
  return {pixels.at(offset),pixels.at(offset+1),pixels.at(offset+2)};
}
unsigned chroma(const Rgb& color) {
  const auto limits=std::minmax_element(color.begin(),color.end());
  return static_cast<unsigned>(*limits.second)-*limits.first;
}
void opaque(const Pixels& pixels) {
  for(std::size_t offset=3;offset<pixels.size();offset+=4) require(pixels[offset]==255,"Background or effect changed opaque output alpha");
}
Pixels frame(action75::NativeRenderer& renderer,float brightness,float saturation,float hue) {
  renderer.setBackgroundAppearance(brightness,saturation,hue);renderer.beginFrame();
  auto pixels=capture(renderer);opaque(pixels);return pixels;
}
}
int main() {
  try {
    ComLifetime com;Temporary temporary;
    const auto source=quadrants(),white=solid(Width,Height,{255,255,255});
    const auto colorFile=temporary.root/"color.png",whiteFile=temporary.root/"white.png",atlasFile=temporary.root/"marker.png";
    writePng(colorFile,Width,Height,source);writePng(whiteFile,Width,Height,white);
    // Two identical 32x32 frames exercise the actual product additive pipeline.
    const Rgb markerColor{48,176,96};writePng(atlasFile,64,32,solid(64,32,markerColor));
    action75::NativeRenderer renderer;std::string error;
    require(renderer.initialize(nullptr,Width,Height,true,error),"Initialize offscreen WARP: "+error);
    const auto load=[&](const fs::path& file) {require(renderer.loadBackground(file,error),"Load generated background: "+error);};
    unsigned passed=0;
    const auto test=[&](const char* name,const std::function<void()>& run) {
      try {run();++passed;std::cout<<"PASS "<<name<<'\n';}
      catch(const std::exception& failure) {throw std::runtime_error(std::string(name)+": "+failure.what());}
    };
    load(colorFile);renderer.beginFrame();const auto baseline=capture(renderer);
    test("default neutral preserves original PNG bytes and opaque-copy path",[&] {
      require(renderer.usingBackdropCopy(),"Default neutral no longer uses opaque-copy fast path");
      require(baseline==source,"Default beginFrame changed decoded source RGBA pixels");opaque(baseline);
    });
    test("explicit neutral matches original copy and legacy background shader",[&] {
      require(frame(renderer,1,1,0)==baseline && renderer.usingBackdropCopy(),"Explicit neutral changed original pixels or copy eligibility");
      renderer.allowBackdropCopy(false);
      require(frame(renderer,1,1,0)==baseline && !renderer.usingBackdropCopy(),"Neutral sampling path differs from original copy pixels");
      renderer.allowBackdropCopy(true);
    });
    test("brightness zero is black and half-white is approximately 128 gray",[&] {
      load(whiteFile);const auto black=frame(renderer,0,1,0);
      require(!renderer.usingBackdropCopy(),"Zero brightness incorrectly bypassed grading through copy");
      const auto half=frame(renderer,.5f,1,0);
      for(std::size_t offset=0;offset<black.size();offset+=4) for(unsigned channel=0;channel<3;++channel) {
        require(black[offset+channel]==0,"Brightness zero did not remove the background");
        require(std::abs(static_cast<int>(half[offset+channel])-128)<=1,"Half-white did not produce mid-gray display RGB");
      }
    });
    test("zero saturation removes visible chroma without tinting white",[&] {
      load(colorFile);const auto gray=frame(renderer,1,0,0);
      require(!renderer.usingBackdropCopy(),"Non-neutral saturation incorrectly used opaque copy");
      require(chroma(pixel(baseline,96,16))>200 && chroma(pixel(gray,96,16))<=1,"Red background did not become achromatic");
      require(chroma(pixel(gray,16,96))<=1,"Blue background did not become achromatic");
      require(pixel(gray,16,16)==Rgb{255,255,255},"Desaturation tinted the white area");
      require(pixel(gray,96,96)==Rgb{0,0,0},"Desaturation lifted the black area");
    });
    test("hue rotates red toward green while white stays neutral",[&] {
      const auto rotated=frame(renderer,1,1,120);const auto green=pixel(rotated,96,16);
      require(!renderer.usingBackdropCopy(),"Non-neutral hue incorrectly used opaque copy");
      require(green[1]>200 && green[1]>green[0]+100 && green[1]>green[2]+100,"120-degree red hue did not become predominantly green");
      load(whiteFile);require(frame(renderer,1,1,120)==white,"Hue rotation colored pure white");
    });
    test("zero and full-turn hue are pixel-identical",[&] {
      load(colorFile);const auto zero=frame(renderer,1,1,0),fullTurn=frame(renderer,1,1,360);
      require(zero==baseline && fullTurn==zero,"Full-turn hue changed visible pixels");
    });
    test("non-neutral grading disables copy and resetting neutral restores it",[&] {
      frame(renderer,.75f,1,0);require(!renderer.usingBackdropCopy(),"Dim background still used ungraded copy");
      frame(renderer,1,.5f,0);require(!renderer.usingBackdropCopy(),"Desaturated background still used ungraded copy");
      frame(renderer,1,1,120);require(!renderer.usingBackdropCopy(),"Hue-shifted background still used ungraded copy");
      require(frame(renderer,1,1,0)==baseline && renderer.usingBackdropCopy(),"Neutral reset did not restore original bytes and copy fast path");
    });
    test("RGB tint colors white without replacing its source and resets exactly",[&] {
      load(whiteFile);const auto* sourceView=renderer.background();
      renderer.setBackgroundAppearance(1,1,0,{64,128,255});renderer.beginFrame();const auto blue=capture(renderer);opaque(blue);
      require(!renderer.usingBackdropCopy(),"Colored background tint incorrectly used ungraded copy");
      require(renderer.background()==sourceView,"Appearance changed the original background source SRV");
      for(unsigned y=0;y<Height;++y) for(unsigned x=0;x<Width;++x) {
        const auto color=pixel(blue,x,y);
        require(std::abs(static_cast<int>(color[0])-64)<=1 && std::abs(static_cast<int>(color[1])-128)<=1 && color[2]==255,
          "Blue RGB tint did not visibly color pure white as selected");
      }
      require(frame(renderer,1,1,0)==white && renderer.usingBackdropCopy(),"Resetting white neutral did not restore original PNG bytes");
      require(renderer.background()==sourceView,"Neutral reset replaced the source background texture");
      load(colorFile);
    });
    test("black tint stays opaque through grading and preserves the backdrop texture",[&] {
      load(colorFile);
      const auto *sourceView=renderer.background();
      renderer.setBackgroundAppearance(4,2,120,{0,0,0});
      renderer.beginFrame();
      require(capture(renderer)==solid(Width,Height,{0,0,0}),"Black tint did not produce opaque black");
      require(!renderer.usingBackdropCopy() && renderer.background()==sourceView,
              "Black tint changed the underlying backdrop resource");
      require(frame(renderer,.75f,.5f,120)!=baseline,"Non-neutral grade after black tint was not applied");
      require(frame(renderer,1,1,0)==baseline,"Reset after black tint changed the original pixels");
    });
    test("higher brightness reveals color without whitening it",[&] {
      const auto brighter=frame(renderer,4,1,0);const auto before=pixel(baseline,16,96),after=pixel(brighter,16,96);
      require(after[0]>=before[0] && after[1]>=before[1] && after[2]>before[2],"Higher brightness did not brighten a non-clipped blue area");
      require(chroma(after)>80 && after[2]>after[0]+80,"Higher brightness flattened the colored area to white");
    });
    test("background grading leaves later additive effect pixels unchanged",[&] {
      load(colorFile);action75::SpriteBurst marker;
      require(marker.load(renderer.device(),atlasFile,{2,1,1,192,false},error,true),"Load generated Sprite marker: "+error);
      require(marker.push(1,.75f,.75f),"Push isolated Sprite marker");
      const auto layered=[&](float brightness,float saturation,float hue,const Rgb& tint=Rgb{255,255,255}) {
        renderer.setBackgroundAppearance(brightness,saturation,hue,tint);renderer.beginFrame();
        require(marker.draw(renderer.context(),Width,Height,1.25,error),"Draw additive marker: "+error);
        auto pixels=capture(renderer);opaque(pixels);return pixels;
      };
      const auto original=layered(1,1,0);const auto markerPixel=pixel(original,96,96);
      require(chroma(markerPixel)>100 && markerPixel[1]>150,"Marker fixture is not visibly colored");
      const std::array<std::array<float,3>,4> grades{{{{0,1,0}},{{.5f,1,0}},{{1,0,0}},{{1,1,120}}}};
      for(const auto& grade:grades) {
        const auto graded=layered(grade[0],grade[1],grade[2]);
        // This central region lies in black background and well inside the marker.
        // A final-image brightness/saturation/hue pass would visibly alter it.
        for(unsigned y=90;y<103;++y) for(unsigned x=90;x<103;++x)
          require(pixel(graded,x,y)==pixel(original,x,y),"Background controls changed independently rendered effect RGB");
        require(pixel(graded,96,16)!=pixel(original,96,16),"Layering test did not actually change the red background");
      }
      const auto tinted=layered(1,1,0,{64,128,255});
      require(pixel(tinted,16,16)==Rgb{64,128,255},"Layering tint did not visibly color the white background");
      require(pixel(tinted,96,96)==markerPixel,"Background RGB tint changed the later additive effect");
      require(frame(renderer,1,1,0)==baseline && renderer.usingBackdropCopy(),"Marker test left neutral background or fast path changed");
    });
    std::cout<<"BackgroundAppearance: "<<passed<<" WARP pixel tests passed; 128x128 temporary PNGs, no window/input/live state.\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<"BackgroundAppearance: "<<error.what()<<'\n';return 1;
  }
}
