#include "Action75Wallpaper12.h"
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using Pixels=std::vector<unsigned char>;
static void require(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
static void hr(HRESULT x,const char*why){if(FAILED(x)){std::cerr<<why<<" HRESULT="<<std::hex<<unsigned(x)<<std::dec<<'\n';throw std::runtime_error(why);}}
static void png(const fs::path&file,const Pixels&bytes,unsigned w,unsigned h){
 ComPtr<IWICImagingFactory>f;ComPtr<IWICStream>s;ComPtr<IWICBitmapEncoder>e;ComPtr<IWICBitmapFrameEncode>frame;ComPtr<IWICBitmap>b;
 hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC");hr(f->CreateStream(&s),"WIC stream");hr(s->InitializeFromFilename(file.c_str(),GENERIC_WRITE),"PNG file");
 hr(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&e),"PNG encoder");hr(e->Initialize(s.Get(),WICBitmapEncoderNoCache),"PNG init");hr(e->CreateNewFrame(&frame,nullptr),"PNG frame");hr(frame->Initialize(nullptr),"PNG frame init");hr(frame->SetSize(w,h),"PNG size");
 WICPixelFormatGUID fmt=GUID_WICPixelFormat32bppRGBA;hr(frame->SetPixelFormat(&fmt),"PNG format");hr(f->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppRGBA,w*4,UINT(bytes.size()),const_cast<BYTE*>(bytes.data()),&b),"PNG memory");hr(frame->WriteSource(b.Get(),nullptr),"PNG pixels");hr(frame->Commit(),"PNG frame commit");hr(e->Commit(),"PNG commit");
}
struct Gpu12{
 ComPtr<ID3D12Device>device;ComPtr<ID3D12CommandQueue>queue;ComPtr<ID3D12CommandAllocator>allocator;ComPtr<ID3D12GraphicsCommandList>list;ComPtr<ID3D12Fence>fence;ComPtr<ID3D12Resource>target,readback;ComPtr<ID3D12DescriptorHeap>views;
 D3D12_CPU_DESCRIPTOR_HANDLE rtv{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0,serial=0;HANDLE ready=nullptr;unsigned w,h;
 Gpu12(unsigned width,unsigned height):w(width),h(height){
  ComPtr<IDXGIFactory4>factory;ComPtr<IDXGIAdapter>adapter;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI");hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"DX12 WARP");
  D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"List");hr(list->Close(),"Initial close");hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(ready,"Fence event");
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.MipLevels=d.DepthOrArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"Target");D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};hr(device->CreateDescriptorHeap(&vd,IID_PPV_ARGS(&views)),"RTV heap");rtv=views->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target.Get(),nullptr,rtv);device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
  d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.Format=DXGI_FORMAT_UNKNOWN;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_NONE;heap.Type=D3D12_HEAP_TYPE_READBACK;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Readback");
 }
 ~Gpu12(){if(ready)CloseHandle(ready);}
 Pixels draw(const A75Wallpaper12*api,void*instance,double time,int&schedule){
  hr(allocator->Reset(),"Reset allocator");hr(list->Reset(allocator.Get(),nullptr),"Reset list");D3D12_VIEWPORT vp{0,0,float(w),float(h),0,1};D3D12_RECT sc{0,0,LONG(w),LONG(h)};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  A75Frame12 frame{sizeof(frame),w,h,time,list.Get(),target.Get(),rtv};schedule=api->render(instance,&frame);require(schedule>=-1,"Plugin render failure");D3D12_RESOURCE_BARRIER b{};b.Transition={target.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);
  D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);hr(list->Close(),"Close frame");ID3D12CommandList*work[]={list.Get()};queue->ExecuteCommandLists(1,work);hr(queue->Signal(fence.Get(),++serial),"Signal");hr(fence->SetEventOnCompletion(serial,ready),"Arm fence");require(WaitForSingleObject(ready,20000)==WAIT_OBJECT_0,"Readback timeout");
  void*p=nullptr;D3D12_RANGE range{0,SIZE_T(bytes)};hr(readback->Map(0,&range,&p),"Map DX12");Pixels result(size_t(w)*h*4);for(unsigned y=0;y<h;++y)std::copy_n(static_cast<unsigned char*>(p)+y*fp.Footprint.RowPitch,w*4,result.data()+size_t(y)*w*4);D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);return result;
 }
};
// Opacity checks use actual ABI2 DLL readback, including the frozen baseline.
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
struct Plugin {
 HMODULE dll=nullptr;const A75Wallpaper12*api=nullptr;fs::path pack;
 explicit Plugin(fs::path p):pack(std::move(p)){dll=LoadLibraryW((pack/L"wallpaper.dll").c_str());require(dll,"Load package DLL");auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(dll,"Action75GetWallpaper12"));require(get&&!get(1)&&get(2),"Public ABI2 negotiation");api=get(2);}
 ~Plugin(){if(dll)FreeLibrary(dll);}
};
struct Instance {
 Plugin&p;Gpu12&g;void*ptr=nullptr;int schedule=-9;
 Instance(Plugin&plugin,Gpu12&gpu,const std::string&settings):p(plugin),g(gpu){A75Create12 c{sizeof(c),g.w,g.h,g.device.Get(),p.pack.c_str(),settings.c_str(),nullptr,nullptr};ptr=p.api->create(&c);require(ptr,"Create ABI2 instance");}
 ~Instance(){if(ptr)p.api->destroy(ptr);}
 void event(unsigned type,double time,const char*settings="{}",float x=.5f,float y=.44f){A75Event e{sizeof(e),type,g.w,g.h,time,x,y,"Space",settings};p.api->on_event(ptr,&e);}
 Pixels draw(double time){return g.draw(p.api,ptr,time,schedule);}
};
static unsigned maxDiff(const Pixels&a,const Pixels&b){require(a.size()==b.size(),"Pixel dimensions");unsigned maximum=0;for(size_t i=0;i<a.size();++i)maximum=std::max(maximum,unsigned(std::abs(int(a[i])-int(b[i]))));return maximum;}
static bool empty(const Pixels&p){return std::all_of(p.begin(),p.end(),[](unsigned char c){return c==0;});}
static void premul(const Pixels&p){for(size_t i=0;i<p.size();i+=4)for(unsigned c=0;c<3;++c)require(p[i+c]<=p[i+3],"RGB exceeds alpha: output must be premultiplied");}
static Pixels desktop(const Pixels&p,std::array<unsigned char,3>bg){auto out=p;for(size_t i=0;i<p.size();i+=4){for(unsigned c=0;c<3;++c)out[i+c]=static_cast<unsigned char>(std::min(255u,unsigned(p[i+c])+unsigned(std::lround(bg[c]*(1-p[i+3]/255.0)))));out[i+3]=255;}return out;}
int wmain(int argc,wchar_t**argv){try{
 require(argc==4,"Pass current pack, frozen ABI2 baseline pack, evidence directory");hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");const fs::path out=fs::absolute(argv[3]);fs::create_directories(out);Plugin candidate(fs::absolute(argv[1])),baseline(fs::absolute(argv[2]));Gpu12 gpu(1920,720);nlohmann::json report={{"renderer","D3D12 WARP"},{"width",gpu.w},{"height",gpu.h},{"baselineSamples",nlohmann::json::array()},{"opacityProfiles",nlohmann::json::array()}};unsigned baselineSamples=0;
 // Compare the unchanged legacy response against this round's real old DLL.
 for(const auto&profile:std::vector<std::string>{"{}",R"({"sky_program":1,"tap_cadence":1})",R"({"sky_program":4,"tap_cadence":1})",R"({"sky_program":7,"tap_cadence":1,"backgroundStyle":1,"backgroundColor":[20,150,190],"backgroundBrightness":0.4,"backgroundHue":210,"backgroundSaturation":1.6})",R"({"tap_cadence":1,"backgroundStyle":1,"backgroundColor":[255,255,255]})",R"({"tap_cadence":1,"backgroundStyle":1,"backgroundBrightness":0})"}){
  Instance a(candidate,gpu,profile),b(baseline,gpu,profile);auto explicitProfile=nlohmann::json::parse(profile);explicitProfile["backgroundOpacity"]=100;explicitProfile["foregroundOpacity"]=100;Instance explicitOpacity(candidate,gpu,explicitProfile.dump());
  for(double time:{0.,1.05,1.35,1.85,3.,9.}){
   if(time==1.05)for(unsigned i=0;i<6;++i){const double t=1+i*.006;const float x=.25f+.25f*(i/2);a.event(A75_KEY_DOWN,t,"{}",x,.44f);b.event(A75_KEY_DOWN,t,"{}",x,.44f);explicitOpacity.event(A75_KEY_DOWN,t,"{}",x,.44f);}
   auto actual=a.draw(time),old=b.draw(time),explicitPixels=explicitOpacity.draw(time);const unsigned difference=maxDiff(actual,old);require(difference==0,"100/100 differs from frozen ABI2 baseline");require(actual==explicitPixels,"Old profile must default both new values to 100");require(a.schedule==b.schedule,"Legacy schedule changed");report["baselineSamples"].push_back({{"settings",nlohmann::json::parse(profile)},{"time",time},{"maxAbsRGBA",difference},{"schedule",a.schedule}});++baselineSamples;
  }
 }
 // Nine combinations prove independent foreground-over-background composition.
 for(unsigned style=0;style<3;++style){
  const auto settings=[&](unsigned background,unsigned foreground){nlohmann::json j={{"sky_program",7},{"tap_cadence",1},{"backgroundOpacity",background},{"foregroundOpacity",foreground}};if(style){j["backgroundStyle"]=1;j["backgroundColor"]=style==1?nlohmann::json{255,255,255}:nlohmann::json{4,8,16};}return j.dump();};
  std::array<std::array<Pixels,3>,3>frames;Pixels idleBase;
  for(unsigned bi=0;bi<3;++bi)for(unsigned fi=0;fi<3;++fi){Instance i(candidate,gpu,settings(bi*50,fi*50));const auto idle=i.draw(0);require(i.schedule==-1,"Idle must sleep");if(bi==0)require(empty(idle),"Zero background idle must be transparent 0000");if(bi==2&&fi==0)idleBase=idle;i.event(A75_KEY_DOWN,1);frames[bi][fi]=i.draw(1.35);premul(frames[bi][fi]);require(i.schedule==0,"Hidden or transparent foreground must still retain active input and timing");if(fi==0)require(frames[bi][fi]==idle,"Foreground zero must remove particles, glow, smoke and reflections");
   const std::string name="style-"+std::to_string(style)+"-b"+std::to_string(bi*50)+"-f"+std::to_string(fi*50);png(out/(name+"-rgba.png"),frames[bi][fi],gpu.w,gpu.h);png(out/(name+"-dark.png"),desktop(frames[bi][fi],{20,25,32}),gpu.w,gpu.h);png(out/(name+"-light.png"),desktop(frames[bi][fi],{242,242,242}),gpu.w,gpu.h);
  }
  require(empty(frames[0][0]),"0/0 active frame must be transparent");require(!empty(frames[0][2]),"Background zero must retain real key effects");unsigned independenceError=0,nonzeroForeground=0,transparentForeground=0;
  const auto&foreground=frames[0][2];const auto&base=frames[2][0];for(size_t i=0;i<foreground.size();i+=4){nonzeroForeground+=foreground[i+3]>0;transparentForeground+=foreground[i+3]==0;for(unsigned bi=0;bi<3;++bi)for(unsigned fi=0;fi<3;++fi){const double bo=bi*.5,fo=fi*.5,alpha=foreground[i+3]/255.0*fo;for(unsigned c=0;c<4;++c){const double expected=c==3?255*(alpha+bo*(1-alpha)):foreground[i+c]*fo+base[i+c]*bo*(1-alpha);const unsigned error=unsigned(std::abs(int(frames[bi][fi][i+c])-int(std::lround(expected))));independenceError=std::max(independenceError,error);}}}
  require(independenceError<=2,"Independent layers violate standard premultiplied over");require(nonzeroForeground&&transparentForeground,"Foreground must have visible coverage and empty transparent pixels");report["opacityProfiles"].push_back({{"style",style},{"combinations",9},{"maxOverEquationErrorRGBA8",independenceError},{"foregroundPixels",nonzeroForeground},{"transparentForegroundPixels",transparentForeground}});
 }
 // Live settings, pause, re-enable hidden input, expiry, and reset.
 Instance live(candidate,gpu,R"({"sky_program":7,"tap_cadence":1,"backgroundOpacity":0})");require(empty(live.draw(0))&&live.schedule==-1,"Transparent idle sleep");live.event(A75_KEY_DOWN,1);auto visible=live.draw(1.35);require(!empty(visible),"Transparent background accepts key");live.event(A75_PAUSE,1.35);require(live.draw(3)==visible&&live.schedule==-1,"Pause freezes transparent effect");live.event(A75_SETTINGS,3.1,R"({"foregroundOpacity":0})");require(empty(live.draw(3.2)),"Both hidden while paused");live.event(A75_SETTINGS,3.3,R"({"foregroundOpacity":100})");require(live.draw(3.4)==visible,"Re-enable must retain full frozen firework");live.event(A75_RESUME,4);require(!empty(live.draw(4.2))&&live.schedule==0,"Resume must retain motion");require(empty(live.draw(20))&&live.schedule==-1,"Expired transparent scene sleeps");live.event(A75_SETTINGS,21,R"({"foregroundOpacity":0})");live.event(A75_KEY_DOWN,22);require(empty(live.draw(22.35))&&live.schedule==0,"Hidden foreground still accepts keys");live.event(A75_SETTINGS,22.35,R"({"foregroundOpacity":100})");require(!empty(live.draw(22.35)),"Re-enable reveals key accepted while hidden");live.event(A75_INPUT_RESET,22.4);require(empty(live.draw(22.5))&&live.schedule==-1,"Reset clears foreground and sleeps");live.event(A75_SETTINGS,23,R"({"foregroundOpacity":0})");live.event(A75_KEY_DOWN,24);require(empty(live.draw(24.35))&&live.schedule==0,"Hidden activity must keep its timing");require(empty(live.draw(40))&&live.schedule==-1,"Hidden effect must expire on absolute scene clock");live.event(A75_SETTINGS,40.1,R"({"foregroundOpacity":100})");require(empty(live.draw(40.1))&&live.schedule==-1,"Showing a long-hidden foreground must not resurrect expired effects");report["pauseResumeHiddenInputResetIdle"]="pass";report["hiddenBeyondLifetimeThenRestore"]="pass";report["baselineComparisons"]=baselineSamples;report["pass"]=true;std::ofstream(out/L"opacity-results.json")<<report.dump(2)<<'\n';std::cout<<"PASS: "<<baselineSamples<<" frozen-ABI2 exact RGBA comparisons, 27 opacity combinations, premul/desktop/keys/pause/reset/expiry\n";CoUninitialize();return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
