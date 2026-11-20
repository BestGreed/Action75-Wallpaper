#include "BlackHoleScene.h"
#include "BlackHoleProfile.h"
#include "NativeRenderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace action75;
namespace fs = std::filesystem;

namespace {
struct ComApartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComApartment() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};

void require(bool valid, const std::string &message) {
  if (!valid)
    throw std::runtime_error(message);
}

struct Image {
  unsigned width;
  std::vector<std::uint8_t> rgba;
  unsigned brightness(BlackHoleScene::Position point) const {
    const auto height = rgba.size() / (4 * width);
    const auto x = static_cast<unsigned>(point[0] * width);
    const auto y = static_cast<unsigned>(point[1] * height);
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return std::max({rgba[offset], rgba[offset + 1], rgba[offset + 2]});
  }
};

Image capture(NativeRenderer &renderer) {
  auto *context = renderer.context();
  ComPtr<ID3D11RenderTargetView> view;
  context->OMGetRenderTargets(1, &view, nullptr);
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> texture, staging;
  require(SUCCEEDED(resource.As(&texture)), "Find scene texture");
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  Image image{desc.Width, std::vector<std::uint8_t>(std::size_t(desc.Width) * desc.Height * 4)};
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Create readback");
  context->CopyResource(staging.Get(), texture.Get());
  D3D11_MAPPED_SUBRESOURCE data{};
  require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data)), "Map scene pixels");
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
    require(argc == 2 || argc == 3,
            "Pass the black-hole pack and optionally an output folder, --motion, or --sky-motion");
    ComApartment com;
    require(SUCCEEDED(com.result), "Initialize WIC");
    const fs::path pack = argv[1];
    nlohmann::json schema, defaults = nlohmann::json::object();
    std::ifstream(pack / "settings.json") >> schema;
    for (const auto &group : schema.at("groups"))
      for (const auto &control : group.at("controls"))
        defaults[control.at("id").get<std::string>()] = control.at("default");
    const auto profile = [&](const nlohmann::json &patch) {
      auto values = defaults;
      values.update(patch);
      return BlackHoleProfile::fromSettings(values);
    };
    const auto still = [&](nlohmann::json patch = nlohmann::json::object()) {
      patch["nebulaSpeed"] = 0;
      return profile(patch);
    };
    const auto appearance = still();

    const bool motion = argc == 3 && std::wstring_view(argv[2]) == L"--motion";
    const bool skyMotion = argc == 3 && std::wstring_view(argv[2]) == L"--sky-motion";
    const bool video = motion || skyMotion;
    const bool preview = argc == 3 && !video;
    const unsigned width = video ? 1280 : (preview ? 1920 : 960);
    const unsigned height = video ? 480 : (preview ? 720 : 360);
    NativeRenderer renderer;
    std::string error;
    require(renderer.initialize(nullptr, width, height, true, error, preview || video), error);
    require(renderer.loadBackground(pack / "nebula.png", error), error);
    BlackHoleScene scene;
    const auto configure = [&](const BlackHoleProfile &settings) {
      scene.configure(settings.blackHole, settings.starSize, settings.starBrightness);
    };
    require(scene.load(renderer.device(), renderer.context(), renderer.background(), error), error);
    renderer.clearBackground();
    configure(appearance);
    const auto frame = [&](double now, bool paused = false) {
      renderer.beginFrame(true);
      require(scene.draw(renderer.context(), width, height, now, paused, error), error);
    };
    const auto writeVideoFrame = [&] {
      const auto pixels = capture(renderer);
      require(fwrite(pixels.rgba.data(), 1, pixels.rgba.size(), stdout) == pixels.rgba.size(),
              "Write raw preview frame");
    };
    if (video)
      require(_setmode(_fileno(stdout), _O_BINARY) != -1, "Open raw video output");
    if (skyMotion) {
      for (int mode = 0; mode < 3; ++mode) {
        configure(profile({{"skyMode", mode},
            {"centerX", 61}, {"centerY", 47}, {"holeSize", 1.35}, {"diskBrightness", .88}}));
        for (int tick = 0; tick < 180; ++tick) {
          frame(20 + (mode * 180 + tick) / 60.);
          writeVideoFrame();
        }
      }
      return 0;
    }
    if (motion) {
      // Sample at 150 Hz for a 1280x480, 0.4x slow-motion stream encoded at 60 Hz.
      const auto study = still({{"centerX", 61}, {"centerY", 50},
          {"holeSize", 1.35}, {"diskBrightness", .80}, {"lensStrength", 1.1},
          {"starSize", 1.65}, {"starBrightness", 1.55}, {"starFlare", 1.25}});
      configure(study);
      struct Press { int frame; BlackHoleScene::Position point; };
      constexpr Press keys[] = {{30, {.5145f, .5825f}}, {165, {.6915f, .4131f}},
                                {240, {.2531f, .5867f}}, {275, {.5145f, .5825f}}};
      for (int tick = 0; tick < 400; ++tick) {
        const double now = 20 + tick / 150.;
        for (const auto &keyframe : keys)
          if (keyframe.frame == tick)
            scene.push(now, keyframe.point, width, height);
        frame(now);
        writeVideoFrame();
      }
      return 0;
    }
    const auto center = scene.center();
    const BlackHoleScene::Position key{.20f, .60f};
    frame(10);
    const auto baseline = capture(renderer);
    require(baseline.brightness(center) < 4, "The silhouette center must stay black");
    require(!scene.push(10, center, width, height), "The shadow must suppress input");
    frame(10);
    require(scene.count() == 0 && capture(renderer).rgba == baseline.rgba,
            "A suppressed input modified the frame");
    require(scene.push(10, key, width, height), "The outside key must light up");
    frame(10);
    require(capture(renderer).rgba == baseline.rgba, "A new pulse began with a visible jump");
    frame(10 + 1.0 / 60);
    const auto firstTick = capture(renderer).brightness(key);
    frame(10.18);
    const auto first = capture(renderer);
    require(firstTick > baseline.brightness(key) + 3 && firstTick + 50 < first.brightness(key),
            "The first frame needs a visible lead-in to the peak");
    require(scene.push(10.18, key, width, height), "An outside key can light up again");
    frame(10.18);
    const auto repeated = capture(renderer);
    require(std::equal(first.rgba.begin(), first.rgba.end(), repeated.rgba.begin(),
                       [](int before, int after) { return std::abs(before - after) <= 2; }),
            "A second impulse reset the visible light or color");
    frame(10.31);
    const auto lit = capture(renderer);
    require(scene.count() == 1, "Repeated input accumulated multiple stars at the same key");
    require(lit.brightness(key) > baseline.brightness(key) + 100,
            "The star did not light up at the calibrated point");
    require(first.rgba != lit.rgba, "Successive presses should vary the star's appearance");
    const auto muted = still({{"starFlare", 0}});
    configure(muted);
    frame(10.31);
    const auto noRays = capture(renderer);
    require(lit.rgba != noRays.rgba && noRays.brightness(key) > baseline.brightness(key) + 100,
            "Flare strength should control the rays while preserving the star at the key");
    configure(appearance);
    frame(13);
    require(scene.count() == 0, "Expired stars stayed in the scene");

    auto unlit = appearance;
    unlit.blackHole.diskBrightness = 0;
    configure(unlit);
    frame(13, true);
    const auto lensed = capture(renderer);
    unlit.blackHole.lensStrength = 0;
    configure(unlit);
    frame(13, true);
    const auto straight = capture(renderer);
    std::size_t absoluteChange = 0;
    for (std::size_t i = 0; i < lensed.rgba.size(); ++i)
      absoluteChange += std::abs(static_cast<int>(lensed.rgba[i]) - straight.rgba[i]);
    require(absoluteChange > 12000, "Lensing did not move the static stars and dust");
    require(lensed.brightness({.03f, .05f}) == straight.brightness({.03f, .05f}),
            "The lens changed a distant pixel");

    const auto moved = still({{"centerX", 20}, {"centerY", 60}});
    configure(moved);
    require(!scene.push(14, key, width, height) && scene.push(14, center, width, height),
            "The input mask did not move with the black hole");
    frame(14.18);
    const auto relocated = capture(renderer);
    require(relocated.brightness(key) < 4 && relocated.brightness(center) > 130,
            "The rendered shadow did not follow the input mask");

    scene.clear();
    configure(appearance);
    const BlackHoleScene::Position justAbove{center[0], center[1] - .197f};
    require(scene.push(16, justAbove, width, height), "The key should start outside the shadow");
    frame(16.18);
    const auto compactFrame = capture(renderer);
    const auto enlarged = still({{"holeSize", 1.65}});
    configure(enlarged);
    require(!scene.push(16.18, justAbove, width, height), "The input mask did not enlarge");
    frame(16.18);
    require(compactFrame.brightness(justAbove) > 130 && capture(renderer).brightness(justAbove) < 5,
            "The rendered shadow or star cutoff did not enlarge");

    scene.clear();
    const auto edge = still({{"holeSize", 3.5}, {"centerX", 100}, {"centerY", 49}});
    configure(edge);
    const BlackHoleScene::Position atEdge{.985f, .49f};
    const BlackHoleScene::Position outsideEdgeShadow{.74f, .49f};
    frame(17);
    require(scene.center()[0] == 1.f && scene.covers(atEdge, width, height) &&
                !scene.covers(outsideEdgeShadow, width, height), "The drag zone did not reach the screen edge");
    require(capture(renderer).brightness(atEdge) < 4 && !scene.push(17, atEdge, width, height) &&
                scene.push(17, outsideEdgeShadow, width, height), "The cropped shadow and key mask must agree");

    scene.clear();
    configure(appearance);
    constexpr BlackHoleScene::Position diskKey{.5145f, .5825f};
    frame(18.18);
    const auto bareDisk = capture(renderer);
    require(scene.push(18, diskKey, width, height), "A key over the outer disk must still light");
    frame(18.18);
    require(capture(renderer).brightness(diskKey) > bareDisk.brightness(diskKey) + 8,
            "The flowing star still needs a visible, anchored core");

    // Isolate the ray paths from the gas disk. At b = 1.25 shadow radii and
    // thetaE^2 = 2.92, both solutions lie outside the opaque shadow.
    scene.clear();
    auto optics = appearance;
    optics.blackHole.diskBrightness = 0;
    optics.blackHole.center = {.56f, .48f};
    optics.blackHole.size = 1.35f;
    const auto opticalCenter = optics.blackHole.center;
    const float shadow = .149f * optics.blackHole.size * static_cast<float>(height);
    const float unit = shadow / static_cast<float>(width);
    constexpr float b = 1.25f, mass = 2.92f;
    const float root = std::sqrt(b * b + 4 * mass);
    const BlackHoleScene::Position source{opticalCenter[0] - b * unit, opticalCenter[1]};
    const BlackHoleScene::Position primary{opticalCenter[0] - .5f * (b + root) * unit, opticalCenter[1]};
    const BlackHoleScene::Position secondary{opticalCenter[0] + .5f * (root - b) * unit, opticalCenter[1]};
    configure(optics);
    frame(19);
    const auto emptyLens = capture(renderer);
    require(scene.push(19, source, width, height), "The lens source must be outside the shadow");
    frame(19.049);
    const auto firstPath = capture(renderer);
    require(firstPath.brightness(primary) > emptyLens.brightness(primary) + 15 &&
                firstPath.brightness(secondary) == emptyLens.brightness(secondary),
            "The short path should arrive before its counterpart across the shadow");
    frame(19.24);
    const auto bothPaths = capture(renderer);
    require(bothPaths.brightness(primary) > emptyLens.brightness(primary) + 50 &&
                bothPaths.brightness(secondary) > emptyLens.brightness(secondary) + 50,
            "Both images should appear where the point-lens equation predicts");
    require(bothPaths.brightness(opticalCenter) < 4, "Traced light must respect the opaque shadow");
    optics.blackHole.lensStrength = 0;
    configure(optics);
    frame(19.24);
    const auto noLens = capture(renderer);
    require(noLens.brightness(source) > noLens.brightness(primary) + 60 &&
                noLens.brightness(secondary) + 35 < bothPaths.brightness(secondary),
            "Disabling the lens must leave the keyed light at the calibrated point");

    scene.clear();
    optics.blackHole.starFlare = 0;
    configure(optics);
    frame(20);
    const auto skyOnly = capture(renderer);
    std::size_t smallest = width * height, largest = 0;
    for (int trial = 0; trial < 12; ++trial) {
      scene.clear();
      require(scene.push(20 + trial, key, width, height), "The size sample must be outside the disk");
      frame(20.2 + trial);
      const auto sample = capture(renderer);
      std::size_t area = 0;
      for (std::size_t i = 0; i < sample.rgba.size(); i += 4) {
        const int gain = std::max({sample.rgba[i] - skyOnly.rgba[i],
                                   sample.rgba[i + 1] - skyOnly.rgba[i + 1],
                                   sample.rgba[i + 2] - skyOnly.rgba[i + 2]});
        area += gain > 35;
      }
      smallest = std::min(smallest, area);
      largest = std::max(largest, area);
    }
    require(smallest > 0 && largest > 4 * smallest,
            "At equal ages, repeated new stars must show distinctly different bright footprints");

    scene.clear();
    std::array<std::uint64_t, 3> diskBlue{};
    for (unsigned palette = 0; palette < diskBlue.size(); ++palette) {
      configure(still({{"diskPalette", palette}}));
      frame(32, true);
      const auto sample = capture(renderer);
      require(sample.brightness(center) < 4, "A disk palette must leave the shadow black");
      require(sample.brightness({.03f, .05f}) == baseline.brightness({.03f, .05f}),
              "A disk palette must preserve the distant sky");
      for (std::size_t i = 2; i < sample.rgba.size(); i += 4)
        diskBlue[palette] += sample.rgba[i];
    }
    require(diskBlue[1] > diskBlue[0] + 25000 && diskBlue[0] > diskBlue[2] + 25000,
            "Disk palettes must visibly separate warm, blue-white and radio colors");

    if (preview) {
      const fs::path folder = argv[2];
      fs::create_directories(folder);
      scene.clear();
      configure(appearance);
      frame(34);
      const bool idle = renderer.saveFrame(folder / "idle.png", error);
      for (const BlackHoleScene::Position position : {BlackHoleScene::Position{.14f, .41f},
              {.299f, .574f}, {.419f, .409f}, {.55f, .745f}, {.762f, .24f}, {.86f, .568f}, {.919f, .77f}})
        scene.push(34, position, width, height);
      frame(34.13);
      const bool active = renderer.saveFrame(folder / "interactive.png", error);
      scene.clear();
      const auto half = profile({{"holeSize", 2.65}, {"centerX", 100}, {"centerY", 49}});
      configure(half);
      for (const BlackHoleScene::Position position : {BlackHoleScene::Position{.1648f, .4167f},
              {.284f, .573f}, {.432f, .249f}, {.634f, .415f}, {.7737f, .5853f}, {.7385f, .7547f}})
        scene.push(35, position, width, height);
      frame(35.17);
      const bool cropped = renderer.saveFrame(folder / "half-horizon.png", error);
      scene.clear();
      auto rays = profile({{"centerX", 61}, {"centerY", 50},
          {"holeSize", 1.35}, {"diskBrightness", .80}, {"lensStrength", 1.1},
          {"starSize", 1.65}, {"starBrightness", 1.55}});
      configure(rays);
      scene.push(36, diskKey, width, height);
      frame(36.24);
      const bool traced = renderer.saveFrame(folder / "traced-light.png", error);
      require(idle && active && cropped && traced, error);

      scene.clear();
      const nlohmann::json study = {{"centerX", 64}, {"centerY", 47}, {"holeSize", 1.5},
                                   {"diskBrightness", 1.05}, {"starSize", 1.6}, {"starBrightness", 1.6}};
      configure(profile(study));
      for (const BlackHoleScene::Position position : {BlackHoleScene::Position{.114f, .42f},
              {.193f, .67f}, {.29f, .36f}, {.375f, .57f}, {.410f, .775f}, {.513f, .595f}, {.75f, .23f}})
        scene.push(38, position, width, height);
      const std::array names{L"warm-disk.png", L"hot-disk.png", L"m87-disk.png"};
      for (unsigned palette = 0; palette < names.size(); ++palette) {
        auto options = study;
        options["diskPalette"] = palette;
        configure(profile(options));
        frame(38.215);
        require(renderer.saveFrame(folder / names[palette], error), error);
      }
    }

    scene.clear();
    const std::array speeds{"nebulaSpeed", "voidSpeed", "starfieldSpeed"};
    const std::array skyFrames{L"sky-nebula.png", L"sky-void.png", L"sky-stars.png"};
    std::array<std::uint64_t, 3> skyBlue{};
    for (unsigned mode = 0; mode < speeds.size(); ++mode) {
      nlohmann::json options{{"skyMode", mode}, {speeds[mode], 1.4}};
      auto backdrop = profile(options);
      backdrop.blackHole.diskBrightness = 0;
      const double start = 50 + mode * 2;
      configure(backdrop);
      frame(start, true);
      const auto original = capture(renderer);
      for (int tick = 0; tick <= 10; ++tick)
        frame(start + .08 * tick);
      const auto movedSky = capture(renderer);
      require(movedSky.rgba != original.rgba, "The chosen sky must animate");
      options[speeds[mode]] = 0;
      backdrop = profile(options);
      backdrop.blackHole.diskBrightness = 0;
      configure(backdrop);
      frame(start + .86);
      require(capture(renderer).rgba == movedSky.rgba, "Freezing a sky must preserve its current position");
      require(movedSky.brightness(center) < 4, "The sky must respect the black hole shadow");
      for (std::size_t i = 2; i < movedSky.rgba.size(); i += 4)
        skyBlue[mode] += movedSky.rgba[i];
      if (preview) {
        options.update({{"centerX", 61}, {"centerY", 47}, {"holeSize", 1.35}, {"diskBrightness", .88}});
        configure(profile(options));
        frame(start + .86);
        require(renderer.saveFrame(fs::path(argv[2]) / skyFrames[mode], error), error);
      }
    }
    require(skyBlue[0] > skyBlue[2] + 10000 && skyBlue[2] > skyBlue[1] + 10000,
            "The cloud, star field, and void should have distinct background light levels");

    auto isolated = profile({{"skyMode", 1}, {"voidBrightness", 0},
        {"nebulaBrightness", 2}, {"starfieldBrightness", 2}, {"voidSpeed", 0}});
    isolated.blackHole.diskBrightness = 0;
    configure(isolated);
    frame(57, true);
    const auto blackSky = capture(renderer);
    require(blackSky.brightness({.03f, .05f}) == 0,
            "Each mode must use its own brightness and leave the other modes' settings independent");
    std::cout << "Black hole optics, star sizes, disk palettes and independent animated skies verified\n";
    return 0;
  } catch (const std::exception &problem) {
    std::cerr << problem.what() << '\n';
    return 1;
  }
}
