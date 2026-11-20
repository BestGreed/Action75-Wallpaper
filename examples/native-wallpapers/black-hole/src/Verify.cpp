#include "Action75Wallpaper.h"
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void __cdecl onPatch(void *host, const char *patch) { *static_cast<std::string *>(host) = patch; }

void run(const std::filesystem::path &pack) {
  const auto library = LoadLibraryW((pack / L"wallpaper.dll").c_str());
  expect(library != nullptr,"DLL load");
  const auto entry = reinterpret_cast<A75GetWallpaper>(GetProcAddress(library,"Action75GetWallpaper"));
  expect(entry && !entry(99),"ABI negotiation");
  const auto api = entry(ACTION75_WALLPAPER_ABI);
  expect(api && api->size == sizeof(*api),"v1 interface");
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  expect(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP creation");
  constexpr UINT w=640,h=240;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width=w; desc.Height=h; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
  desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> target,staging;
  ComPtr<ID3D11RenderTargetView> view;
  expect(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)),"target");
  expect(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&view)),"view");
  desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.BindFlags=0;
  expect(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"staging");
  std::string patch;
  const A75Create create{sizeof(A75Create),w,h,device.Get(),context.Get(),pack.c_str(),"{}",&patch,onPatch};
  void *instance=api->create(&create);
  expect(instance,"instance");
  auto event = [&](uint32_t type,float x,float y,double t,const char *settings=nullptr) {
    const A75Event e{sizeof(A75Event),type,w,h,t,x,y,"KeyQ",settings};
    api->on_event(instance,&e);
  };
  auto pixels=[&](double seconds) {
    const A75Frame f{sizeof(A75Frame),w,h,seconds,view.Get()};
    expect(api->render(instance,&f)>=-1,"render");
    context->CopyResource(staging.Get(),target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"readback");
    std::vector<uint32_t> data(w*h);
    for (UINT y=0;y<h;y++)
      memcpy(data.data()+w*y,static_cast<char *>(mapped.pData)+mapped.RowPitch*y,w*sizeof(uint32_t));
    context->Unmap(staging.Get(),0);
    return data;
  };
  expect(api->hit_test(instance,.58f,.46f,w,h) && !api->hit_test(instance,.1f,.1f,w,h),"hit testing");
  const auto nebula=pixels(1);
  event(A75_SETTINGS,0,0,1,R"({"skyMode":1})");
  const auto dark=pixels(1);
  event(A75_SETTINGS,0,0,1,R"({"skyMode":2})");
  expect(nebula!=dark && pixels(1)!=dark,"three skies");
  event(A75_SETTINGS,0,0,1,R"({"skyMode":1,"starBrightness":0})");
  event(A75_KEY_DOWN,.15f,.3f,1);
  const auto zero=pixels(1.2);
  event(A75_SETTINGS,0,0,1.2,R"({"skyMode":1,"starBrightness":1})");
  event(A75_KEY_DOWN,.15f,.3f,1.1);
  expect(zero!=pixels(1.2),"key-generated star");
  event(A75_POINTER_DOWN,.58f,.46f,1.3);
  event(A75_POINTER_MOVE,.42f,.66f,1.3);
  event(A75_POINTER_UP,.42f,.66f,1.3);
  expect(patch.find("\"centerX\":42.0")!=std::string::npos &&
         patch.find("\"centerY\":66.0")!=std::string::npos,"drag patch");
  expect(api->hit_test(instance,.42f,.66f,w,h) && !api->hit_test(instance,.58f,.46f,w,h),"moved center");
  event(A75_SETTINGS,0,0,1.4,patch.c_str());
  event(A75_POINTER_DOWN,.42f,.66f,1.4);
  event(A75_POINTER_MOVE,.28f,.35f,1.4);
  event(A75_INPUT_RESET,0,0,1.4);
  expect(api->hit_test(instance,.42f,.66f,w,h),"cancelled drag");
  api->destroy(instance);
  FreeLibrary(library);
}
}
int wmain(int argc,wchar_t **argv) {
  if(argc!=2) return 2;
  if (FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) return 3;
  try { run(std::filesystem::absolute(argv[1])); }
  catch(const std::exception &ex) {std::cerr<<ex.what()<<'\n';return 1;}
  CoUninitialize();
  std::cout<<"Event Horizon ABI, render, skies, key star, hit test, drag persistence and cancellation passed.\n";
}
