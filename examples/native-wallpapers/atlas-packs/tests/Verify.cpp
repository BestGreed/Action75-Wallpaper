#include "Graphics.h"
#include "Settings.h"

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace atlas_packs;

namespace {
struct Module {
  HMODULE handle;
  explicit Module(const fs::path& path)
      : handle(LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    if (!handle)
      throw std::runtime_error("Cannot load package DLL, Windows error " + std::to_string(GetLastError()));
  }
  ~Module() { FreeLibrary(handle); }
  FARPROC get(const char* name) const { return GetProcAddress(handle, name); }
};

void expect(bool condition, const char* what) {
  if (!condition)
    throw std::runtime_error(what);
}

class Reference {
public:
  Reference(const fs::path& dir, const std::string& values, unsigned width, unsigned height)
      : width_(width), height_(height), module_(dir / L"wallpaper.dll") {
    auto entry = reinterpret_cast<A75GetWallpaper>(module_.get("Action75GetWallpaper"));
    expect(entry && (api_ = entry(1)), "Old DLL exports ABI 1");
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                            D3D11_SDK_VERSION, &device_, nullptr, &context_), "Create reference WARP device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device_->CreateTexture2D(&desc, nullptr, &texture_), "Create reference target");
    check(device_->CreateRenderTargetView(texture_.Get(), nullptr, &rtv_), "Create reference RTV");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device_->CreateTexture2D(&desc, nullptr, &readback_), "Create reference readback");
    const A75Create create{sizeof(A75Create), width, height, device_.Get(), context_.Get(),
                           dir.c_str(), values.c_str(), nullptr, nullptr};
    instance_ = api_->create(&create);
    expect(instance_, "Create legacy wallpaper");
  }

  ~Reference() {
    if (instance_)
      api_->destroy(instance_);
  }

  void event(const A75Event& event) { api_->on_event(instance_, &event); }

  int render(double seconds, const fs::path& capture) {
    const A75Frame frame{sizeof(A75Frame), width_, height_, seconds, rtv_.Get()};
    const int next = api_->render(instance_, &frame);
    expect(next >= -1, "Old renderer returned failure");
    if (!capture.empty()) {
      context_->CopyResource(readback_.Get(), texture_.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      check(context_->Map(readback_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map reference pixels");
      std::ofstream file(capture, std::ios::binary);
      for (unsigned y = 0; y < height_; ++y)
        file.write(static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch, width_ * 4);
      context_->Unmap(readback_.Get(), 0);
      expect(bool(file), "Save legacy RGBA capture");
    } else {
      context_->Flush();
    }
    return next;
  }

private:
  unsigned width_, height_;
  Module module_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> texture_, readback_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  const A75Wallpaper* api_ = nullptr;
  void* instance_ = nullptr;
};

class Migrated {
public:
  Migrated(const fs::path& dir, const std::string& values, unsigned width, unsigned height)
      : width_(width), height_(height), module_(dir / L"wallpaper.dll") {
    auto entry = reinterpret_cast<A75GetWallpaper12>(module_.get("Action75GetWallpaper12"));
    expect(entry && !module_.get("Action75GetWallpaper") && !entry(1) && (api_ = entry(2)),
           "New DLL exports only the ABI 2 entry");
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
      debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "Find WARP");
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
          "Create DX12 WARP device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    check(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "Create DX12 queue");
    check(device_->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator_)), "Create DX12 allocator");
    check(device_->CreateCommandList(0, queueDesc.Type, allocator_.Get(), nullptr, IID_PPV_ARGS(&commands_)),
          "Create DX12 command list");
    check(commands_->Close(), "Close initial DX12 list");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create DX12 fence");
    auto desc = textureDescription(width, height);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture_)), "Create DX12 target");
    D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    check(device_->CreateDescriptorHeap(&views, IID_PPV_ARGS(&heap_)), "DX12 RTV descriptor heap");
    rtv_ = heap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(texture_.Get(), nullptr, rtv_);

    device_->GetCopyableFootprints(&desc, 0, 1, 0, &layout_, nullptr, nullptr, &bytes_);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes_;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)), "Create DX12 readback");
    const A75Create12 create{sizeof(A75Create12), width, height, device_.Get(),
                             dir.c_str(), values.c_str(), nullptr, nullptr};
    instance_ = api_->create(&create);
    printDiagnostics();
    expect(instance_, "Create migrated wallpaper");
  }

  ~Migrated() {
    if (instance_)
      api_->destroy(instance_);
  }

  void event(const A75Event& event) { api_->on_event(instance_, &event); }

  int render(double seconds, const fs::path& capture) {
    check(allocator_->Reset(), "Reset DX12 allocator");
    check(commands_->Reset(allocator_.Get(), nullptr), "Reset DX12 commands");
    const A75Frame12 frame{sizeof(A75Frame12), width_, height_, seconds, commands_.Get(), texture_.Get(), rtv_};
    bindTarget(frame);
    const int next = api_->render(instance_, &frame);
    printDiagnostics();
    expect(next >= -1, "New renderer returned failure");
    if (!capture.empty()) {
      transition(commands_.Get(), texture_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION source{};
      source.pResource = texture_.Get();
      D3D12_TEXTURE_COPY_LOCATION destination{};
      destination.pResource = readback_.Get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      destination.PlacedFootprint = layout_;
      commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      transition(commands_.Get(), texture_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    check(commands_->Close(), "Close DX12 commands");
    ID3D12CommandList* lists[]{commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    check(queue_->Signal(fence_.Get(), ++sequence_), "Signal readback");
    check(fence_->SetEventOnCompletion(sequence_, nullptr), "Fence before the next ABI call");
    check(device_->GetDeviceRemovedReason(), "Read back the native frame");
    printDiagnostics();
    if (!capture.empty()) {
      void* mapped = nullptr;
      const D3D12_RANGE read{0, size_t(bytes_)}, written{0, 0};
      check(readback_->Map(0, &read, &mapped), "Map native pixels");
      std::ofstream file(capture, std::ios::binary);
      for (unsigned y = 0; y < height_; ++y)
        file.write(static_cast<const char*>(mapped) + size_t(y) * layout_.Footprint.RowPitch, width_ * 4);
      readback_->Unmap(0, &written);
      expect(bool(file), "Save native RGBA capture");
    }
    return next;
  }

private:
  void printDiagnostics() {
    ComPtr<ID3D12InfoQueue> debug;
    if (FAILED(device_.As(&debug)))
      return;
    bool errors = false;
    for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
      SIZE_T bytes = 0;
      if (FAILED(debug->GetMessage(i, nullptr, &bytes)))
        continue;
      std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
      auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
      if (SUCCEEDED(debug->GetMessage(i, message, &bytes)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
        std::cerr << "D3D12 diagnostic " << message->ID << ' ' << message->pDescription << '\n';
        errors = errors || message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
      }
    }
    debug->ClearStoredMessages();
    expect(!errors, "D3D12 debug layer reported an error");
  }

  unsigned width_, height_;
  Module module_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12GraphicsCommandList> commands_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12Resource> texture_, readback_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout_{};
  UINT64 bytes_ = 0, sequence_ = 0;
  const A75Wallpaper12* api_ = nullptr;
  void* instance_ = nullptr;
};

json defaults(const fs::path& directory) {
  json result = json::object();
  const auto schema = readJson(directory / L"settings.json");
  for (const auto& group : schema.at("groups"))
    for (const auto& control : group.at("controls"))
      result[control.at("id").get<std::string>()] = control.at("default");
  return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  int result = 0;
  try {
    expect(argc == 6, "Usage: verify_atlas_dx12 OLD_PACK NEW_PACK CAPTURES WIDTH HEIGHT");
    const auto original = fs::absolute(argv[1]), migrated = fs::absolute(argv[2]), output = fs::absolute(argv[3]);
    const unsigned width = std::stoul(argv[4]), height = std::stoul(argv[5]);
    fs::create_directories(output);
    auto profile = defaults(original);
    auto values = profile.dump();
    const bool paint = readJson(original / L"scene.json").contains("canvasEffect");
    Reference old(original, values, width, height);
    Migrated now(migrated, values, width, height);

    auto capture = [&](double seconds, const std::string& name = {}) {
      const auto file = [&](std::string tag) {
        return name.empty() ? fs::path() : output / (name + "-" + tag + ".rgba");
      };
      const int oldNext = old.render(seconds, file("old"));
      const int newNext = now.render(seconds, file("new"));
      if (!name.empty())
        std::cout << name << ' ' << oldNext << ' ' << newNext << '\n';
      expect(oldNext == newNext, "Old and new return different scheduling codes");
      return newNext;
    };

    auto emit = [&](unsigned type, double seconds, float x = .32f, float y = .58f) {
      const A75Event event{sizeof(A75Event), type, width, height, seconds, x, y, "KeyA", values.c_str()};
      old.event(event);
      now.event(event);
    };

    expect(capture(0, "idle") == -1, "Idle scene suspends until an event");
    if (paint)
      profile["backgroundBrightness"] = 50;
    values = profile.dump();
    emit(A75_SETTINGS, .1);
    expect(capture(.1, "artwork") == -1, "Static backdrop does not schedule frames");
    emit(A75_KEY_DOWN, 1);
    for (int f = 1; f <= 28; ++f) {
      if (f == 11)
        emit(A75_KEY_DOWN, 1 + f / 60., .64f, .36f);
      capture(1 + f / 60., f == 6 ? "hit" : f == 17 ? "overlap" : f == 28 ? "progress" : "");
    }
    emit(A75_PAUSE, 1.49);
    expect(capture(1.49, "paused") == -1, "Paused scene does not schedule frames");
    emit(A75_RESUME, 1.5);
    capture(1.51, "resumed");
    emit(A75_INPUT_RESET, 1.52);
    expect(capture(1.52, "reset") == -1, "Input reset clears the scene");

    profile["backgroundStyle"] = 1;
    profile["backgroundBrightness"] = 75;
    profile["backgroundSaturation"] = 83;
    profile["backgroundHue"] = 37;
    profile["backgroundSolidColor"] = {70, 52, 38};
    profile["palette"] = {{"preset", "custom"}, {"colors", {{240, 76, 120}, {71, 207, 216}, {156, 118, 247}}}};
    profile["saturation"] = 80;
    profile["hue"] = -42;
    profile["brightness"] = 97;
    profile["size"] = 2.4;
    if (profile.contains("arcCount")) {
      profile["arcCount"] = 3;
      profile["arcWidth"] = 1.7;
      profile["arcSweep"] = .7;
      profile["arcEccentricity"] = .72;
    }
    if (paint) {
      profile["density"] = 78;
      profile["centerSize"] = .75;
      profile["speed"] = 1.5;
      profile["pigmentHoldSeconds"] = .75;
      profile["pigmentFadeSeconds"] = .5;
    } else {
      profile["duration"] = .7;
    }
    values = profile.dump();
    emit(A75_SETTINGS, 2);
    capture(2, "styled-idle");
    emit(A75_KEY_DOWN, 2.1, .43f, .50f);
    for (int f = 1; f <= (paint ? 220 : 60); ++f) {
      const auto name = f == 20 ? "styled-hit" : f == 53 ? "styled-decay" : "";
      capture(2.1 + f / 60., name);
    }
    expect(capture(6.5, "expired") == -1, "After the last effect, scene returns to idle");
  } catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    result = 1;
  }
  if (com)
    CoUninitialize();
  return result;
}
