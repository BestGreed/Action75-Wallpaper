#include "GlassShatter.h"
#include "NativeRenderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace action75;
namespace fs = std::filesystem;

namespace {
constexpr const char *packId = "glass-shatter";
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Pixels {
  unsigned width, height;
  std::vector<std::uint8_t> rgba;

  unsigned changedNear(const Pixels &baseline, float x, float y, float inner, float outer,
                       int threshold = 7) const {
    const float scale = static_cast<float>(std::min(width, height)) / 720.f;
    const float centerX = x * width, centerY = y * height;
    unsigned changed = 0;
    for (unsigned row = 0; row < height; ++row)
      for (unsigned column = 0; column < width; ++column) {
        const float dx = (column + .5f - centerX) / scale;
        const float dy = (row + .5f - centerY) / scale;
        const float radiusSquared = dx * dx + dy * dy;
        if (radiusSquared < inner * inner || radiusSquared > outer * outer)
          continue;
        const auto offset = (std::size_t(row) * width + column) * 4;
        const auto difference = std::max({std::abs(rgba[offset] - baseline.rgba[offset]),
                                          std::abs(rgba[offset+1] - baseline.rgba[offset+1]),
                                          std::abs(rgba[offset+2] - baseline.rgba[offset+2])});
        changed += difference >= threshold;
      }
    return changed;
  }
};

Pixels capture(const NativeRenderer &renderer) {
  auto *context = renderer.context();
  ComPtr<ID3D11RenderTargetView> view;
  context->OMGetRenderTargets(1, &view, nullptr);
  ComPtr<ID3D11Resource> resource;
  view->GetResource(&resource);
  ComPtr<ID3D11Texture2D> target, staging;
  require(SUCCEEDED(resource.As(&target)), "Render target must be a texture");
  D3D11_TEXTURE2D_DESC desc{};
  target->GetDesc(&desc);
  Pixels image{desc.Width, desc.Height, std::vector<std::uint8_t>(size_t(desc.Width) * desc.Height * 4)};
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Create readback texture");
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE data{};
  require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &data)), "Map render target");
  const auto rowBytes = size_t(desc.Width) * 4;
  for (unsigned y = 0; y < desc.Height; ++y)
    std::memcpy(image.rgba.data() + y * rowBytes,
                static_cast<const std::uint8_t *>(data.pData) + y * data.RowPitch, rowBytes);
  context->Unmap(staging.Get(), 0);
  return image;
}

void verifyScene(NativeRenderer &renderer, GlassShatter &scene, PackSettingsStore &settings) {
  std::string error;
  const auto frame = [&](double time) {
    renderer.beginFrame(true);
    require(scene.draw(renderer.context(), renderer.width(), renderer.height(), time, error), error);
    return capture(renderer);
  };
  const auto set = [&](nlohmann::json values) {
    scene.configure(settings.evaluate(settings.prepare(packId, values)));
  };
  const auto lower = settings.prepare(packId, {{"impactMin", 2.4}});
  const auto upper = settings.prepare(packId, {{"impactMax", .2}});
  const auto reversed = settings.prepare(packId, {{"impactMin", 2.4}, {"impactMax", .2}});
  require(lower.values["impactMax"] == 2.4 && upper.values["impactMin"] == .2 &&
              reversed.values["impactMin"] == .2 && reversed.values["impactMax"] == 2.4,
          "The two impact sliders crossed");
  const double maxStrength = GlassShatterAppearance::maxImpactStrength;
  const double maxSpread = GlassShatterAppearance::maxSpread;
  const auto newUpper = settings.prepare(packId, {{"impactMin", maxStrength}, {"spread", maxSpread}});
  require(newUpper.values["impactMax"] == maxStrength && newUpper.values["spread"] == maxSpread,
          "The expanded strength and spread limits did not reach the renderer");

  set({{"impactMin", .3}, {"impactMax", 1.15}});
  float minimum = 2.4f, maximum = 0;
  for (unsigned i = 0; i < 120; ++i) {
    require(scene.push(10, .5f, .5f), "A random impact was rejected");
    minimum = std::min(minimum, scene.lastStrength());
    maximum = std::max(maximum, scene.lastStrength());
  }
  require(scene.count() == 1, "Repeated presses at one key must share one visible fracture");
  require(minimum >= .3f && maximum <= 1.15f && maximum - minimum > .7f,
          "Random impact strengths did not span the requested range");
  scene.clear();
  for (unsigned i = 0; i < 120; ++i)
    require(scene.push(10.5, (i % 12) / 11.f, (i / 12) / 9.f), "A distinct impact was rejected");
  require(scene.count() == GlassShatter::maxImpacts, "The distinct-impact pool exceeded its limit");
  scene.clear();
  set({{"impactMin", 0}, {"impactMax", 0}});
  require(!scene.push(11, .5f, .5f) && scene.count() == 0, "Zero impact strength should stay idle");

  set({{"impactMin", 2.4}, {"impactMax", 2.4}});
  const auto idle = frame(12);
  require(!scene.push(12, -.1f, .5f) && !scene.push(12, .5f, std::numeric_limits<float>::quiet_NaN()),
          "Invalid impact coordinates were accepted");
  constexpr float x = .31f, y = .53f, remote = .74f;
  require(scene.push(12, x, y) && scene.lastStrength() == 2.4f, "The fixed upper strength was not used");
  const auto first = frame(12);
  require(first.changedNear(idle, x, y, 0, 54) > 20 && first.changedNear(idle, remote, y, 0, 170) == 0,
          "The first frame missed its origin or affected a remote key");
  require(scene.push(12.025, x, y) && scene.push(12.025, remote, y) && scene.count() == 2,
          "A repeated key added another fracture or a different key was lost");
  const auto multiple = frame(12.18);
  const auto heavy = multiple.changedNear(idle, remote, y, 115, 220);
  require(multiple.changedNear(idle, x, y, 0, 135) > 180 && heavy > 40,
          "The crack branches did not grow under both keys");
  require(frame(13.30).rgba == idle.rgba && scene.count() == 0, "Cracks did not fade back to idle");

  set({{"impactMin", .25}, {"impactMax", .25}});
  require(scene.push(14, remote, y) && scene.lastStrength() == .25f, "The fixed lower strength was not used");
  require(frame(14.16).changedNear(idle, remote, y, 115, 220) < heavy / 4,
          "Low and high impacts produced similar reach");
  scene.clear();
  require(frame(14.17).rgba == idle.rgba, "Clearing left residual crack fragments");

  set({{"impactMin", 2.25}, {"impactMax", 2.25}, {"chips", 0}});
  require(scene.push(14.2, .49f, .40f), "Could not create the fixed impact for slider comparisons");
  const auto style = [&](const char *name, double value) {
    nlohmann::json values{{"chips", 0}};
    values[name] = value;
    set(values);
    return frame(14.36);
  };
  const auto cropped = style("spread", .55);
  const auto extended = style("spread", maxSpread);
  require(extended.changedNear(idle, .49f, .40f, 145, 370) >
              cropped.changedNear(idle, .49f, .40f, 145, 370) + 30,
          "The spread slider did not expand the crack footprint");
  struct Control { const char *name; double minimum, maximum; };
  constexpr Control controls[]{{"branches", 0, 7}, {"seamWidth", .55, 1.5}, {"glints", 0, 2},
                               {"refraction", 0, 2}, {"chips", 0, 4}};
  for (const auto &control : controls) {
    const auto &[name, minimumValue, maximumValue] = control;
    const auto lower = style(name, minimumValue);
    const auto upper = style(name, maximumValue);
    require(upper.changedNear(lower, .49f, .40f, 0, 320, 2) > 10,
            std::string(name) + " did not change rendered pixels");
  }
  auto noCore = settings.evaluate(settings.prepare(packId, {{"chips", 0}}));
  noCore.scale = 0;
  scene.configure(noCore);
  const auto bareCracks = frame(14.36);
  const auto smallCore = style("coreSpread", .5);
  const auto mediumCore = style("coreSpread", 1.75);
  const auto wideCore = style("coreSpread", 3);
  const auto smallArea = smallCore.changedNear(bareCracks, .49f, .40f, 0, 500, 4);
  const auto mediumArea = mediumCore.changedNear(bareCracks, .49f, .40f, 0, 500, 4);
  const auto wideArea = wideCore.changedNear(bareCracks, .49f, .40f, 0, 500, 4);
  require(smallArea > 80 && mediumArea > smallArea * 3 && wideArea > mediumArea * 1.5,
          "The center scale did not grow the area of shattered glass");
  require(bareCracks.changedNear(idle, .49f, .40f, 250, 2000) > 12 &&
              mediumCore.changedNear(smallCore, .49f, .40f, 250, 2000, 1) == 0,
          "Widening the attached glass altered the distant fracture pixels");
  set({{"duration", 2.8}});
  require(frame(14.8).rgba != idle.rgba && scene.count() == 1, "A long duration expired early");
  set({{"duration", .4}});
  require(frame(14.8).rgba == idle.rgba && scene.count() == 0, "A short duration did not stop drawing");

  set({{"backdrop", 1}});
  const auto amber = frame(15);
  set({{"backdrop", 2}});
  const auto silver = frame(15);
  require(amber.changedNear(idle, .5f, .5f, 0, 2000) > 100000 &&
              silver.changedNear(amber, .5f, .5f, 0, 2000) > 100000,
          "Backdrop choices did not produce distinct frames");
  require(renderer.resize(540, 480, error), error);
  const auto resized = frame(16);
  require(scene.push(16, 1, .45f), "The edge of a resized screen rejected valid input");
  const auto edge = frame(16.15);
  require(edge.changedNear(resized, 1, .45f, 0, 160) > 40 &&
              edge.changedNear(resized, 0, .45f, 0, 160) == 0,
          "Resize or edge clipping moved an impact");

  scene.clear();
  set({{"backdrop", 2}, {"brightness", 0}});
  const auto blackout = frame(17);
  require(blackout.changedNear(resized, .5f, .5f, 0, 2000) > 100000,
          "Background brightness did not affect the backdrop");
  require(scene.push(17, .5f, .5f), "A dark backdrop rejected an impact");
  require(frame(17.10).changedNear(blackout, .5f, .5f, 0, 120) > 70,
          "Dimming the backdrop also hid the cracks");

  // The maximum crack may leave the screen, so test both viewport edges at the normal aspect ratio.
  scene.clear();
  require(renderer.resize(960, 360, error), error);
  set({{"impactMin", maxStrength}, {"impactMax", maxStrength}, {"spread", maxSpread}, {"coreSpread", 3}});
  const auto wideIdle = frame(18);
  for (const float edgeX : {0.f, 1.f}) {
    require(scene.push(18, edgeX, .5f) && scene.lastStrength() == static_cast<float>(maxStrength),
            "Maximum force could not be applied at an edge");
    const auto clipped = frame(18.25);
    require(clipped.changedNear(wideIdle, edgeX, .5f, 0, 160) > 30 &&
                clipped.changedNear(wideIdle, 1.f - edgeX, .5f, 0, 48) == 0,
            "The expanded fracture was clipped at its origin or wrapped across the viewport");
    scene.clear();
  }
}

void writePreviews(NativeRenderer &renderer, GlassShatter &scene, PackSettingsStore &settings,
                   const fs::path &pack, const fs::path &output, bool sequence) {
  fs::create_directories(output);
  std::ifstream calibration(pack.parent_path().parent_path() / "assets/defaults/fm-calibration.json");
  const auto points = nlohmann::json::parse(calibration).at("points");
  std::string error;
  const auto save = [&](double time, const fs::path &filename) {
    renderer.beginFrame(true);
    require(scene.draw(renderer.context(), renderer.width(), renderer.height(), time, error), error);
    require(renderer.saveFrame(output / filename, error), error);
  };
  auto appearance = settings.evaluate(settings.current(packId));
  const auto press = [&](const char *key, double time, float strength) {
    auto fixed = appearance;
    fixed.glassShatter.impactMin = fixed.glassShatter.impactMax = strength;
    scene.configure(fixed);
    const auto &point = points.at(key);
    require(scene.push(time, point.at("x"), point.at("y")), "Preview key was rejected");
  };
  scene.configure(appearance);
  save(0, "idle.png");
  press("KeyD", .023, 1.05f);
  press("KeyH", .067, 2.05f);
  press("KeyP", .096, 1.29f);
  constexpr const char *filenames[]{"midnight.png", "amber.png", "silver.png"};
  for (unsigned look = 0; look < std::size(filenames); ++look) {
    auto styled = appearance;
    styled.glassShatter.backdrop = static_cast<GlassBackdrop>(look);
    scene.configure(styled);
    save(.212, filenames[look]);
  }
  if (!sequence)
    return;

  scene.clear();
  scene.configure(appearance);
  struct KeyPress { unsigned frame; const char *key; float strength; };
  constexpr KeyPress cue[]{
      {12, "KeyH", .43f}, {27, "KeyH", .89f}, {29, "KeyP", 1.52f}, {33, "KeyH", 1.77f},
      {43, "KeyD", 2.4f}, {43, "KeyL", 1.68f}, {52, "Space", 1.17f}, {69, "ArrowRight", 1.23f},
  };
  fs::create_directories(output / "frames");
  for (unsigned index = 0; index < 150; ++index) {
    const double time = 2 + index / 60.0;
    for (const auto &event : cue)
      if (event.frame == index)
        press(event.key, time, event.strength);
    std::ostringstream filename;
    filename << "frame-" << std::setfill('0') << std::setw(3) << index << ".png";
    save(time, fs::path("frames") / filename.str());
  }
}

void writeStrengthPreviews(NativeRenderer &renderer, PackSettingsStore &settings, const fs::path &output) {
  fs::create_directories(output);
  struct Preview { const char *filename; float force; };
  const Preview examples[]{{"light.png", .45f}, {"medium.png", 1.15f}, {"heavy.png", 2.25f}};
  for (const auto &example : examples) {
    std::string error;
    GlassShatter scene(0xa751u);
    require(scene.load(renderer.device(), error), error);
    const auto fixed = settings.prepare(packId, {{"impactMin", example.force}, {"impactMax", example.force}});
    scene.configure(settings.evaluate(fixed));
    require(scene.push(1, .515f, .50f), "The preview impact was rejected");
    renderer.beginFrame(true);
    require(scene.draw(renderer.context(), renderer.width(), renderer.height(), 1.19, error), error);
    require(renderer.saveFrame(output / example.filename, error), error);
  }
}

void writeCoveragePreviews(NativeRenderer &renderer, PackSettingsStore &settings, const fs::path &output) {
  const auto profile = settings.current(packId);
  const auto maximum = [&](std::string_view id) {
    for (const auto &group : profile.schema.at("groups"))
      for (const auto &control : group.at("controls"))
        if (control.at("id").get<std::string>() == id)
          return control.at("max").get<double>();
    throw std::runtime_error("Missing range in glass preview schema");
  };
  const double maxStrength = maximum("impactMax");
  const double maxSpread = maximum("spread");
  const double defaultSpread = profile.values.at("spread").get<double>();
  struct Preview { const char *name; double force, spread; float x, y; };
  const Preview examples[]{
      {"idle", 0, defaultSpread, .515f, .50f},
      {"light", .45, defaultSpread, .515f, .50f},
      {"default", profile.values.at("impactMax").get<double>(), defaultSpread, .515f, .50f},
      {"heavy", 2.40, defaultSpread, .515f, .50f},
      {"limit", maxStrength, maxSpread, .515f, .50f},
      {"left-edge", maxStrength, maxSpread, .015f, .53f},
      {"right-edge", maxStrength, maxSpread, .99f, .47f},
  };
  constexpr const char *backdrops[]{"midnight", "amber", "silver"};
  for (unsigned backdrop = 0; backdrop < std::size(backdrops); ++backdrop) {
    const auto directory = output / backdrops[backdrop];
    fs::create_directories(directory);
    for (const auto &example : examples) {
      std::string error;
      GlassShatter scene(0xa751u);
      require(scene.load(renderer.device(), error), error);
      const auto fixed = settings.prepare(packId, {{"impactMin", example.force}, {"impactMax", example.force},
                                                   {"spread", example.spread}, {"backdrop", backdrop}});
      scene.configure(settings.evaluate(fixed));
      if (example.force > 0)
        require(scene.push(1, example.x, example.y), "The coverage preview impact was rejected");
      renderer.beginFrame(true);
      require(scene.draw(renderer.context(), renderer.width(), renderer.height(), 1.25, error), error);
      const auto filename = std::string(example.name) + ".png";
      require(renderer.saveFrame(directory / filename, error), error);
    }
  }
}

void writeCorePreviews(NativeRenderer &renderer, PackSettingsStore &settings, const fs::path &output) {
  constexpr const char *backdrops[]{"midnight", "amber", "silver"};
  constexpr std::array<float, 5> sizes{.5f, 1.75f, 3.f, 1.f, 0.f};
  constexpr const char *samples[]{"low", "medium", "high", "default", "bare-cracks"};
  std::string error;
  const auto base = settings.evaluate(settings.prepare(packId, {{"impactMin", 2.4}, {"impactMax", 2.4}}));
  const auto save = [&](GlassShatter &scene, const fs::path &file) {
    renderer.beginFrame(true);
    require(scene.draw(renderer.context(), renderer.width(), renderer.height(), 1.25, error), error);
    require(renderer.saveFrame(file, error), error);
  };
  for (unsigned backdrop = 0; backdrop < std::size(backdrops); ++backdrop) {
    const auto directory = output / backdrops[backdrop];
    fs::create_directories(directory);
    GlassShatter scene(0xa751u);
    require(scene.load(renderer.device(), error), error);
    auto appearance = base;
    appearance.glassShatter.backdrop = static_cast<GlassBackdrop>(backdrop);
    scene.configure(appearance);
    save(scene, directory / "idle.png");
    require(scene.push(1, .515f, .50f), "Center preview impact was rejected");
    for (std::size_t index = 0; index < sizes.size(); ++index) {
      appearance.scale = sizes[index];
      scene.configure(appearance);
      save(scene, directory / (std::string(samples[index]) + ".png"));
    }
  }
  constexpr float edgeXs[]{.013f, .988f};
  constexpr const char *edgeNames[]{"left-edge.png", "right-edge.png"};
  for (unsigned edge = 0; edge < std::size(edgeXs); ++edge) {
    GlassShatter scene(0xa751u);
    require(scene.load(renderer.device(), error), error);
    auto appearance = base;
    appearance.scale = sizes[2];
    scene.configure(appearance);
    require(scene.push(1, edgeXs[edge], .50f), "Center preview edge impact was rejected");
    save(scene, output / edgeNames[edge]);
  }
  GlassShatter burst(0xa751u);
  require(burst.load(renderer.device(), error), error);
  auto appearance = base;
  appearance.scale = sizes[2];
  burst.configure(appearance);
  require(burst.push(1, .515f, .50f) && burst.push(1.06, .43f, .59f) &&
              burst.push(1.10, .515f, .50f) && burst.push(1.14, .61f, .46f) && burst.count() == 3,
          "The high-core burst duplicated or lost a strike");
  save(burst, output / "high-burst.png");
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int code = 0;
  try {
    const bool sequence = argc == 4 && std::wstring_view(argv[3]) == L"--sequence";
    const bool strengths = argc == 4 && std::wstring_view(argv[3]) == L"--strengths";
    const bool coverage = argc == 4 && std::wstring_view(argv[3]) == L"--coverage";
    const bool core = argc == 4 && std::wstring_view(argv[3]) == L"--core-sizes";
    require(argc == 2 || argc == 3 || sequence || strengths || coverage || core,
            "Pass pack [output-dir [--sequence|--strengths|--coverage|--core-sizes]]");
    require(SUCCEEDED(com), "Initialize WIC");
    const fs::path pack = fs::absolute(argv[1]);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    PackSettingsStore settings(fs::temp_directory_path() / ("Action75GlassShatter-" + std::to_string(stamp)));
    settings.registerPack(packId, pack, pack / "settings.json");
    require(settings.hasSchema(packId), settings.schemaError(packId));

    const bool preview = argc >= 3;
    NativeRenderer renderer;
    std::string error;
    require(renderer.initialize(nullptr, preview ? 1920 : 960, preview ? 720 : 360,
                                true, error, preview), error);
    GlassShatter scene(0xa751u);
    require(scene.load(renderer.device(), error), error);
    if (core)
      writeCorePreviews(renderer, settings, argv[2]);
    else if (coverage)
      writeCoveragePreviews(renderer, settings, argv[2]);
    else if (strengths)
      writeStrengthPreviews(renderer, settings, argv[2]);
    else if (preview)
      writePreviews(renderer, scene, settings, pack, argv[2], sequence);
    else
      verifyScene(renderer, scene, settings);
    std::cout << "Glass rendering checks completed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    code = 1;
  }
  if (SUCCEEDED(com))
    CoUninitialize();
  return code;
}
