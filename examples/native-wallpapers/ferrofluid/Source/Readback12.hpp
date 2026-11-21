#pragma once
#include "Action75Wallpaper12.h"
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace readback {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

inline void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
inline void hr(HRESULT result, const char* what) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", what, static_cast<unsigned long>(result));
        throw std::runtime_error(what);
    }
}
struct Library {
    HMODULE handle;
    explicit Library(const fs::path& path) : handle(LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
        check(handle, "load package");
    }
    ~Library() { FreeLibrary(handle); }
};
struct EventHandle {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~EventHandle() { if (handle) CloseHandle(handle); }
};
template<class Interface>
struct Plugin {
    Library library;
    const Interface* api = nullptr;
    void* instance = nullptr;
    explicit Plugin(const fs::path& pack) : library(pack / L"wallpaper.dll") {}
    ~Plugin() { if (instance) api->destroy(instance); }
    void event(const A75Event& e) { api->on_event(instance, &e); }
};

inline void readRows(Bytes& output, const unsigned char* pixels, size_t stride, UINT width, UINT height) {
    const size_t rowBytes = size_t(width) * 4;
    output.resize(rowBytes * height);
    for (UINT y = 0; y < height; ++y)
        std::copy_n(pixels + y * stride, rowBytes, output.data() + y * rowBytes);
}

struct Warp12 {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12Resource> color, staging;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> debug;
    EventHandle done;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 submission = 0, bytes = 0;
    Plugin<A75Wallpaper12> plugin;
    UINT width, height;

    Warp12(const fs::path& pack, UINT w, UINT h) : plugin(pack), width(w), height(h) {
        auto get = reinterpret_cast<A75GetWallpaper12>(GetProcAddress(plugin.library.handle, "Action75GetWallpaper12"));
        plugin.api = get ? get(ACTION75_WALLPAPER_ABI12) : nullptr;
        check(plugin.api && plugin.api->size >= sizeof(A75Wallpaper12), "ABI 2 entry");
        ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create factory");
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "choose D3D12 WARP");
        hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "create D3D12 WARP device");
        device.As(&debug);
        const D3D12_COMMAND_QUEUE_DESC queueDesc{};
        hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create command queue");
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)), "create allocator");
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
                                    IID_PPV_ARGS(&commands)), "create command list");
        hr(commands->Close(), "initial close");
        hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)), "create fence");
        check(done.handle, "fence event");
        const D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};
        hr(device->CreateDescriptorHeap(&rtvDesc,IID_PPV_ARGS(&rtv)), "create RTV heap");
        resize(w, h);
        const A75Create12 cfg{sizeof(cfg),w,h,device.Get(),pack.c_str(),"{}"};
        plugin.instance = plugin.api->create(&cfg);
        checkDebug();
        check(plugin.instance, "create ABI 2 instance");
    }
    void resize(UINT w, UINT h) {
        width = w;
        height = h;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource.Width = w;
        resource.Height = h;
        resource.DepthOrArraySize = resource.MipLevels = resource.SampleDesc.Count = 1;
        resource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,IID_PPV_ARGS(color.ReleaseAndGetAddressOf())), "create ABI 2 target");
        device->CreateRenderTargetView(color.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
        device->GetCopyableFootprints(&resource,0,1,0,&footprint,nullptr,nullptr,&bytes);
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = bytes;
        resource.Height = 1;
        resource.Format = DXGI_FORMAT_UNKNOWN;
        resource.Flags = D3D12_RESOURCE_FLAG_NONE;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(staging.ReleaseAndGetAddressOf())), "create ABI 2 staging");
    }
    void checkDebug() const {
        if (!debug) return;
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
            SIZE_T bytesForMessage = 0;
            hr(debug->GetMessage(i,nullptr,&bytesForMessage), "query debug message size");
            std::vector<unsigned char> storage(bytesForMessage);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            hr(debug->GetMessage(i,message,&bytesForMessage), "read debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "D3D12 %d: %s\n", int(message->ID), message->pDescription);
                throw std::runtime_error("D3D12 debug validation failed");
            }
        }
        debug->ClearStoredMessages();
    }
    int render(double seconds, Bytes& result) {
        hr(allocator->Reset(), "reset allocator");
        hr(commands->Reset(allocator.Get(),nullptr), "reset commands");
        const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
        const auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
        commands->RSSetViewports(1,&viewport);
        commands->RSSetScissorRects(1,&scissor);
        commands->OMSetRenderTargets(1,&handle,FALSE,nullptr);
        const A75Frame12 frame{sizeof(frame),width,height,seconds,commands.Get(),color.Get(),handle};
        const int delay = plugin.api->render(plugin.instance, &frame);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {color.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1,&barrier);
        const D3D12_TEXTURE_COPY_LOCATION source{color.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_TEXTURE_COPY_LOCATION dest{staging.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,footprint};
        commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commands->ResourceBarrier(1,&barrier);
        hr(commands->Close(), "close commands");
        ID3D12CommandList* lists[]{commands.Get()};
        queue->ExecuteCommandLists(1,lists);
        hr(queue->Signal(fence.Get(),++submission), "signal fence");
        hr(fence->SetEventOnCompletion(submission,done.handle), "arm fence event");
        check(WaitForSingleObject(done.handle,5000)==WAIT_OBJECT_0 && fence->GetCompletedValue()==submission,
              "wait for ABI 2 target");
        checkDebug();
        void* mapped = nullptr;
        const D3D12_RANGE read{0,static_cast<SIZE_T>(bytes)};
        const D3D12_RANGE noWrite{};
        hr(staging->Map(0,&read,&mapped), "map ABI 2 pixels");
        readRows(result, static_cast<const unsigned char*>(mapped) + footprint.Offset, footprint.Footprint.RowPitch,
                 width, height);
        staging->Unmap(0,&noWrite);
        return delay;
    }
};

} // namespace readback
