#include "NativeRenderer.h"
#include "PackSettings.h"
#include "WaterSurface.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace action75;
namespace fs = std::filesystem;

namespace {
void require(bool valid, const std::string &message) {
  if (!valid)
    throw std::runtime_error(message);
}

struct ComApartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComApartment() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};

struct Image {
  unsigned width, height;
  std::vector<std::uint8_t> rgba;

  std::size_t changes(const Image &baseline, WaterSurface::Position center, double inner,
                      double outer, int threshold = 3) const {
    const auto shortSide = static_cast<double>(std::min(width, height));
    std::size_t changed = 0;
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        const auto dx = (x + .5 - center[0] * width) / shortSide;
        const auto dy = (y + .5 - center[1] * height) / shortSide * 1.17;
        const auto radiusSquared = dx * dx + dy * dy;
        if (radiusSquared < inner * inner || radiusSquared > outer * outer)
          continue;
        const auto offset = (std::size_t(y) * width + x) * 4;
        const auto difference = std::max({std::abs(rgba[offset] - baseline.rgba[offset]),
                                          std::abs(rgba[offset + 1] - baseline.rgba[offset + 1]),
                                          std::abs(rgba[offset + 2] - baseline.rgba[offset + 2])});
        changed += difference >= threshold;
      }
    return changed;
  }
};

Image capture(NativeRenderer &renderer) {
  auto *context = renderer.context();
  ComPtr<ID3D11RenderTargetView> view;
  context->OMGetRenderTargets(1, &view, nullptr);
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture, staging;
  require(SUCCEEDED(resource.As(&texture)), "Readback requires a texture");
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  Image image{desc.Width, desc.Height,
              std::vector<std::uint8_t>(std::size_t(desc.Width) * desc.Height * 4)};
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Create readback");
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE data{};
  require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data)), "Map water pixels");
  for (unsigned y = 0; y < desc.Height; ++y)
    std::memcpy(image.rgba.data() + std::size_t(y) * desc.Width * 4,
                static_cast<const std::uint8_t *>(data.pData) + std::size_t(y) * data.RowPitch,
                desc.Width * 4);
  context->Unmap(staging.Get(), 0);
  return image;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    const bool impactStudy = (argc == 4 || argc == 5) &&
                             std::wstring_view(argv[3]) == L"--impact-study";
    require(argc == 2 || argc == 3 || impactStudy,
            "Pass pack [preview-dir [--impact-study [profile.json]]]");
    ComApartment com;
    require(SUCCEEDED(com.result), "Initialize WIC");
    const fs::path pack = argv[1];
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    PackSettingsStore settings(fs::temp_directory_path() /
                               ("Action75Stillwater-" + std::to_string(stamp)));
    settings.registerPack("stillwater", pack, pack / "settings.json");
    require(settings.hasSchema("stillwater"), settings.schemaError("stillwater"));
    const auto defaults = settings.evaluate(settings.current("stillwater"));
    const auto still = settings.evaluate(settings.prepare(
        "stillwater", {{"ambient", 0}, {"impactMin", 1}, {"impactMax", 1}}));
    require(still.waterSurface.ambient == 0 && defaults.waterSurface.ambient > 0,
            "Water settings do not reach the renderer");
    require(settings.prepare("stillwater", {{"impactMin", 2}}).values["impactMax"] == 2 &&
                settings.prepare("stillwater", {{"impactMax", .25}}).values["impactMin"] == .25,
            "Editing either end must keep the impact limits ordered");
    const auto reordered = settings.prepare("stillwater", {{"impactMin", 2}, {"impactMax", .25}});
    require(reordered.values["impactMin"] == .25 && reordered.values["impactMax"] == 2,
            "Reversed upper and lower limits were not normalized");

    const bool preview = argc >= 3;
    const unsigned width = preview ? 1920 : 960, height = preview ? 720 : 360;
    std::string error;
    NativeRenderer renderer;
    require(renderer.initialize(nullptr, width, height, true, error, preview), error);
    WaterSurface scene;
    require(scene.load(renderer.device(), renderer.context(), width, height, error), error);
    double now = 10;
    const auto frame = [&](bool paused = false) {
      renderer.beginFrame(true);
      require(scene.draw(renderer.context(), renderer.width(), renderer.height(), now, paused,
                         error), error);
    };
    const auto tick = [&](unsigned frames, double step = 1.0 / 60) {
      for (unsigned i = 0; i < frames; ++i) {
        now += step;
        frame();
      }
    };

    if (impactStudy) {
      const fs::path output = argv[2];
      fs::create_directories(output);
      auto appearance = still;
      if (argc == 5) {
        std::ifstream profileFile{fs::path(argv[4])};
        nlohmann::json profile;
        profileFile >> profile;
        appearance = settings.evaluate(settings.prepare("stillwater", profile.at("values")));
        appearance.waterSurface.ambient = 0;
      }
      scene.configure(appearance);
      frame();
      require(renderer.saveFrame(output / "idle.png", error), error);
      constexpr float forces[]{.25f, .62f, 1.f, 1.45f, 2.4f};
      constexpr unsigned samples[]{1, 4, 10, 22, 44, 77};
      for (unsigned strength = 0; strength < std::size(forces); ++strength) {
        scene.clear();
        appearance.waterSurface.impactMin = appearance.waterSurface.impactMax = forces[strength];
        scene.configure(appearance);
        require(scene.push({.5f, .5f}), "A study tap was rejected");
        unsigned previous = 0;
        for (const auto sample : samples) {
          tick(sample - previous);
          const auto name = "tap-" + std::to_string(strength) + "-" + std::to_string(sample) + ".png";
          require(renderer.saveFrame(output / name, error), error);
          previous = sample;
        }
      }
      return 0;
    }

    if (preview) {
      const fs::path output = argv[2];
      fs::create_directories(output);
      constexpr const char *names[]{"moonlit.png", "lagoon.png", "dusk.png", "obsidian.png",
                                     "custom.png"};
      for (unsigned look = 0; look < std::size(names); ++look) {
        auto appearance = still;
        appearance.waterSurface.backdrop = static_cast<WaterBackdrop>(look);
        if (appearance.waterSurface.backdrop == WaterBackdrop::Custom)
          appearance.backgroundColor = {22, 60, 76};
        scene.clear();
        scene.configure(appearance);
        frame();
        if (look == 0)
          require(renderer.saveFrame(output / "idle.png", error), error);
        const auto press = [&](WaterSurface::Position point, float force) {
          appearance.waterSurface.impactMin = appearance.waterSurface.impactMax = force;
          scene.configure(appearance);
          require(scene.push(point), "A preview tap was rejected");
        };
        press({.44f, .53f}, 1.45f);
        tick(16);
        press({.59f, .45f}, 1.f);
        press({.31f, .35f}, .62f);
        tick(12);
        require(renderer.saveFrame(output / names[look], error), error);
      }
      return 0;
    }

    scene.configure(still);
    frame();
    const auto idle = capture(renderer);
    require(!scene.hasMotion(), "The quiet surface did not sleep");
    require(!scene.push({-0.01f, .5f}) && !scene.push({.5f, 1.01f}) &&
                !scene.push({std::numeric_limits<float>::quiet_NaN(), .5f}),
            "The surface accepted invalid coordinates");
    constexpr WaterSurface::Position touch{.43f, .54f}, remote{.78f, .32f};
    require(scene.push(touch) && scene.hasMotion(), "An idle tap did not wake the surface");
    frame();
    const auto first = capture(renderer);
    require(first.changes(idle, touch, 0, .047) > 12, "The first frame missed the tap center");
    require(first.changes(idle, touch, .082, .14) == 0, "The first frame jumped ahead of the wave");
    tick(15);
    const auto spread = capture(renderer);
    require(spread.changes(idle, touch, .079, .155) > 40, "The wavefront did not propagate");
    require(spread.changes(idle, remote, 0, .06) == 0, "A remote part of the water rippled too early");
    require(scene.push(remote), "A second tap was discarded while the water was moving");
    frame();
    const auto multiple = capture(renderer);
    require(multiple.changes(idle, remote, 0, .047) > 12 &&
                multiple.changes(idle, touch, .079, .155) > 40,
            "Overlapping taps did not retain both wavefronts");

    tick(104, 1.0 / 24);
    require(!scene.hasMotion() && capture(renderer).rgba == idle.rgba,
            "Damped water did not return to its exact resting frame");
    require(scene.push({0, .5f}), "An edge tap must be accepted");
    tick(14);
    const auto edge = capture(renderer);
    require(edge.changes(idle, {0, .5f}, 0, .15) > 12 &&
                edge.changes(idle, {1, .5f}, 0, .14) == 0,
            "Edge waves wrapped onto the other side");
    scene.clear();
    frame();
    require(!scene.hasWaves() && capture(renderer).rgba == idle.rgba, "Clear left residual waves");

    auto solid = still;
    solid.waterSurface.backdrop = WaterBackdrop::Custom;
    solid.waterSurface.texture = solid.waterSurface.caustics = 0;
    solid.backgroundBrightness = 1;
    const auto tap = [&](const PackAppearance &appearance, unsigned frames = 0) {
      scene.clear();
      scene.configure(appearance);
      require(scene.push(touch), "A valid tap was rejected");
      if (frames)
        tick(frames);
      else
        frame();
      return capture(renderer);
    };
    solid.backgroundColor = {0, 0, 0};
    scene.configure(solid);
    frame();
    const auto black = capture(renderer);
    const auto fixed = tap(solid);
    require(fixed.changes(black, touch, 0, .047) > 40 && tap(solid).rgba == fixed.rgba,
            "Equal limits must reproduce the tap, with visible crests on black");

    auto lightTap = solid, heavyTap = solid;
    lightTap.waterSurface.impactMin = lightTap.waterSurface.impactMax = .62f;
    heavyTap.waterSurface.impactMin = heavyTap.waterSurface.impactMax = 1.45f;
    const auto lightFront = tap(lightTap, 4).changes(black, touch, .059, .087, 12);
    const auto heavyFront = tap(heavyTap, 4).changes(black, touch, .059, .087, 12);
    require(heavyFront > 30 && heavyFront > 3 * lightFront,
            "A heavier contact did not produce a distinctly wider initial wave");
    const auto recovery = [&](const PackAppearance &appearance) {
      auto changed = tap(appearance, 9).changes(black, touch, 0, .044, 12);
      for (unsigned sample = 0; sample < 4; ++sample) {
        tick(1);
        changed += capture(renderer).changes(black, touch, 0, .044, 12);
      }
      return changed;
    };
    const auto lightRecovery = recovery(lightTap), heavyRecovery = recovery(heavyTap);
    require(heavyRecovery > 120 && heavyRecovery > 2 * lightRecovery,
            "The heavy rebound did not outlast the light contact (light=" +
                std::to_string(lightRecovery) + ", heavy=" + std::to_string(heavyRecovery) + ")");
    const auto lightTail = tap(lightTap, 44).changes(black, touch, .09, .42, 12);
    const auto heavyTail = tap(heavyTap, 44).changes(black, touch, .09, .42, 12);
    require(heavyTail > 250 && heavyTail > 2 * lightTail,
            "Light and heavy taps produced similar trailing waves");

    auto randomized = solid;
    randomized.waterSurface.impactMin = .3f;
    randomized.waterSurface.impactMax = 1.7f;
    const auto randomFirst = tap(randomized);
    const auto randomNext = tap(randomized);
    require(randomNext.rgba != randomFirst.rgba || tap(randomized).rgba != randomFirst.rgba,
            "Different force limits produced identical taps");
    scene.clear();
    solid.backgroundColor = {255, 255, 255};
    scene.configure(solid);
    frame();
    const auto white = capture(renderer);
    require(tap(solid).changes(white, touch, 0, .047) > 30,
            "Wave troughs must remain visible on white");
    scene.clear();
    solid.waterSurface.impactMin = solid.waterSurface.impactMax = 0;
    scene.configure(solid);
    require(!scene.push(touch), "A zero force range woke the simulation");

    scene.configure(defaults);
    tick(10);
    require(capture(renderer).rgba != idle.rgba, "Ambient water did not flow");
    frame(true);
    const auto frozen = capture(renderer);
    now += 4;
    frame(true);
    require(capture(renderer).rgba == frozen.rgba, "Pause did not freeze the water");

    require(renderer.resize(360, 480, error), error);
    scene.configure(still);
    frame();
    const auto resized = capture(renderer);
    require(scene.push({.68f, .39f}), "Resize dropped a valid tap");
    tick(1);
    require(capture(renderer).changes(resized, {.68f, .39f}, 0, .065) > 12,
            "Pointer mapping drifted after resizing");
    std::cout << "D3D11 water input, force range, contrast, propagation, decay, pause and resize verified\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
