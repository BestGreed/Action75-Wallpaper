#include "FireworksScene.h"
#include "NativeRenderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace action75;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
void require(bool success, const std::string &message) {
  if (!success)
    throw std::runtime_error(message);
}

struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~Apartment() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};

class Readback {
public:
  Readback(NativeRenderer &renderer)
      : renderer_(renderer), width_(renderer.width()), height_(renderer.height()) {
    D3D11_TEXTURE2D_DESC info{};
    info.Width = width_;
    info.Height = height_;
    info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.MipLevels = info.ArraySize = info.SampleDesc.Count = 1;
    info.Usage = D3D11_USAGE_STAGING;
    info.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer_.device()->CreateTexture2D(&info, nullptr, &staging_)),
            "Create fireworks readback texture");
  }

  void copy(std::vector<unsigned char> &pixels) {
    auto *context = renderer_.context();
    ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, &view, nullptr);
    require(static_cast<bool>(view), "Fireworks did not restore its output target");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    context->CopyResource(staging_.Get(), resource.Get());
    pixels.resize(static_cast<std::size_t>(width_) * height_ * 4);
    D3D11_MAPPED_SUBRESOURCE map{};
    require(SUCCEEDED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)), "Read fireworks pixels");
    for (unsigned row = 0; row < height_; ++row)
      std::memcpy(pixels.data() + static_cast<std::size_t>(row) * width_ * 4,
                  static_cast<const unsigned char *>(map.pData) + static_cast<std::size_t>(row) * map.RowPitch,
                  width_ * 4);
    context->Unmap(staging_.Get(), 0);
  }

private:
  NativeRenderer &renderer_;
  unsigned width_, height_;
  ComPtr<ID3D11Texture2D> staging_;
};

std::array<float, 2> peak(const std::vector<unsigned char> &before,
                          const std::vector<unsigned char> &after, unsigned width, unsigned height) {
  float strength = 0, x = 0, y = 0;
  for (unsigned row = 0; row < height; ++row) {
    for (unsigned col = 0; col < width; ++col) {
      const auto offset = (static_cast<std::size_t>(row) * width + col) * 4;
      const auto change = std::max({after[offset] - before[offset], after[offset + 1] - before[offset + 1],
                                    after[offset + 2] - before[offset + 2]});
      if (change < 68)
        continue;
      x += (col + .5f) * change;
      y += (row + .5f) * change;
      strength += static_cast<float>(change);
    }
  }
  require(strength > 150, "The first frame did not light the key");
  return {x / strength, y / strength};
}

struct Keyframe {
  unsigned frame, shape;
  float x, y, scale;
};
// Compare the new silhouettes at several positions, scales, and palettes.
constexpr Keyframe sequence[]{
    {5, 8, .19f, .46f, 1.63f}, {16, 4, .72f, .67f, 2.00f},
    {21, 9, .43f, .35f, 2.3f}, {27, 10, .86f, .30f, 1.8f},
    {173, 8, .49f, .48f, 2.40f},
    {292, 4, .28f, .70f, 2.60f}, {310, 8, .68f, .47f, 2.20f},
    {391, 8, .48f, .46f, 3.55f},
};

struct LitPoint { unsigned x, y; };

std::vector<LitPoint> brightPoints(const std::vector<unsigned char> &idle,
                                   const std::vector<unsigned char> &lit,
                                   unsigned width, unsigned height) {
  std::vector<LitPoint> points;
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const auto index = (static_cast<std::size_t>(y) * width + x) * 4;
      const int difference = std::max({lit[index] - idle[index], lit[index + 1] - idle[index + 1],
                                       lit[index + 2] - idle[index + 2]});
      if (difference >= 76)
        points.push_back({x,y});
    }
  }
  return points;
}

void requireHeartOutline(const std::vector<LitPoint> &points) {
  require(points.size() > 25, "No visible heart sparks");
  const auto horizontal = std::minmax_element(points.begin(), points.end(),
      [](LitPoint a, LitPoint b) { return a.x < b.x; });
  const auto vertical = std::minmax_element(points.begin(), points.end(),
      [](LitPoint a, LitPoint b) { return a.y < b.y; });
  const unsigned left = horizontal.first->x, top = vertical.first->y;
  const float width = static_cast<float>(horizontal.second->x - left);
  const float height = static_cast<float>(vertical.second->y - top);
  require(width > 16 && height > 14, "Heart outline is too small to see");
  unsigned lobeLeft = 0, lobeRight = 0, notch = 0, tip = 0, strayAtTip = 0, solidCenter = 0;
  for (const auto point : points) {
    const float x = (point.x - left) / width;
    const float y = (point.y - top) / height;
    if (y < .17f) {
      lobeLeft += x < .43f;
      lobeRight += x > .57f;
    }
    notch += y < .085f && x > .465f && x < .535f;
    if (y > .86f) {
      tip += x > .41f && x < .59f;
      strayAtTip += x < .25f || x > .75f;
    }
    solidCenter += x > .41f && x < .59f && y > .40f && y < .61f;
  }
  require(lobeLeft && lobeRight && tip && !notch && !strayAtTip && !solidCenter,
          "Missing heart geometry: lobes=" + std::to_string(lobeLeft) + "/" +
              std::to_string(lobeRight) + ", notch fill=" + std::to_string(notch) +
              ", tip=" + std::to_string(tip) + ", side pixels at tip=" +
              std::to_string(strayAtTip) + ", center fill=" + std::to_string(solidCenter));
}

bool blueDominates(const std::vector<unsigned char> &pixels, unsigned width, unsigned height,
                   float center) {
  const auto left = static_cast<unsigned>((center - .09f) * width);
  const auto right = static_cast<unsigned>((center + .09f) * width);
  std::uint64_t red = 0, blue = 0;
  for (unsigned y = height / 4; y < height * 3 / 4; ++y) {
    for (unsigned x = left; x < right; ++x) {
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
      if (std::max(pixels[offset],pixels[offset + 2]) > 65) {
        red += pixels[offset];
        blue += pixels[offset + 2];
      }
    }
  }
  require(red + blue > 1000, "No pixels to verify the burst palette");
  return blue > red;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  try {
    require(argc == 2 || argc == 3, "Pass the pack and optionally a preview directory or --motion");
    Apartment com;
    require(SUCCEEDED(com.result), "Initialize COM");
    const fs::path pack = argv[1];
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    PackSettingsStore store(fs::temp_directory_path() / ("Action75Fireworks-" + std::to_string(stamp)));
    store.registerPack("night-fireworks", pack, pack / "settings.json");
    require(store.hasSchema("night-fireworks"), store.schemaError("night-fireworks"));
    const auto defaults = store.evaluate(store.current("night-fireworks"));
    require(defaults.scale == 3.5f && defaults.fireworks.selected == 0b1110001111 && defaults.fireworks.keyInterval == 1 &&
            defaults.sourcePalette &&
            !defaults.paletteOverride && !defaults.fireworks.randomPalette &&
            defaults.fireworks.paletteChoices.size() == 6,
            "The pack defaults lost their shape or palette selections");
    const auto randomized =
        store.evaluate(store.prepare("night-fireworks", {{"randomPalette", true}}));
    require(randomized.fireworks.randomPalette &&
                randomized.fireworks.paletteChoices == defaults.fireworks.paletteChoices,
            "The package switch did not enable its named palette rotation");
    const auto wider = store.evaluate(store.prepare("night-fireworks", {{"size",6}}));
    require(wider.scale == 6 && store.legacyPatch("night-fireworks", {{"effectScale",6}})["size"] == 6,
            "The wider radius did not reach both settings paths");

    const bool motion = argc == 3 && std::wstring_view(argv[2]) == L"--motion";
    const bool preview = argc == 3 && !motion;
    const unsigned width = motion || preview ? 1920 : 640;
    const unsigned height = motion || preview ? 720 : 240;
    NativeRenderer renderer;
    std::string error;
    require(renderer.initialize(nullptr, width, height, true, error, motion || preview), error);
    FireworksScene scene;
    require(scene.load(renderer.device(), width, height, error), error);
    scene.configure(defaults);
    const auto frame = [&](double now) {
      renderer.beginFrame(true);
      require(scene.draw(renderer.context(), renderer.width(), renderer.height(), now, error), error);
    };

    if (motion || preview) {
      Readback reader(renderer);
      std::vector<unsigned char> pixels;
      if (motion)
        require(_setmode(_fileno(stdout), _O_BINARY) != -1, "Open video frame pipe");
      const fs::path output = preview ? fs::path(argv[2]) : fs::path();
      if (preview)
        fs::create_directories(output);
      const unsigned stop = preview ? 54 : 494;
      unsigned cue = 0;
      for (unsigned tick = 0; tick < stop; ++tick) {
        const double now = 20 + tick / 60.0;
        if (cue < std::size(sequence) && sequence[cue].frame == tick) {
          const auto &press = sequence[cue++];
          auto appearance = defaults;
          appearance.fireworks.shape = press.shape;
          appearance.fireworks.randomPalette = tick >= 173;
          appearance.scale = press.scale;
          scene.configure(appearance);
          require(scene.push(now, press.x, press.y), "Could not trigger preview key");
        }
        frame(now);
        if (motion) {
          reader.copy(pixels);
          require(fwrite(pixels.data(), 1, pixels.size(), stdout) == pixels.size(), "Write video frame");
        } else if (tick == 6 || tick == 11 || tick + 1 == stop) {
          const auto name = tick == 6 ? "heart-first-frame.png" : (tick == 11 ? "heart-opening.png" : "preview.png");
          require(renderer.saveFrame(output / name, error), error);
        }
      }
      return 0;
    }

    require(!scene.push(10, -.01f, .5f) &&
            !scene.push(10, std::numeric_limits<float>::quiet_NaN(), .5f),
            "Invalid positions entered the firework queue");
    require(scene.readyForKeyBurst() && scene.push(10, .24f, .93f) && scene.lastShape() == 1,
            "First key did not choose the willow");
    require(scene.readyForKeyBurst() && scene.push(10, .75f, .4f) && scene.lastShape() == 4,
            "The next key did not choose the palm");
    require(scene.readyForKeyBurst() && scene.push(10, .6f, .2f) &&
            scene.lastShape() == FireworksAppearance::heartShape,
            "The third key did not choose the heart");
    scene.clear();
    auto one = defaults;
    one.fireworks.shape = 2;
    one.fireworks.keyInterval = 3;
    scene.configure(one);
    Readback reader(renderer);
    std::vector<unsigned char> resting, lit;
    frame(10);
    reader.copy(resting);
    for (const auto &p : {std::array<float, 2>{.24f,.93f}, std::array<float, 2>{.977f,.102f}}) {
      require(!scene.readyForKeyBurst(), "The first key entered the firework queue");
      scene.configure(one);
      require(!scene.readyForKeyBurst() && scene.count() == 0,
              "The second key entered the firework queue");
      require(scene.readyForKeyBurst() && scene.push(10, p[0], p[1]),
              "The third calibrated key did not trigger");
      frame(10 + 1.0 / 60);
      reader.copy(lit);
      const auto center = peak(resting, lit, width, height);
      require(std::hypot(center[0] - p[0] * width, center[1] - p[1] * height) < 3,
              "Newborn sparks moved away from the original pressed point");
      scene.clear();
    }
    auto sequenced = defaults;
    sequenced.fireworks.order = {8, 1, 4};
    sequenced.fireworks.selected = (1u << 7) | (1u << 3);
    sequenced.fireworks.keyInterval = 3;
    scene.configure(sequenced);
    require(!scene.readyForKeyBurst(), "Alternate sequence accepted the first key");
    require(scene.push(10, .3f, .4f) && scene.lastShape() == 8,
            "A pointer burst must play immediately in the declared order");
    require(!scene.readyForKeyBurst(), "The pointer burst changed the keyboard counter");
    require(scene.readyForKeyBurst() && scene.push(10, .5f, .4f) && scene.lastShape() == 4,
            "The third key did not take the next checked shape");
    require(scene.push(10, .7f, .4f) && scene.lastShape() == 8,
            "The next pointer burst did not wrap the declared sequence");
    scene.clear();
    sequenced.fireworks.shuffle = true;
    scene.configure(sequenced);
    for (unsigned i = 0; i < 24; ++i)
      require(scene.push(10, .5f, .4f) && (scene.lastShape() == 8 || scene.lastShape() == 4),
              "Random playback selected an unchecked shape");
    scene.clear();
    auto shaped = defaults;
    shaped.fireworks.shape = FireworksAppearance::heartShape;
    shaped.scale = 3;
    shaped.fireworks.bloom = shaped.fireworks.smoke = shaped.fireworks.reflection = 0;
    scene.configure(shaped);
    require(scene.push(10, .49f, .46f), "A heart press was rejected");
    for (const double age : {.052, .16, .64}) {
      frame(10 + age);
      reader.copy(lit);
      requireHeartOutline(brightPoints(resting, lit, width, height));
    }
    scene.clear();
    shaped.scale = 1.2f;
    shaped.fireworks.randomPalette = randomized.fireworks.randomPalette;
    shaped.fireworks.paletteChoices = {{{255,28,44}}, {{27,46,255}}};
    scene.configure(shaped);
    require(scene.push(10, .26f, .45f), "Random palette refused the first burst");
    const unsigned firstPalette = scene.lastPalette();
    require(scene.push(10.06, .73f, .45f), "Random palette refused the second burst");
    const unsigned secondPalette = scene.lastPalette();
    require(firstPalette != secondPalette && firstPalette > 0 && secondPalette > 0,
            "Adjacent random bursts chose the same palette");
    frame(10.68);
    reader.copy(lit);
    require(blueDominates(lit, width, height, .26f) == (firstPalette == 2) &&
            blueDominates(lit, width, height, .73f) == (secondPalette == 2),
            "A new random pick recolored a previous active burst");
    scene.clear();
    auto silent = defaults;
    silent.fireworks.selected = 0;
    scene.configure(silent);
    require(!scene.push(10, .5f, .5f), "A deselected group should stay quiet");
    auto strong = store.evaluate(store.prepare("night-fireworks", {{"quality", 2}, {"density", 1.0},
        {"colors", {{"preset", "rose-lilac"}, {"colors", {{255, 79, 155}, {161, 116, 255}, {255, 183, 207}}}}}}));
    require(strong.paletteOverride && strong.resolvedPaletteColors.size() == 3,
            "The selected palette did not reach the GPU settings");
    scene.configure(strong);
    for (unsigned i = 0; i < 570; ++i)
      require(scene.push(11, static_cast<float>(i % 83) / 84, .55f), "The full queue dropped a new input");
    frame(11.2);
    require(scene.count() == FireworksScene::maxBursts &&
            scene.particleCount() <= FireworksScene::maxVisibleSparks,
            "The peak spark count exceeded its GPU budget");
    frame(20);
    reader.copy(lit);
    require(scene.count() == 0 && scene.particleCount() == 0 && lit == resting,
            "Old embers prevented the wallpaper from resting");
    std::cout << "Firework pack, origin, shape selection, palette, stress budget and idle verified\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
