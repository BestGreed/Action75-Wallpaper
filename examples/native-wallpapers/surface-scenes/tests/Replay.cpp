#include "Action75Wallpaper12.h"

#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const auto format = DXGI_FORMAT_R8G8B8A8_UNORM;

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void check(HRESULT result, const char *message) {
  if (FAILED(result))
    throw std::runtime_error(std::string(message) + " (" + std::to_string(result) + ")");
}

void writeImage(const fs::path &path, const void *pixels, unsigned pitch, unsigned width, unsigned height) {
  std::ofstream file(path, std::ios::binary);
  require(!!file, "Open capture for writing");
  file << "P6\n" << width << ' ' << height << "\n255\n";
  std::vector<char> row(width * 3);
  for (unsigned y = 0; y < height; ++y) {
    const auto *source = static_cast<const unsigned char *>(pixels) + y * pitch;
    for (unsigned x = 0; x < width; ++x) {
      if (source[x * 4 + 3] != 255)
        throw std::runtime_error("Wallpaper target alpha must be opaque");
      std::memcpy(row.data() + x * 3, source + x * 4, 3);
    }
    file.write(row.data(), row.size());
  }
  require(!!file, "Write frame capture");
}

struct Output11 {
  using Api = A75Wallpaper;
  static constexpr const char *entry = "Action75GetWallpaper";
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> target, readback;
  ComPtr<ID3D11RenderTargetView> rtv;
  unsigned width = 0, height = 0;
  bool debug = false;

  Output11() {
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                            &device, nullptr, &context), "Create D3D11 WARP device");
  }

  A75Create setup(const fs::path &pack, const std::string &settings, unsigned w, unsigned h) const {
    return {sizeof(A75Create), w, h, device.Get(), context.Get(), pack.c_str(), settings.c_str()};
  }

  void resize(unsigned w, unsigned h) {
    if (w == width && h == height)
      return;
    width = w;
    height = h;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&desc, nullptr, target.ReleaseAndGetAddressOf()), "Create D3D11 target");
    check(device->CreateRenderTargetView(target.Get(), nullptr, rtv.ReleaseAndGetAddressOf()), "Create D3D11 RTV");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, readback.ReleaseAndGetAddressOf()), "Create D3D11 readback");
  }

  int frame(void *instance, const Api &api, unsigned w, unsigned h, double now, const fs::path &capture) {
    context->ClearState();
    resize(w, h);
    const A75Frame frame{sizeof(frame), width, height, now, rtv.Get()};
    const int status = api.render(instance, &frame);
    if (!capture.empty()) {
      context->CopyResource(readback.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map D3D11 readback");
      writeImage(capture, mapped.pData, mapped.RowPitch, width, height);
      context->Unmap(readback.Get(), 0);
    }
    return status;
  }
};

struct Output12 {
  using Api = A75Wallpaper12;
  static constexpr const char *entry = "Action75GetWallpaper12";
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12Resource> target, readback;
  ComPtr<ID3D12DescriptorHeap> heap;
  ComPtr<ID3D12InfoQueue> messages;
  std::unique_ptr<void, decltype(&CloseHandle)> completed{CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0, submission = 0;
  unsigned width = 0, height = 0;
  bool debug = false;

  Output12() {
    ComPtr<ID3D12Debug> layer;
    debug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
    if (debug)
      layer->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create DXGI factory");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Find WARP adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Create D3D12 WARP device");
    if (debug)
      check(device.As(&messages), "Read D3D12 debug layer messages");
    const D3D12_COMMAND_QUEUE_DESC q{};
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Create D3D12 queue");
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "Create D3D12 allocator");
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "Create D3D12 list");
    check(commands->Close(), "Close initial D3D12 list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create D3D12 fence");
    require(!!completed, "Create D3D12 fence event");
    const D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    check(device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&heap)), "Create D3D12 RTV heap");
  }

  A75Create12 setup(const fs::path &pack, const std::string &settings, unsigned w, unsigned h) const {
    return {sizeof(A75Create12), w, h, device.Get(), pack.c_str(), settings.c_str()};
  }

  void resize(unsigned w, unsigned h) {
    if (w == width && h == height)
      return;
    width = w;
    height = h;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(target.ReleaseAndGetAddressOf())),
          "Create D3D12 render target");
    device->CreateRenderTargetView(target.Get(), nullptr, heap->GetCPUDescriptorHandleForHeapStart());
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.ReleaseAndGetAddressOf())),
          "Create D3D12 readback");
  }

  void validate() {
    if (!messages)
      return;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
      SIZE_T length = 0;
      check(messages->GetMessage(i, nullptr, &length), "Read D3D12 debug message size");
      std::vector<unsigned char> storage(length);
      auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
      check(messages->GetMessage(i, message, &length), "Read D3D12 debug message");
      if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        throw std::runtime_error(message->pDescription);
    }
    messages->ClearStoredMessages();
  }

  int frame(void *instance, const Api &api, unsigned w, unsigned h, double now, const fs::path &capture) {
    resize(w, h);
    check(allocator->Reset(), "Reset D3D12 allocator");
    check(commands->Reset(allocator.Get(), nullptr), "Begin D3D12 frame");
    const auto view = heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    commands->OMSetRenderTargets(1, &view, FALSE, nullptr);
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    const A75Frame12 frame{sizeof(frame), width, height, now, commands.Get(), target.Get(), view};
    const int status = api.render(instance, &frame);
    validate();
    if (!capture.empty()) {
      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
      commands->ResourceBarrier(1, &barrier);
      const D3D12_TEXTURE_COPY_LOCATION source{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      destination.PlacedFootprint = footprint;
      commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      commands->ResourceBarrier(1, &barrier);
    }
    check(commands->Close(), "Close D3D12 frame");
    ID3D12CommandList *lists[]{commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++submission), "Signal D3D12 fence");
    check(fence->SetEventOnCompletion(submission, completed.get()), "Wait for D3D12 frame");
    require(WaitForSingleObject(completed.get(), 15000) == WAIT_OBJECT_0, "D3D12 frame timed out");
    validate();
    check(device->GetDeviceRemovedReason(), "D3D12 device state");
    if (!capture.empty()) {
      const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, noWrite{};
      void *mapped = nullptr;
      check(readback->Map(0, &range, &mapped), "Map D3D12 readback");
      writeImage(capture, mapped, footprint.Footprint.RowPitch, width, height);
      readback->Unmap(0, &noWrite);
    }
    return status;
  }
};

template<class Output>
Json replay(unsigned abi, const fs::path &pack, const Json &timeline, const fs::path &directory) {
  Output output;
  using Api = typename Output::Api;
  using Get = const Api *(__cdecl *)(uint32_t);
  using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
  Module module(LoadLibraryW((pack / L"wallpaper.dll").c_str()), FreeLibrary);
  require(!!module, "Load " + pack.string());
  const auto get = reinterpret_cast<Get>(GetProcAddress(module.get(), Output::entry));
  require(get && !get(abi == 1 ? 2 : 1), "Locate ABI export and reject incompatible versions");
  const auto *api = get(abi);
  require(api && api->abi == abi && api->size >= sizeof(Api), "Load ABI table");
  auto width = timeline.value("width", 960u), height = timeline.value("height", 360u);
  const auto settings = timeline.value("settings", Json::object()).dump();
  const auto create = output.setup(pack, settings, width, height);
  std::unique_ptr<void, decltype(api->destroy)> instance(api->create(&create), api->destroy);
  require(!!instance, "Create scene");
  const bool hit = api->hit_test && api->hit_test(instance.get(), .5f, .5f, width, height);
  Json results = Json::array();
  for (const auto &step : timeline.at("frames")) {
    const double now = step.at("at").get<double>();
    width = step.value("width", width);
    height = step.value("height", height);
    for (const auto &e : step.value("events", Json::array())) {
      const auto values = e.value("values", Json::object()).dump();
      const auto code = e.value("code", std::string{});
      const A75Event event{sizeof(event), e.at("type").get<uint32_t>(), width, height, e.value("at", now),
                           e.value("x", .5f), e.value("y", .5f), code.c_str(), values.c_str()};
      api->on_event(instance.get(), &event);
    }
    const auto name = step.value("capture", std::string{});
    const auto capture = name.empty() ? fs::path{} : directory / (name + ".ppm");
    const auto status = output.frame(instance.get(), *api, width, height, now, capture);
    require(status >= -1, "Frame failed at " + std::to_string(now));
    if (step.contains("status"))
      require(status == step.at("status").get<int>(), "Unexpected scheduling at " + std::to_string(now));
    results.push_back(status);
  }
  return {{"abi", abi}, {"debug", output.debug}, {"capturesPointer", hit}, {"scheduling", results}};
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    require(argc == 5, "Usage: surface_replay <11|12> <pack> <timeline.json> <output directory>");
    std::ifstream input{fs::path(argv[3])};
    Json timeline;
    input >> timeline;
    const auto pack = fs::absolute(argv[2]), destination = fs::absolute(argv[4]);
    fs::create_directories(destination);
    const std::wstring_view version = argv[1];
    require(version == L"11" || version == L"12", "Expected D3D11 or D3D12");
    const auto report = version == L"11" ? replay<Output11>(1, pack, timeline, destination)
                                        : replay<Output12>(2, pack, timeline, destination);
    std::ofstream(destination / "replay.json") << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
