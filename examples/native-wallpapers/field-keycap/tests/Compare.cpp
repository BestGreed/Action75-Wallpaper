#include "Action75Wallpaper12.h"
#include "Background.h"
#include "Gpu12.h"

#include <dxgi1_6.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using wallpaper::Json;
using wallpaper::check;
using wallpaper::ComPtr;
using Pixels = std::vector<unsigned char>;

void require(bool value, const char *description) {
  if (!value)
    throw std::runtime_error(description);
}

struct Pack {
  HMODULE module = nullptr;
  void *instance = nullptr;
  void (__cdecl *destroy)(void *) = nullptr;

  explicit Pack(const fs::path &path) {
    module = LoadLibraryExW((fs::absolute(path) / L"wallpaper.dll").c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module != nullptr, "Load wallpaper DLL");
  }
  Pack(const Pack &) = delete;
  Pack &operator=(const Pack &) = delete;
  ~Pack() {
    if (instance && destroy)
      destroy(instance);
    FreeLibrary(module);
  }
};

struct Size { unsigned width, height; };

Pixels visibleRows(const void *data, std::size_t pitch, Size size) {
  const auto row = size.width * 4;
  Pixels pixels(static_cast<std::size_t>(row) * size.height);
  const auto *source = static_cast<const unsigned char *>(data);
  for (unsigned y = 0; y < size.height; ++y)
    std::memcpy(pixels.data() + y * row, source + y * pitch, row);
  return pixels;
}

struct Render11 {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> commands;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11Texture2D> target, readback;

  explicit Render11(IDXGIAdapter *hardware) {
    check(D3D11CreateDevice(hardware, hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &commands),
          "Create old D3D11 device");
  }

  void resize(Size size) {
    commands->OMSetRenderTargets(0, nullptr, nullptr);
    rtv.Reset();
    target.Reset();
    readback.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width;
    desc.Height = size.height;
    desc.Format = wallpaper::OutputFormat;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&desc, nullptr, &target), "Create old target");
    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create old RTV");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &readback), "Create old readback");
  }

  Pixels draw(const A75Wallpaper &api, Pack &pack, Size size, double now, int delay) {
    A75Frame frame{sizeof(A75Frame), size.width, size.height, now, rtv.Get()};
    require(api.render(pack.instance, &frame) == delay, "Old DLL changed its frame scheduling result");
    commands->OMSetRenderTargets(0, nullptr, nullptr);
    commands->CopyResource(readback.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(commands->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read old pixels");
    auto pixels = visibleRows(mapped.pData, mapped.RowPitch, size);
    commands->Unmap(readback.Get(), 0);
    return pixels;
  }
};

struct Render12 {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  ComPtr<ID3D12Resource> target, readback;
  ComPtr<ID3D12Fence> fence;
  std::unique_ptr<void, decltype(&CloseHandle)> completed{CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 sequence = 0, bytes = 0;

  explicit Render12(IDXGIAdapter *adapter) {
    require(completed != nullptr, "Create readback completion event");
    check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Create D3D12 device");
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create D3D12 queue");
    check(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator)), "Create allocator");
    check(device->CreateCommandList(0, queueDesc.Type, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)), "Create commands");
    check(commands->Close(), "Close initial command list");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create comparison fence");
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)), "Create comparison RTV heap");
  }

  void resize(Size size) {
    target = wallpaper::texture(device.Get(), size.width, size.height, wallpaper::OutputFormat,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    device->CreateRenderTargetView(target.Get(), nullptr, descriptors->GetCPUDescriptorHandleForHeapStart());
    const auto texture = target->GetDesc();
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES memory{};
    memory.Type = D3D12_HEAP_TYPE_READBACK;
    memory.CreationNodeMask = memory.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback.Reset();
    check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "Allocate D3D12 readback");
  }

  Pixels draw(const A75Wallpaper12 &api, Pack &pack, Size size, double now, int delay) {
    check(allocator->Reset(), "Reset comparison allocator");
    check(commands->Reset(allocator.Get(), nullptr), "Reset comparison commands");
    const auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart();
    wallpaper::setTarget(commands.Get(), rtv, size.width, size.height);
    A75Frame12 frame{sizeof(A75Frame12), size.width, size.height, now, commands.Get(), target.Get(), rtv};
    require(api.render(pack.instance, &frame) == delay, "D3D12 DLL changed its frame scheduling result");
    wallpaper::transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    src.pResource = target.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    wallpaper::transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    check(commands->Close(), "Close comparison commands");
    ID3D12CommandList *lists[]{commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++sequence), "Signal comparison fence");
    check(fence->SetEventOnCompletion(sequence, completed.get()), "Watch comparison frame");
    require(WaitForSingleObject(completed.get(), 10000) == WAIT_OBJECT_0, "Comparison frame timed out");
    check(device->GetDeviceRemovedReason(), "Check D3D12 execution");
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    void *mapped = nullptr;
    check(readback->Map(0, &range, &mapped), "Read D3D12 pixels");
    auto pixels = visibleRows(mapped, footprint.Footprint.RowPitch, size);
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return pixels;
  }
};

void save(const fs::path &file, const Pixels &pixels, Size size) {
  std::ofstream out(file, std::ios::binary);
  out << "P6\n" << size.width << ' ' << size.height << "\n255\n";
  std::vector<char> rgb;
  rgb.reserve(static_cast<std::size_t>(size.width) * size.height * 3);
  for (std::size_t i = 0; i < pixels.size(); i += 4)
    for (unsigned c = 0; c < 3; ++c)
      rgb.push_back(static_cast<char>(pixels[i + c]));
  out.write(rgb.data(), rgb.size());
  require(static_cast<bool>(out), "Write visual comparison image");
}

Json defaults(const fs::path &pack) {
  const auto schema = wallpaper::readJson(pack / L"settings.json");
  Json values = Json::object();
  for (const auto &group : schema.at("groups"))
    for (const auto &control : group.at("controls"))
      values[control.at("id").get<std::string>()] = control.at("default");
  return values;
}

void compare(const fs::path &oldPack, const fs::path &newPack, const fs::path &output,
             bool hardware) {
  const auto original = defaults(oldPack);
  require(original == defaults(newPack), "Default parameters changed");
  const bool field = original.contains("palette");
  const auto layout = wallpaper::readJson(oldPack / L"layout.json").at("keys");
  std::vector<std::string> codes;
  std::vector<std::array<float, 2>> positions;
  for (const auto &key : layout) {
    codes.push_back(key.at("code"));
    const auto &center = key.at("centerPx");
    positions.push_back({std::clamp(center.at("x").get<float>() / 1920, 0.f, 1.f),
                         std::clamp(center.at("y").get<float>() / 720, 0.f, 1.f)});
  }

  ComPtr<IDXGIFactory6> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create comparison DXGI factory");
  ComPtr<IDXGIAdapter1> adapter;
  if (hardware)
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&adapter)),
          "Choose integrated adapter");
  else
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Choose WARP adapter");
  DXGI_ADAPTER_DESC1 adapterInfo{};
  check(adapter->GetDesc1(&adapterInfo), "Identify comparison adapter");
  require(!hardware || !(adapterInfo.Flags & DXGI_ADAPTER_FLAG_SOFTWARE), "Expected a hardware adapter");
  std::wcout << L"Adapter " << adapterInfo.Description << L'\n';
  Render11 d3d11(hardware ? adapter.Get() : nullptr);
  Render12 d3d12(adapter.Get());
  Pack old(oldPack), replacement(newPack);
  const auto oldApi = reinterpret_cast<A75GetWallpaper>(GetProcAddress(old.module, "Action75GetWallpaper"));
  const auto newApi = reinterpret_cast<A75GetWallpaper12>(GetProcAddress(replacement.module, "Action75GetWallpaper12"));
  require(oldApi && oldApi(1) && !oldApi(2), "Negotiate old ABI 1");
  require(newApi && newApi(2) && !newApi(1) &&
          !GetProcAddress(replacement.module, "Action75GetWallpaper"), "Negotiate ABI 2 only");
  const auto &api11 = *oldApi(1);
  const auto &api12 = *newApi(2);
  const auto values = original.dump();
  Size size = hardware ? Size{1920, 720} : Size{640, 240};
  A75Create create11{sizeof(A75Create), size.width, size.height, d3d11.device.Get(), d3d11.commands.Get(),
                     oldPack.c_str(), values.c_str(), nullptr, nullptr};
  A75Create12 create12{sizeof(A75Create12), size.width, size.height, d3d12.device.Get(), newPack.c_str(),
                       values.c_str(), nullptr, nullptr};
  old.destroy = api11.destroy;
  replacement.destroy = api12.destroy;
  old.instance = api11.create(&create11);
  replacement.instance = api12.create(&create12);
  require(old.instance && replacement.instance, "Create both author DLLs");

  const auto send = [&](unsigned type, double now, const char *code, std::array<float, 2> position,
                        const char *settings = "{}") {
    A75Event event{sizeof(A75Event), type, size.width, size.height, now, position[0], position[1], code, settings};
    api11.on_event(old.instance, &event);
    api12.on_event(replacement.instance, &event);
  };
  const auto simple = [&](unsigned type, double now) { send(type, now, "", {0, 0}); };
  const auto settings = [&](const Json &config, double now) {
    const auto text = config.dump();
    send(A75_SETTINGS, now, "", {0, 0}, text.c_str());
  };
  const auto hit = [&](std::size_t index, double now) {
    send(A75_KEY_DOWN, now, codes[index].c_str(), positions[index]);
    send(A75_KEY_UP, now + .005, codes[index].c_str(), positions[index]);
  };
  fs::create_directories(output);
  Json results = Json::array();
  const auto capture = [&](const char *name, double now, int delay, bool savePair = false) {
    const auto a = d3d11.draw(api11, old, size, now, delay);
    const auto b = d3d12.draw(api12, replacement, size, now, delay);
    unsigned peak = 0, alpha = 0;
    std::uint64_t total = 0, changed = 0, overFour = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
      unsigned difference = 0;
      for (unsigned c = 0; c < 3; ++c) {
        const auto value = static_cast<unsigned>(std::abs(int(a[i + c]) - int(b[i + c])));
        difference = std::max(difference, value);
        total += value;
      }
      peak = std::max(peak, difference);
      alpha += a[i + 3] != b[i + 3];
      changed += difference != 0;
      overFour += difference > 4;
    }
    const auto mae = double(total) / (size.width * size.height * 3);
    results.push_back({{"case", name}, {"timeS", now}, {"delayMs", delay},
                       {"width", size.width}, {"height", size.height}, {"rgbPeak", peak},
                       {"rgbMae", mae}, {"changedPixels", changed}, {"pixelsOverFour", overFour},
                       {"alphaChanged", alpha}});
    std::cout << results.back().dump() << std::endl;
    // The WARP comparison requires exact RGBA8. Drivers may evaluate shader
    // intrinsics differently. Also bound the size and magnitude of the hardware
    // difference instead of using a single outlier as the image score.
    const bool matches = alpha == 0 && (hardware ? mae <= .15 && overFour * 200 <= size.width * size.height
                                                 : peak == 0);
    if (savePair || !matches) {
      save(output / (std::string(name) + "-dx11.ppm"), a, size);
      save(output / (std::string(name) + "-dx12.ppm"), b, size);
    }
    require(matches, "Appearance exceeded the comparison tolerance");
    return b;
  };

  d3d11.resize(size);
  d3d12.resize(size);
  const auto idle = capture("default-idle", 0, 0, !hardware);
  hit(39, .03);
  require(capture("one-key", .12, 0, true) != idle, "Input must change the image");
  hit(62, .15);
  hit(78, .2);
  capture("three-keys", .25, 0, true);
  for (unsigned i = 0; i < 40; ++i)
    hit((i * 17) % codes.size(), .27 + i * .007);
  capture("burst", .59, 0);
  simple(A75_PAUSE, .60);
  hit(10, .61);
  capture("pause", .70, -1);
  simple(A75_INPUT_RESET, .71);
  capture("input-reset", .72, -1);
  simple(A75_RESUME, .73);

  auto custom = original;
  custom["backgroundHue"] = 48;
  custom["backgroundSaturation"] = .65;
  custom["backgroundBrightness"] = .8;
  if (field) {
    custom["palette"] = Json{{"preset", "custom"}, {"colors", {{8, 18, 75}, {171, 36, 124}, {229, 181, 105}}}};
    custom["bloom"] = 0;
    custom["quality"] = 0;
    custom["speed"] = 1.65;
  } else {
    custom["design"] = 1;
    custom["color"] = {213, 97, 20};
    custom["flowSpeed"] = 0;
    custom["offsetX"] = 14;
    custom["scaleX"] = .91;
  }
  settings(custom, .74);
  capture("custom-before-input", .75, field ? 0 : -1);
  hit(39, .76);
  capture("custom-active", .84, 0, true);
  capture("decay", 3.54, field ? 0 : -1);

  auto solid = custom;
  solid["backgroundStyle"] = 1;
  solid["backgroundColor"] = {26, 59, 94};
  solid["brightness"] = 0;
  simple(A75_INPUT_RESET, 3.6);
  settings(solid, 3.61);
  const auto resting = capture("solid-idle", 3.66, -1);
  require(capture("solid-rest", 3.89, -1) == resting, "Static background must stay identical");

  auto alternate = original;
  if (field) {
    alternate["quality"] = 2;
    alternate["bloom"] = 1.15;
  } else {
    alternate["design"] = 2;
    alternate["offsetY"] = 13;
    alternate["capScale"] = .78;
  }
  settings(alternate, 4.01);
  size = hardware ? Size{1600, 600} : Size{600, 256};
  d3d11.resize(size);
  d3d12.resize(size);
  simple(A75_RESIZE, 4.02);
  hit(78, 4.03);
  capture("resized", 4.16, 0, true);
  std::ofstream report(output / "metrics.json");
  report << results.dump(2) << '\n';
  require(static_cast<bool>(report), "Write comparison metrics");
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "Usage: action75_field_compare old-pack new-pack output [--hardware]\n";
    return 2;
  }
  try {
    compare(fs::absolute(argv[1]), fs::absolute(argv[2]), argv[3], argc == 5);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
