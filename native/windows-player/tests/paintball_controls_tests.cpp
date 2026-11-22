#include "EffekseerBackend.h"
#include "LegacyNativeSettings.h"
#include "PackSettings.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<unsigned char, 4>;
constexpr unsigned Width = 640, Height = 360;
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
int wmain(int argc, wchar_t **argv) {
  try {
    require(argc >= 2, "Expected Paintball directory");
    const auto pack = std::filesystem::absolute(argv[1]);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto scratch = std::filesystem::temp_directory_path() /
                         ("Action75-Pigment-" + std::to_string(stamp));
    const action75::test::LegacyNativeSettings legacy(scratch);
    action75::PackSettingsStore settings(scratch);
    legacy.registerPack(settings, "paintball-native", pack);
    const auto defaults = settings.evaluate(settings.current("paintball-native"));
    const auto concentrated =
        settings.evaluate(settings.prepare("paintball-native", {{"density", 100}}));
    const auto brighter =
        settings.evaluate(settings.prepare("paintball-native", {{"brightness", 100}}));
    const auto larger = settings.evaluate(settings.prepare("paintball-native", {{"size", 2}}));
    const auto smallerCenter =
        settings.evaluate(settings.prepare("paintball-native", {{"centerSize", .5}}));
    const auto shortHold =
        settings.evaluate(settings.prepare("paintball-native", {{"pigmentHoldSeconds", 0}}));
    const auto quickFade = settings.evaluate(settings.prepare(
        "paintball-native", {{"pigmentHoldSeconds", 0}, {"pigmentFadeSeconds", .25}}));
    require(defaults.brightness == 1 && defaults.intensity == 1, "Neutral defaults");
    require(concentrated.intensity == 2.5f && concentrated.scale == defaults.scale &&
                concentrated.brightness == defaults.brightness,
            "Concentration binding isolation");
    require(brighter.brightness == 2 && brighter.intensity == defaults.intensity &&
                brighter.scale == defaults.scale,
            "Brightness binding isolation");
    require(larger.scale == 2 && larger.intensity == defaults.intensity &&
                larger.brightness == defaults.brightness,
            "Size binding isolation");
    require(smallerCenter.scale == defaults.scale && shortHold.speed == defaults.speed,
            "Center and hold must stay independent of overall scale and speed");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                        D3D11_SDK_VERSION, &device, nullptr, &context)),
            "WARP device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = Width;
    desc.Height = Height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> view;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)), "Target");
    require(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &view)), "View");
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Readback");
    action75::EffekseerBackend backend;
    require(backend.Initialize(device.Get(), context.Get(), 2048), "Backend");
    backend.Resize(Width, Height);
    constexpr float PlaybackSpeed = .8f;
    constexpr float DeltaTime = PlaybackSpeed / 60.f;
    auto applyUniforms = [&](int id, const action75::PackAppearance &appearance) {
      std::vector<action75::EffekseerBackend::UniformValue> uniforms;
      for (const auto &value : appearance.uniforms)
        uniforms.push_back({value.name, value.component, value.value});
      std::string error;
      if (!backend.SetMaterialUniforms(id, uniforms, error))
        throw std::runtime_error(error);
    };
    auto snapshot = [&](float brightness, float concentration, float scale = 1.f) {
      backend.SetAppearance(scale, brightness, concentration, {255, 255, 255}, PlaybackSpeed);
      backend.Update(0);
      const float clear[4]{.8f, .8f, .8f, 0};
      context->ClearRenderTargetView(view.Get(), clear);
      auto *rtv = view.Get();
      context->OMSetRenderTargets(1, &rtv, nullptr);
      D3D11_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1};
      context->RSSetViewports(1, &viewport);
      require(backend.Draw(), "Draw");
      context->CopyResource(staging.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped{};
      require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Map");
      std::vector<Pixel> pixels(Width * Height);
      for (unsigned y = 0; y < Height; ++y)
        std::copy_n(reinterpret_cast<const Pixel *>(
                        static_cast<const unsigned char *>(mapped.pData) + y * mapped.RowPitch),
                    Width, pixels.data() + y * Width);
      context->Unmap(staging.Get(), 0);
      return pixels;
    };
    auto alphaEqual = [](const auto &a, const auto &b) {
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i][3] != b[i][3])
          return false;
      return true;
    };
    auto opaqueArea = [](const auto &image) {
      return std::count_if(image.begin(), image.end(), [](const Pixel &pixel) { return pixel[3] >= 250; });
    };
    auto alphaMass = [](const auto &image) {
      unsigned sum = 0;
      for (const auto &pixel : image)
        sum += pixel[3];
      return sum;
    };
    auto luma = [](const auto &image, const auto &reference) {
      double sum = 0;
      unsigned count = 0;
      for (size_t i = 0; i < image.size(); ++i)
        if (reference[i][3] > 32) {
          sum += .2126 * image[i][0] + .7152 * image[i][1] + .0722 * image[i][2];
          ++count;
        }
      require(count > 20, "No pigment pixels");
      return sum / count;
    };
    auto save = [&](const auto &pixels, const std::filesystem::path &path) {
      std::ofstream out(path, std::ios::binary);
      out << "P6\n" << Width << " " << Height << "\n255\n";
      for (const auto &p : pixels)
        out.write(reinterpret_cast<const char *>(p.data()), 3);
    };
    for (int variant = 1; variant <= 9; ++variant) {
      char file[40];
      std::snprintf(file, sizeof(file), "paintball-%02d.efk", variant);
      const auto id = backend.LoadEffect(pack / file, true, variant - 1);
      require(id > 0, backend.LastError().c_str());
      applyUniforms(id, defaults);
      require(backend.PlayAt(id, Width * .5f, Height * .5f, 1.f) >= 0, "Play");
      backend.Update(DeltaTime);
      const auto opening = snapshot(1, 1);
      applyUniforms(id, smallerCenter);
      const auto compactOpening = snapshot(1, 1);
      require(opaqueArea(opening) > 20 && opaqueArea(compactOpening) < opaqueArea(opening) * .6,
              "Center size did not shrink the opening ripple");
      applyUniforms(id, shortHold);
      require(opening == snapshot(1, 1), "Retention changed the opening ripple");
      applyUniforms(id, defaults);
      for (int frame = 1; frame < 19; ++frame)
        backend.Update(DeltaTime);
      const auto neutral = snapshot(1, 1), light = snapshot(1, .25f), deep = snapshot(1, 2.5f);
      const auto dim = snapshot(.25f, 1), bright = snapshot(2, 1), large = snapshot(1, 1, 1.5f);
      require(alphaEqual(neutral, light) && alphaEqual(neutral, deep),
              "Concentration changed mask");
      require(alphaEqual(neutral, dim) && alphaEqual(neutral, bright), "Brightness changed mask");
      require(!alphaEqual(neutral, large), "Size did not change geometry");
      const double n = luma(neutral, neutral), l = luma(light, neutral), d = luma(deep, neutral);
      const double lo = luma(dim, neutral), hi = luma(bright, neutral);
      std::printf(
          "variant %d: depth %.1f > %.1f > %.1f; brightness %.1f < %.1f < %.1f; masks identical\n",
          variant, l, n, d, lo, n, hi);
      require(l > n + 2 && n > d + 2, "Concentration did not independently deepen color");
      require(lo < n - 2 && hi > n + 2, "Brightness change too small");
      for (int frame = 19; frame < 66; ++frame)
        backend.Update(DeltaTime);
      const auto settled = snapshot(1, 1);
      applyUniforms(id, smallerCenter);
      require(settled == snapshot(1, 1), "Center size changed the settled pigment");
      applyUniforms(id, shortHold);
      const auto fading = snapshot(1, 1);
      require(alphaMass(fading) > 0 && alphaMass(fading) < alphaMass(settled),
              "Short retention must fade the pigment progressively");
      require(backend.HasVisibleEffects(), "Visible pigment must keep requesting frames");
      applyUniforms(id, quickFade);
      require(alphaMass(snapshot(1, 1)) < alphaMass(fading),
              "Fade duration must change the fade slope at the same hold time");
      for (int frame = 66; frame < 85; ++frame)
        backend.Update(DeltaTime);
      require(alphaMass(snapshot(1, 1)) == 0, "Quick fade must finish first");
      require(backend.HasActiveEffects() && !backend.HasVisibleEffects(),
              "Transparent paint must let the player idle with a live instance");
      applyUniforms(id, shortHold);
      require(backend.HasVisibleEffects() && alphaMass(snapshot(1, 1)) > 0,
              "A longer fade must restore live paint");
      for (int frame = 85; frame < 110; ++frame)
        backend.Update(DeltaTime);
      require(alphaMass(snapshot(1, 1)) == 0, "Default fade did not finish");
      require(!backend.HasVisibleEffects(), "Default fade must also let the player idle");
      applyUniforms(id, defaults);
      require(backend.HasVisibleEffects() && alphaMass(snapshot(1, 1)) > 0,
              "Restoring default retention must reveal the original mark");
      if (argc > 2 && variant == 1) {
        std::filesystem::create_directories(argv[2]);
        const auto out = std::filesystem::path(argv[2]);
        save(light, out / "light-pigment.ppm");
        save(neutral, out / "neutral.ppm");
        save(deep, out / "deep-pigment.ppm");
        save(dim, out / "dim.ppm");
        save(bright, out / "bright.ppm");
        save(large, out / "large.ppm");
      }
      backend.UnloadEffects();
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
