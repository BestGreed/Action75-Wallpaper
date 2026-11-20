#include "VideoBackground.h"

#include <d3d11.h>
#include <dxgi.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

namespace {

struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Apartment() {
    if (FAILED(result)) throw std::runtime_error("Cannot initialize COM apartment");
  }
  ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

std::string utf8(const wchar_t *text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string value(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), size, nullptr, nullptr);
  value.resize(static_cast<size_t>(size - 1));
  return value;
}

std::uint64_t checksum(ID3D11Device *device, ID3D11DeviceContext *context,
                       ID3D11ShaderResourceView *view) {
  require(view != nullptr, "No transferred video texture");
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture;
  require(SUCCEEDED(resource.As(&texture)), "Video view is not a 2D texture");
  D3D11_TEXTURE2D_DESC description{};
  texture->GetDesc(&description);
  require(description.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
          "Video output did not use BGRA");
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.MiscFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;
  require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &readback)),
          "Cannot create video readback texture");
  context->CopyResource(readback.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  require(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
          "Cannot read back video frame");
  std::uint64_t hash = 14695981039346656037ULL;
  const auto *bytes = static_cast<const std::uint8_t *>(mapped.pData);
  for (unsigned row = 0; row < description.Height; ++row) {
    const auto *pixels = bytes + static_cast<size_t>(row) * mapped.RowPitch;
    for (unsigned offset = 0; offset < description.Width * 4; ++offset) {
      hash ^= pixels[offset];
      hash *= 1099511628211ULL;
    }
  }
  context->Unmap(readback.Get(), 0);
  return hash;
}

json probe(const std::filesystem::path &file) {
  Apartment apartment;
  const auto started = Clock::now();
  const auto deadline = started + std::chrono::seconds(5);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  const HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
      nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
  require(SUCCEEDED(created), "Cannot create hardware D3D11 video device");
  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC description{};
  require(SUCCEEDED(device.As(&dxgiDevice)) &&
              SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->GetDesc(&description)), "Cannot inspect D3D11 adapter");

  action75::VideoBackground video;
  require(video.Initialize(device.Get(), context.Get()), video.LastError());
  // Keep diagnostics small and offscreen; no HWND, swap chain, audio output or
  // desktop capture is requested by this program.
  require(video.Resize(320, 120), video.LastError());
  require(video.Open(file), video.LastError());
  unsigned transferred = 0;
  std::uint64_t firstHash = 0, currentHash = 0;
  bool firstFrame = false, distinctFrame = false;
  while (Clock::now() < deadline && !distinctFrame) {
    if (video.UpdateFrame()) {
      currentHash = checksum(device.Get(), context.Get(), video.TextureView());
      ++transferred;
      if (!firstFrame) { firstHash = currentHash; firstFrame = true; }
      else distinctFrame = currentHash != firstHash;
    }
    require(video.LastError().empty(), video.LastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(firstFrame, "No video frame within the 5-second diagnostic budget");
  require(distinctFrame, "Video did not produce distinct frame checksums");
  const auto pausedHash = currentHash;
  video.SetPaused(true);
  require(!video.IsAnimated(), "Video did not enter the paused state");
  for (int attempt = 0; attempt < 10; ++attempt) {
    require(!video.UpdateFrame(), "Paused video unexpectedly transferred a frame");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(checksum(device.Get(), context.Get(), video.TextureView()) == pausedHash,
          "Paused video output changed");
  video.SetPaused(false);
  require(video.IsAnimated(), "Video did not resume");
  bool resumedFrame = false;
  while (Clock::now() < deadline && !resumedFrame) {
    if (video.UpdateFrame()) {
      currentHash = checksum(device.Get(), context.Get(), video.TextureView());
      ++transferred;
      resumedFrame = currentHash != pausedHash;
    }
    require(video.LastError().empty(), video.LastError());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(resumedFrame, "Resumed video did not advance before the diagnostic deadline");
  video.Close();
  require(!video.IsAnimated() && !video.TextureView(), "Video did not close cleanly");
  return {{"ok", true}, {"test", "offscreen-native-video"},
          {"firstFrame", firstFrame}, {"distinctFrames", distinctFrame},
          {"pausePassed", true}, {"resumePassed", resumedFrame}, {"closePassed", true},
          {"transferredFrames", transferred}, {"firstChecksum", firstHash},
          {"lastChecksum", currentHash}, {"width", 320}, {"height", 120},
          {"adapter", utf8(description.Description)}, {"applicationWindowsCreated", 0},
          {"audioForcedMute", true}, {"hardwareDecodeVerified", false},
          {"decodePath", "Media Foundation Media Engine with DXGI device manager"},
          {"elapsedMs", std::chrono::duration<double, std::milli>(Clock::now() - started).count()}};
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    if (argc != 2) throw std::runtime_error("Usage: Action75VideoProbe.exe <local-video-file>");
    std::cout << probe(std::filesystem::path(argv[1])).dump() << '\n';
    return 0;
  } catch (const std::exception &exception) {
    std::cout << json({{"ok", false}, {"test", "offscreen-native-video"},
                      {"error", exception.what()}, {"applicationWindowsCreated", 0},
                      {"hardwareDecodeVerified", false}}).dump() << '\n';
    return 1;
  }
}
