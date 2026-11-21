#include "Action75Wallpaper12.h"
#include <cstdio>
#include <cstring>
#include <dxgi1_4.h>
#include <utility>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#expr); return 1; } } while(false)
int wmain(int argc,wchar_t **argv) {
    CHECK(argc==2);
    HMODULE module=LoadLibraryW(argv[1]); CHECK(module);
    auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));
    CHECK(get && !get(0));
    auto api=get(ACTION75_WALLPAPER_ABI12);
    CHECK(api && api->abi==ACTION75_WALLPAPER_ABI12 && api->size>=sizeof(A75Wallpaper12));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    CHECK(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
    constexpr unsigned width=960,height=360;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=width; desc.Height=height; desc.MipLevels=desc.DepthOrArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target,readback;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target))));
    const D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};
    ComPtr<ID3D12DescriptorHeap> rtv;
    CHECK(SUCCEEDED(device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtv))));
    const auto handle=rtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(),nullptr,handle);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes=0;
    device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC output{};
    output.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    output.Width=bytes; output.Height=output.MipLevels=output.DepthOrArraySize=output.SampleDesc.Count=1;
    output.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&output,
          D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC q{};
    CHECK(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))));
    CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));
    CHECK(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
          IID_PPV_ARGS(&commands))) && SUCCEEDED(commands->Close()));
    CHECK(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));
    struct WaitEvent {
        HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        ~WaitEvent() { if (value) CloseHandle(value); }
    } done;
    CHECK(done.value);
    UINT64 submitted=0;
    const auto begin=[&](){
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(commands->Reset(allocator.Get(),nullptr));
    };
    const auto submit=[&](){
        if (FAILED(commands->Close())) return false;
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1,lists);
        ++submitted;
        return SUCCEEDED(queue->Signal(fence.Get(),submitted)) &&
               SUCCEEDED(fence->SetEventOnCompletion(submitted,done.value)) &&
               WaitForSingleObject(done.value,5000)==WAIT_OBJECT_0 &&
               fence->GetCompletedValue()==submitted;
    };
    bool copied=false;
    auto pixel=[&](int x,int y){
        if (!copied) {
            if (!begin()) return 0u;
            D3D12_RESOURCE_BARRIER b{};
            b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={target.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
            commands->ResourceBarrier(1,&b);
            const D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,footprint};
            D3D12_TEXTURE_COPY_LOCATION src{target.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
            commands->ResourceBarrier(1,&b);
            if (!submit()) return 0u;
            copied=true;
        }
        void *mapped=nullptr;
        const auto offset=size_t(y)*footprint.Footprint.RowPitch+size_t(x)*4;
        const D3D12_RANGE range{offset,offset+4};
        if (FAILED(readback->Map(0,&range,&mapped))) return 0u;
        const auto *p=static_cast<const unsigned char*>(mapped)+offset;
        const unsigned result=(unsigned(p[3])<<24)|(unsigned(p[0])<<16)|(unsigned(p[1])<<8)|p[2];
        const D3D12_RANGE none{};
        readback->Unmap(0,&none);
        return result;
    };
    A75Create12 cfg={sizeof(cfg),width,height,device.Get(),L".","{}"};
    void *game=api->create(&cfg); CHECK(game);
    A75Frame12 frame={sizeof(frame),width,height,0,commands.Get(),target.Get(),handle};
    auto render=[&](){
        copied=false;
        if (!begin()) return -2;
        frame.seconds+=1./60;
        const auto delay=api->render(game,&frame);
        return submit() ? delay : -2;
    };
    auto emit=[&](unsigned type,const char *code=nullptr,const char *json=nullptr){
        A75Event e={sizeof(e),type,width,height,frame.seconds,0,0,code,json};
        api->on_event(game,&e);
    };
    CHECK(render()==-1 && pixel(93,246)==0xff86efce);
    struct Case {const char* update;const char* reset;bool foreground;};
    const Case appearance[]={
        {R"({"runnerStyle":2})",R"({"runnerStyle":0})",true},
        {R"({"runnerBrightness":0.5})",R"({"runnerBrightness":1})",true},
        {R"({"runnerHue":120})",R"({"runnerHue":0})",true},
        {R"({"runnerSaturation":0})",R"({"runnerSaturation":1})",true},
        {R"({"runnerColor":[255,68,36]})",R"({"runnerColor":[255,255,255]})",true},
        {R"({"stationStyle":2})",R"({"stationStyle":0})",false},
        {R"({"stationBrightness":0.5})",R"({"stationBrightness":1})",false},
        {R"({"stationHue":120})",R"({"stationHue":0})",false},
        {R"({"stationSaturation":0})",R"({"stationSaturation":1})",false},
        {R"({"stationColor":[255,68,36]})",R"({"stationColor":[255,255,255]})",false}
    };
    for(const Case& test:appearance){
        emit(A75_SETTINGS,nullptr,test.update); CHECK(render()==-1);
        CHECK((pixel(93,246)!=0xff86efce)==test.foreground);
        CHECK((pixel(14,10)!=0xff101b2b)!=test.foreground);
        emit(A75_SETTINGS,nullptr,test.reset); CHECK(render()==-1);
        CHECK(pixel(93,246)==0xff86efce && pixel(14,10)==0xff101b2b);
    }
    emit(A75_KEY_DOWN,"ArrowRight");
    for (int i=0;i<32;++i) CHECK(render()==0);
    emit(A75_KEY_UP,"ArrowRight"); CHECK(render()==-1);
    CHECK(pixel(93,246)!=0xff86efce && pixel(101,24)==0xffdcefeb && pixel(92,26)!=0xffdcefeb);
    emit(A75_SETTINGS,nullptr,"{ \"amber_lights\" : true }"); CHECK(render()==-1 && pixel(50,270)==0xffffbb59);
    emit(A75_KEY_DOWN,"Space"); CHECK(render()==0); emit(A75_INPUT_RESET);
    emit(A75_PAUSE); CHECK(render()==-1 && pixel(456,138)==0xffffbb59);
    emit(A75_RESUME); CHECK(render()==0 && pixel(456,138)!=0xffffbb59);
    emit(A75_KEY_DOWN,"Enter"); CHECK(render()==-1);
    emit(A75_KEY_DOWN,"Enter"); CHECK(render()==-1);
    emit(A75_KEY_UP,"Enter"); emit(A75_KEY_DOWN,"Enter"); CHECK(render()==0);
    for (int i=0;i<120;++i) render();
    CHECK(render()==-1);
    emit(A75_KEY_DOWN,"ArrowRight");
    for (int i=0;i<155;++i) render();
    CHECK(render()==-1 && pixel(93,246)==0xffffbb59 && pixel(196,26)!=0xffdcefeb);
    api->destroy(game); FreeLibrary(module);
    std::puts("ABI 2 RGBA8 WARP game readback passed, including amber RGB(255,187,89)");
    return 0;
}
