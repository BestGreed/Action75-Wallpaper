#include "Action75Wallpaper12.h"
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
using Pixel = std::array<std::uint8_t, 4>;
using Image = std::vector<Pixel>;

struct Size { UINT width, height; };
struct Readback { int delay; Image pixels; };

void check(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}

void require(HRESULT result, const char *message) {
  if (FAILED(result)) throw std::runtime_error(std::string(message) +
      " (HRESULT " + std::to_string(static_cast<std::uint32_t>(result)) + ")");
}

struct Library {
  HMODULE handle;
  explicit Library(const fs::path &pack) : handle(LoadLibraryW((pack / L"wallpaper.dll").c_str())) {
    check(handle, "Load wallpaper DLL");
  }
  ~Library() { FreeLibrary(handle); }
  template <class T> T entry(const char *name) const {
    return reinterpret_cast<T>(GetProcAddress(handle, name));
  }
};

struct Instance {
  void *object;
  void (__cdecl *destroy)(void *);
  ~Instance() { if (object) destroy(object); }
};

void __cdecl patch(void *host, const char *value) {
  *static_cast<std::string *>(host) = value;
}

Image copyPixels(const void *data, size_t rowPitch, Size size) {
  Image pixels(size_t(size.width) * size.height);
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (UINT y = 0; y < size.height; ++y)
    std::memcpy(pixels.data() + size_t(y) * size.width, bytes + size_t(y) * rowPitch,
                size_t(size.width) * sizeof(Pixel));
  return pixels;
}

struct Renderer11 {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> target, staging;
  ComPtr<ID3D11RenderTargetView> rtv;

  Renderer11(IDXGIAdapter *adapter, bool software) {
    require(D3D11CreateDevice(software ? nullptr : adapter,
        software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
        "Create legacy device");
  }

  void resize(Size size) {
    context->ClearState();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width;
    desc.Height = size.height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    require(device->CreateTexture2D(&desc, nullptr, target.ReleaseAndGetAddressOf()), "Legacy target");
    require(device->CreateRenderTargetView(target.Get(), nullptr, rtv.ReleaseAndGetAddressOf()), "Legacy RTV");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(device->CreateTexture2D(&desc, nullptr, staging.ReleaseAndGetAddressOf()), "Legacy readback");
  }

  Readback frame(const A75Wallpaper &api, void *instance, Size size, double time) {
    const A75Frame f{sizeof(f), size.width, size.height, time, rtv.Get()};
    const int delay = api.render(instance, &f);
    check(delay >= -1, "Legacy render");
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map legacy readback");
    auto pixels = copyPixels(mapped.pData, mapped.RowPitch, size);
    context->Unmap(staging.Get(), 0);
    return {delay, std::move(pixels)};
  }
};

struct Renderer12 {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  ComPtr<ID3D12Resource> target, staging;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT imageLayout{};
  UINT64 bytes = 0, sequence = 0;
  HANDLE completed = nullptr;

  explicit Renderer12(IDXGIAdapter *adapter) {
    require(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Create native device");
    constexpr D3D12_COMMAND_QUEUE_DESC queueDesc{};
    require(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create queue");
    require(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "Create allocator");
    require(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "Create command list");
    require(list->Close(), "Close initial list");
    require(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    constexpr D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    require(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)), "Create RTV heap");
    completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(completed, "Create completion event");
  }
  ~Renderer12() { if (completed) CloseHandle(completed); }

  void resize(Size size) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size.width;
    desc.Height = size.height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(target.ReleaseAndGetAddressOf())), "Native target");
    device->CreateRenderTargetView(target.Get(), nullptr, descriptors->GetCPUDescriptorHandleForHeapStart());
    device->GetCopyableFootprints(&desc, 0, 1, 0, &imageLayout, nullptr, nullptr, &bytes);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(staging.ReleaseAndGetAddressOf())), "Native readback");
  }

  Readback frame(const A75Wallpaper12 &api, void *instance, Size size, double time) {
    require(allocator->Reset(), "Reset allocator");
    require(list->Reset(allocator.Get(), nullptr), "Reset list");
    const D3D12_VIEWPORT viewport{0, 0, float(size.width), float(size.height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(size.width), LONG(size.height)};
    const auto view = descriptors->GetCPUDescriptorHandleForHeapStart();
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &view, FALSE, nullptr);
    const A75Frame12 f{sizeof(f), size.width, size.height, time, list.Get(), target.Get(), view};
    const int delay = api.render(instance, &f);
    check(delay >= -1, "Native render");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = target.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = staging.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = imageLayout;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    require(list->Close(), "Close rendered list");
    ID3D12CommandList *work[] = {list.Get()};
    queue->ExecuteCommandLists(1, work);
    require(queue->Signal(fence.Get(), ++sequence), "Signal readback");
    require(fence->SetEventOnCompletion(sequence, completed), "Arm readback event");
    check(WaitForSingleObject(completed, 10000) == WAIT_OBJECT_0, "Wait for readback");
    require(device->GetDeviceRemovedReason(), "Native device status");
    void *mapped = nullptr;
    const D3D12_RANGE read{0, size_t(bytes)};
    require(staging->Map(0, &read, &mapped), "Map native readback");
    auto pixels = copyPixels(static_cast<unsigned char *>(mapped) + imageLayout.Offset,
                             imageLayout.Footprint.RowPitch, size);
    const D3D12_RANGE noWrite{};
    staging->Unmap(0, &noWrite);
    return {delay, std::move(pixels)};
  }
};

void writePpm(const fs::path &path, const Image &pixels, Size size) {
  std::ofstream out(path, std::ios::binary);
  out << "P6\n" << size.width << ' ' << size.height << "\n255\n";
  for (const auto &pixel : pixels)
    out.write(reinterpret_cast<const char *>(pixel.data()), 3);
  check(bool(out), "Write image evidence");
}

Json difference(const Image &old, const Image &now) {
  std::array<std::uint64_t, 256> hist{};
  std::uint64_t sum = 0, alphaChanges = 0;
  for (size_t i = 0; i < old.size(); ++i) {
    for (size_t c = 0; c < 3; ++c) {
      const auto delta = std::abs(int(old[i][c]) - int(now[i][c]));
      ++hist[delta];
      sum += delta;
    }
    alphaChanges += old[i][3] != now[i][3];
  }
  const std::uint64_t channels = old.size() * 3;
  unsigned p99 = 0, maximum = 0;
  std::uint64_t cumulative = 0, above8 = 0;
  for (unsigned delta = 0; delta < hist.size(); ++delta) {
    if (cumulative * 100 < channels * 99) p99 = delta;
    cumulative += hist[delta];
    if (hist[delta]) maximum = delta;
    if (delta > 8) above8 += hist[delta];
  }
  return {{"rgbMeanAbsolute", double(sum) / channels}, {"rgbP99", p99}, {"rgbMax", maximum},
          {"rgbOver8", double(above8) / channels}, {"alphaChanges", alphaChanges},
          {"rgbIdentical", hist[0] == channels}};
}

void checkDebugMessages(ID3D12Device *device, Json &report) {
  ComPtr<ID3D12InfoQueue> info;
  const bool available = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)));
  report["debugLayer"] = available;
  if (!available) return;
  for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
    SIZE_T bytes = 0;
    require(info->GetMessage(i, nullptr, &bytes), "Read debug message size");
    std::vector<char> storage(bytes);
    auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
    require(info->GetMessage(i, message, &bytes), "Read debug message");
    check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
  }
}

void verify(const fs::path &oldPack, const fs::path &newPack, const fs::path &output,
            bool probe, std::wstring_view deviceType) {
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
  ComPtr<IDXGIFactory6> factory;
  require(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
  ComPtr<IDXGIAdapter1> adapter;
  const bool warp = deviceType == L"warp";
  if (warp) {
    require(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Select WARP adapter");
  } else {
    const auto preference = deviceType == L"low" ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                  : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
    require(factory->EnumAdapterByGpuPreference(0, preference, IID_PPV_ARGS(&adapter)), "Select hardware adapter");
  }
  DXGI_ADAPTER_DESC1 adapterInfo{};
  require(adapter->GetDesc1(&adapterInfo), "Identify adapter");
  Renderer11 legacy(adapter.Get(), warp);
  Renderer12 native(adapter.Get());
  Library old(oldPack), now(newPack);
  const auto entry11 = old.entry<A75GetWallpaper>("Action75GetWallpaper");
  const auto entry12 = now.entry<A75GetWallpaper12>("Action75GetWallpaper12");
  check(entry11 && entry12 && !entry12(1) && !now.entry<FARPROC>("Action75GetWallpaper"), "ABI isolation");
  const auto *v1 = entry11(1);
  const auto *v2 = entry12(2);
  check(v1 && v1->abi == 1 && v1->size == sizeof(*v1) && v2 && v2->abi == 2 && v2->size == sizeof(*v2),
        "ABI negotiation");

  fs::create_directories(output);
  std::wstring adapterName(adapterInfo.Description);
  const int nameBytes = WideCharToMultiByte(CP_UTF8, 0, adapterName.data(), int(adapterName.size()),
                                            nullptr, 0, nullptr, nullptr);
  std::string adapterUtf8(nameBytes, '\0');
  WideCharToMultiByte(CP_UTF8, 0, adapterName.data(), int(adapterName.size()),
                      adapterUtf8.data(), nameBytes, nullptr, nullptr);
  Json report{{"adapter", adapterUtf8}, {"frames", Json::array()}, {"pixelFailures", Json::array()}};
  Size size{640, 240};
  legacy.resize(size);
  native.resize(size);
  std::string oldPatch, newPatch;
  const A75Create c1{sizeof(c1), size.width, size.height, legacy.device.Get(), legacy.context.Get(),
    oldPack.c_str(), "{}", &oldPatch, patch};
  const A75Create12 c2{sizeof(c2), size.width, size.height, native.device.Get(),
    newPack.c_str(), "{}", &newPatch, patch};
  Instance previous{v1->create(&c1), v1->destroy}, current{v2->create(&c2), v2->destroy};
  check(previous.object && current.object, "Create both packages");

  const auto event = [&](UINT type, double time, float x = 0, float y = 0, const char *settings = nullptr) {
    const A75Event e{sizeof(e), type, size.width, size.height, time, x, y, "KeyQ", settings};
    v1->on_event(previous.object, &e);
    v2->on_event(current.object, &e);
  };
  const auto frame = [&](const char *name, double time, int expected, bool images = false) {
    auto a = legacy.frame(*v1, previous.object, size, time);
    auto b = native.frame(*v2, current.object, size, time);
    auto stats = difference(a.pixels, b.pixels);
    stats["name"] = name;
    stats["size"] = {size.width, size.height};
    stats["time"] = time;
    stats["delay"] = b.delay;
    report["frames"].push_back(stats);
    if (images) {
      writePpm(output / (std::string(name) + "-11.ppm"), a.pixels, size);
      writePpm(output / (std::string(name) + "-12.ppm"), b.pixels, size);
    }
    std::cout << stats.dump() << '\n';
    check(a.delay == b.delay && (expected == 1 ? b.delay > 0 : b.delay == expected), "Scheduling parity");
    const bool close = probe ? stats["rgbIdentical"].get<bool>() :
        stats["rgbMeanAbsolute"].get<double>() <= .30 && stats["rgbP99"].get<int>() <= 3 &&
        stats["rgbOver8"].get<double>() <= .002;
    if (stats["alphaChanges"] != 0 || !close)
      report["pixelFailures"].push_back(name);
    return b.pixels;
  };

  if (probe) {
    const auto idle = frame("idle", 0, -1, true);
    event(A75_KEY_DOWN, 1, .5f, .5f);
    const auto lit = frame("center", 1.1, 1, true);
    const auto litCount = [&](const Image &image) {
      return std::count_if(image.begin(), image.end(), [&](auto pixel) { return pixel != idle[0]; });
    };
    check(litCount(lit) == 88 * 88 && lit[120 * 640 + 320] != idle[0], "Centered 88px flash");
    event(A75_KEY_DOWN, 1.3, .0f, 1.f);
    const auto clipped = frame("clipped-retrigger", 1.55, 1, true);
    check(litCount(clipped) == 44 * 44 && clipped[120 * 640 + 320] == idle[0] &&
          clipped[239 * 640] != idle[0], "Clipped lower-left flash");
    check(frame("expired", 1.7501, -1) == idle, "Probe expiry and sleep");
  } else {
    const auto hit = [&](float x, float y, bool expected) {
      check(bool(v1->hit_test(previous.object, x, y, size.width, size.height)) == expected &&
            bool(v2->hit_test(current.object, x, y, size.width, size.height)) == expected, "Shadow hit test");
    };
    hit(.58f, .46f, true);
    hit(.10f, .10f, false);
    const auto nebula = frame("nebula", 1, 0, true);
    event(A75_SETTINGS, 1, 0, 0, R"({"skyMode":1})");
    const auto dark = frame("void", 1, 0, true);
    event(A75_SETTINGS, 1, 0, 0, R"({"skyMode":2})");
    const auto stars = frame("starfield", 1, 0, true);
    event(A75_SETTINGS, 1, 0, 0, R"({"skyMode":3,"backgroundColor":[50,115,187],"backgroundBrightness":1.2})");
    const auto solid = frame("solid-tinted", 1, 0, true);
    check(nebula != dark && nebula != stars && nebula != solid && dark != stars && dark != solid && stars != solid,
          "Distinct four sky modes");
    event(A75_KEY_DOWN, 1, .58f, .46f);
    check(frame("shadow-key", 1, 0) == solid, "Under-shadow key stays quiet");
    event(A75_SETTINGS, 1, 0, 0, R"({"skyMode":2,"starBrightness":1.8,"starSize":1.2,"starHue":27,"lensStrength":1.25,"diskPalette":2})");
    const auto unlit = frame("before-key", 1, 0);
    event(A75_KEY_DOWN, 1, .47f, .48f);
    check(frame("first-key-lens", 1.12, 0, true) != unlit, "Outside key emits through the lens");
    event(A75_KEY_DOWN, 1.13, .47f, .48f);
    event(A75_KEY_DOWN, 1.16, .47f, .48f);
    event(A75_KEY_DOWN, 1.16, .17f, .28f);
    frame("repeated-key-heat", 1.27, 0, true);
    event(A75_POINTER_DOWN, 1.30, .1f, .1f);
    event(A75_POINTER_UP, 1.30, .42f, .66f);
    check(oldPatch.empty() && newPatch.empty(), "Outside drag does not commit");
    event(A75_POINTER_DOWN, 1.31, .58f, .46f);
    event(A75_POINTER_MOVE, 1.32, .42f, .66f);
    frame("dragging", 1.33, 0, true);
    event(A75_POINTER_UP, 1.34, .42f, .66f);
    const auto committed = Json::parse(newPatch);
    check(committed == Json::parse(oldPatch) && committed == Json{{"centerX", 42.0}, {"centerY", 66.0}},
          "Persist quantized center from drag");
    hit(.42f, .66f, true);
    hit(.58f, .46f, false);
    event(A75_SETTINGS, 1.35, 0, 0, newPatch.c_str());
    event(A75_POINTER_DOWN, 1.36, .42f, .66f);
    event(A75_POINTER_MOVE, 1.37, .21f, .40f);
    event(A75_INPUT_RESET, 1.38);
    hit(.42f, .66f, true);
    hit(.21f, .40f, false);
    event(A75_PAUSE, 1.4);
    hit(.42f, .66f, false);
    frame("paused", 1.41, -1);
    event(A75_RESUME, 1.6);
    frame("resumed", 1.60, 0);
    size = {853, 287};
    legacy.resize(size);
    native.resize(size);
    event(A75_RESIZE, 1.61);
    frame("odd-size", 1.62, 0, true);
    size = {1920, 720};
    legacy.resize(size);
    native.resize(size);
    event(A75_RESIZE, 1.66);
    event(A75_KEY_DOWN, 1.67, .51f, .69f);
    frame("keyboard-output", 1.81, 0, true);
    report["dragCommit"] = committed;
  }
  checkDebugMessages(native.device.Get(), report);
  report["passed"] = report["pixelFailures"].empty();
  std::ofstream(output / "parity.json", std::ios::binary) << report.dump(2) << '\n';
  check(report["passed"].get<bool>(), "Pixel parity: " + report["pixelFailures"].dump());
  std::cout << "Paired playback verified on " << adapterUtf8 << '\n';
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 6) {
    std::cerr << "Usage: verify_native_parity <black-hole|probe> <warp|high|low> <ABI1 pack> <ABI2 pack> <output>\n";
    return 2;
  }
  const std::wstring_view scene = argv[1], device = argv[2];
  if ((scene != L"black-hole" && scene != L"probe") ||
      (device != L"warp" && device != L"high" && device != L"low")) return 2;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
  int code = 0;
  try {
    verify(fs::absolute(argv[3]), fs::absolute(argv[4]), fs::absolute(argv[5]), scene == L"probe", device);
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    code = 1;
  }
  CoUninitialize();
  return code;
}
