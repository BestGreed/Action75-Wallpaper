#include "Action75Wallpaper12.h"
#include "FireworksScene.h"
#include "SceneSettings.h"
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
struct Gpu11{
 ComPtr<ID3D11Device>device;ComPtr<ID3D11DeviceContext>context;ComPtr<ID3D11Texture2D>target,readback;ComPtr<ID3D11RenderTargetView>rtv;unsigned w,h;
 Gpu11(unsigned width,unsigned height):w(width),h(height){const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context),"Reference DX11 WARP");D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;hr(device->CreateTexture2D(&d,nullptr,&target),"DX11 target");hr(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"DX11 RTV");d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.Usage=D3D11_USAGE_STAGING;hr(device->CreateTexture2D(&d,nullptr,&readback),"DX11 readback");}
 Pixels draw(action75::FireworksScene&scene,double time){auto*view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);const D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);std::string error;require(scene.draw(context.Get(),w,h,time,error),error.c_str());context->CopyResource(readback.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m),"Map DX11");Pixels result(size_t(w)*h*4);for(unsigned y=0;y<h;++y)std::copy_n(static_cast<unsigned char*>(m.pData)+y*m.RowPitch,w*4,result.data()+size_t(y)*w*4);context->Unmap(readback.Get(),0);return result;}
};
struct Verify{
 const A75Wallpaper12*api;fs::path pack,out;Gpu11 legacy;Gpu12 native;action75::FireworksScene scene;action75::Configuration config;void*instance=nullptr;bool paused=false;double offset=0,pauseTime=0;unsigned comparisons=0;std::ofstream csv;
 Verify(const A75Wallpaper12*a,fs::path p,fs::path output):api(a),pack(std::move(p)),out(std::move(output)),legacy(1920,720),native(1920,720){fs::create_directories(out);csv.open(out/L"metrics.csv");csv<<"sample,maxChannelError,meanAbsoluteError,channelsOver2,legacyBursts,legacyShape,schedule\n";reset("{}");}
 ~Verify(){if(instance)api->destroy(instance);}
 double time(double wall)const{return paused?pauseTime:wall-offset;}
 void reset(const char*settings){if(instance)api->destroy(instance);instance=nullptr;paused=false;offset=pauseTime=0;config=action75::Configuration{};config.apply(settings);scene=action75::FireworksScene{};std::string error;require(scene.load(legacy.device.Get(),legacy.w,legacy.h,error),error.c_str());scene.configure(config.appearance);A75Create12 c{sizeof(c),native.w,native.h,native.device.Get(),pack.c_str(),settings,nullptr,nullptr};instance=api->create(&c);require(instance,"Create native DX12 wallpaper");}
 void event(unsigned type,double wall,const char*settings="{}",float x=.5f,float y=.4f){A75Event e{sizeof(e),type,native.w,native.h,wall,x,y,"Space",settings};api->on_event(instance,&e);switch(type){case A75_SETTINGS:config.apply(settings);scene.configure(config.appearance);break;case A75_KEY_DOWN:if(!paused&&scene.readyForKeyBurst())scene.push(time(wall),x,y);break;case A75_POINTER_DOWN:if(!paused)scene.push(time(wall),x,y);break;case A75_INPUT_RESET:scene.clear();break;case A75_PAUSE:if(!paused){pauseTime=time(wall);paused=true;}break;case A75_RESUME:if(paused){offset=wall-pauseTime;paused=false;}break;}}
 Pixels sample(const std::string&label,double wall,bool save=false){const auto old=legacy.draw(scene,time(wall));int schedule=-9;auto current=native.draw(api,instance,wall,schedule);require(schedule==(!paused&&scene.count()?0:-1),"Schedule follows motion/pause");unsigned maximum=0,over=0;UINT64 total=0;for(size_t i=0;i<old.size();++i){const auto d=unsigned(std::abs(int(old[i])-int(current[i])));maximum=std::max(maximum,d);total+=d;over+=d>2;}double mean=double(total)/old.size();csv<<label<<','<<maximum<<','<<mean<<','<<over<<','<<scene.count()<<','<<scene.lastShape()<<','<<schedule<<'\n';csv.flush();if(save){png(out/(label+"-legacy.png"),old,native.w,native.h);png(out/(label+"-dx12.png"),current,native.w,native.h);}if(maximum>3||mean>.015){std::cerr<<label<<" max="<<maximum<<" mean="<<mean<<" over2="<<over<<'\n';throw std::runtime_error("DX12 pixels differ from original scene");}++comparisons;return current;}
};
int wmain(int argc,wchar_t**argv){try{
 require(argc==3,"Pass pack and evidence directory");hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");const auto pack=fs::absolute(argv[1]),out=fs::absolute(argv[2]);auto module=LoadLibraryW((pack/L"wallpaper.dll").c_str());require(module,"Load package DLL");auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));require(get&&!get(1)&&get(2),"ABI2 negotiation");require(!GetProcAddress(module,"Action75GetWallpaper"),"No ABI1 export");
 {Verify v(get(2),pack,out);v.sample("idle",0,true);constexpr unsigned shapes[]{0,7,7,1,1,4};for(unsigned i=0;i<6;++i){v.event(A75_KEY_DOWN,1+i*.1,"{}",.23f+.25f*(i/2),.43f);require(v.scene.lastShape()==shapes[i],"Default sequence/cadence");}require(v.scene.count()==3,"Six keys produce three bursts");v.sample("default-three-shapes",1.85,true);
  for(unsigned shape:{1u,4u,7u}){auto settings=nlohmann::json{{"sky_program",shape},{"tap_cadence",1}}.dump();v.reset(settings.c_str());v.event(A75_KEY_DOWN,.2);for(double age:{.05,.35,.85,1.8,4.0,8.0})v.sample("shape-"+std::to_string(shape)+"-"+std::to_string(int(age*1000)),.2+age,age==.35);}
  v.reset(R"({"tap_cadence":99})");for(unsigned i=0;i<98;++i)v.event(A75_KEY_DOWN,1+i*.001);require(!v.scene.count(),"Maximum cadence waits for all 99 keys");v.sample("cadence-99-before",1.2);v.event(A75_KEY_DOWN,1.3);require(v.scene.count()==1&&v.scene.lastShape()==7,"Key 99 emits the first author shape");v.sample("cadence-99-after",1.6);
  v.reset(R"({"tap_cadence":99})");v.event(A75_POINTER_DOWN,1);require(v.scene.count()==1,"Pointer trigger keeps the original cadence bypass");v.sample("pointer",1.4);
  v.reset(R"({"tap_cadence":1,"palm_fan":false})");for(unsigned i=0;i<4;++i){v.event(A75_KEY_DOWN,1+i*.12);require(v.scene.lastShape()==(i%2?1u:7u),"Subset order");}v.sample("subset-order",1.8,true);
  v.reset(R"({"tap_cadence":1,"mix_stars":true})");std::set<unsigned>seen;for(unsigned i=0;i<12;++i){v.event(A75_KEY_DOWN,1+i*.05);seen.insert(v.scene.lastShape());}require(seen.size()>1&&std::all_of(seen.begin(),seen.end(),[](unsigned x){return x==1||x==4||x==7;}),"Shuffle candidates");v.sample("shuffle-twelve",1.8,true);
  v.reset(R"({"tap_cadence":1,"willow_arc":false,"palm_fan":false,"saturn_ring":false})");v.event(A75_KEY_DOWN,1);require(!v.scene.count(),"Empty selection emits nothing");v.sample("empty-selection",1.4);
  v.reset(R"({"sky_program":7,"tap_cadence":1,"willow_arc":false,"palm_fan":false,"saturn_ring":false})");v.event(A75_KEY_DOWN,1);require(v.scene.lastShape()==7,"Explicit single shape");v.sample("single-selection",1.4);
  v.reset("{}");v.event(A75_SETTINGS,.5,R"({"backgroundStyle":1,"backgroundColor":[20,150,190],"backgroundBrightness":0.4,"backgroundHue":210,"backgroundSaturation":1.6})");v.sample("graded-solid",.5,true);v.event(A75_SETTINGS,1,R"({"backgroundBrightness":0})");auto black=v.sample("solid-black",1);for(size_t i=0;i<black.size();++i)require(i%4==3||black[i]==0,"Zero background brightness");
  v.reset(R"({"sky_program":7,"tap_cadence":1})");v.event(A75_KEY_DOWN,1);auto before=v.sample("before-pause",1.3,true);v.event(A75_PAUSE,1.3);require(v.sample("paused",3,true)==before,"Pause freezes all animation");v.event(A75_KEY_DOWN,3.1);v.event(A75_SETTINGS,3.2,R"({"backgroundBrightness":0.3})");v.sample("paused-background",3.5,true);v.event(A75_RESUME,4);v.sample("resumed",4.2,true);v.sample("expired",10,true);v.event(A75_KEY_DOWN,11);v.event(A75_INPUT_RESET,11.1);v.sample("input-reset",11.2,true);
  std::cout<<"PASS "<<v.comparisons<<" original-DX11/direct-DX12 comparisons; order/cadence/subset/shuffle/background/pause/reset/expiry\n";
 }FreeLibrary(module);CoUninitialize();return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
