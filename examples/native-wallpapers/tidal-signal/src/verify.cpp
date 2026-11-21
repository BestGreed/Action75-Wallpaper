#include "Action75Wallpaper12.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
static void check(bool ok,const char *why) { if (!ok) throw std::runtime_error(why); }
static void hr(HRESULT result,const char *why) { check(SUCCEEDED(result),why); }

int wmain(int argc,wchar_t **argv) {
    try {
        check(argc==2,"Pass a pack directory");
        const auto dir=std::filesystem::absolute(argv[1]);
        auto module=LoadLibraryW((dir/L"wallpaper.dll").c_str()); check(module!=nullptr,"Load plugin");
        auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));
        check(get && !get(1) && get(2),"Negotiate ABI2");
        auto api=get(2);
        ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> device;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 device");
        ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands; ComPtr<ID3D12Fence> fence;
        D3D12_COMMAND_QUEUE_DESC q{};
        hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
        hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Command list");
        hr(commands->Close(),"Close initial list"); hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
        constexpr UINT w=320,h=120;
        ComPtr<ID3D12Resource> target,readback; ComPtr<ID3D12DescriptorHeap> views;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=w; desc.Height=h; desc.MipLevels=desc.SampleDesc.Count=desc.DepthOrArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"Target");
        D3D12_DESCRIPTOR_HEAP_DESC viewDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};
        hr(device->CreateDescriptorHeap(&viewDesc,IID_PPV_ARGS(&views)),"RTV heap");
        const auto rtv=views->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(target.Get(),nullptr,rtv);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1; desc.Format=DXGI_FORMAT_UNKNOWN;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=D3D12_RESOURCE_FLAG_NONE; heap.Type=D3D12_HEAP_TYPE_READBACK;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Readback");
        A75Create12 c{sizeof(c),w,h,device.Get(),dir.c_str(),"{}",nullptr,nullptr};
        void *instance=api->create(&c); check(instance,"Create plugin pipeline");
        UINT64 next=0;
        auto sample=[&](double seconds,int expected,int x,int y) {
            hr(allocator->Reset(),"Reset allocator"); hr(commands->Reset(allocator.Get(),nullptr),"Reset commands");
            D3D12_VIEWPORT vp{0,0,float(w),float(h),0,1}; D3D12_RECT sc{0,0,w,h};
            commands->RSSetViewports(1,&vp); commands->RSSetScissorRects(1,&sc); commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
            A75Frame12 frame{sizeof(frame),w,h,seconds,commands.Get(),target.Get(),rtv};
            check(api->render(instance,&frame)==expected,"Frame scheduling result");
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Transition={target.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
            commands->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=target.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); commands->ResourceBarrier(1,&barrier);
            hr(commands->Close(),"Close render list"); ID3D12CommandList *lists[]={commands.Get()}; queue->ExecuteCommandLists(1,lists);
            hr(queue->Signal(fence.Get(),++next),"Signal fence"); hr(fence->SetEventOnCompletion(next,nullptr),"Await readback");
            unsigned char *mapped=nullptr; D3D12_RANGE range{0,size_t(bytes)}; hr(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map pixels");
            auto pixel=mapped+y*fp.Footprint.RowPitch+x*4; std::array<int,3> rgb{pixel[0],pixel[1],pixel[2]};
            D3D12_RANGE written{0,0}; readback->Unmap(0,&written); return rgb;
        };
        auto emit=[&](unsigned type,double time,const char *json="{}") {
            A75Event e{sizeof(e),type,w,h,time,.25f,.5f,"Space",json}; api->on_event(instance,&e);
        };
        auto idle=sample(0,-1,80,60);
        emit(A75_KEY_DOWN,1); auto beacon=sample(1.1,0,80,60);
        check(beacon[0]==63 && beacon[1]==190 && beacon[2]==161,"Default key pixel");
        check(sample(1.2,0,290,18)[1]<50,"Key effect localized");
        emit(A75_SETTINGS,1.2,R"({"signal_rgb":[240,60,20],"radiance":0.5})");
        check(sample(1.3,0,80,60)==std::array<int,3>{120,30,10},"Color and brightness");
        check(sample(4,-1,80,60)==idle,"Expiration and sleep");
        emit(A75_KEY_DOWN,5); emit(A75_INPUT_RESET,5.1); check(sample(5.2,-1,80,60)==idle,"Input reset");
        api->destroy(instance); FreeLibrary(module);
        std::puts("ABI2 WARP RGBA8 readback verified: key RGB(63,190,161), changed RGB(120,30,10), localization, expiry, reset.");
    } catch (const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
