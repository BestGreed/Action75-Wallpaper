#include "Action75Wallpaper.h"
#include "FireworksScene.h"
#include "WaterSurface.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace action75;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr UINT width = 480, height = 180;
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Output {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> texture, staging;
  ComPtr<ID3D11RenderTargetView> view;
  Output() {
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "create WARP device");
    D3D11_TEXTURE2D_DESC t{};
    t.Width = width; t.Height = height; t.MipLevels = t.ArraySize = t.SampleDesc.Count = 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.BindFlags = D3D11_BIND_RENDER_TARGET;
    require(SUCCEEDED(device->CreateTexture2D(&t, nullptr, &texture)) &&
            SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view)), "create render target");
    t.BindFlags = 0; t.Usage = D3D11_USAGE_STAGING; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(device->CreateTexture2D(&t, nullptr, &staging)), "create readback texture");
  }
  void begin() {
    context->ClearState();
    auto *rt = view.Get();
    context->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &vp);
  }
  std::vector<unsigned> pixels() {
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "read frame");
    std::vector<unsigned> result(width * height);
    for (unsigned y = 0; y < height; ++y)
      std::memcpy(result.data() + y * width, static_cast<const char *>(m.pData) + y * m.RowPitch, width * 4);
    context->Unmap(staging.Get(), 0);
    return result;
  }
};

static void compare(const fs::path &dir, bool water) {
  Output old, next;
  std::string error;
  WaterSurface waves;
  FireworksScene fireworks;
  auto controls = PackSettingsStore::packageControls(dir / "settings.json");
  // Both solvers must receive the same impact strength despite independent RNG seeds.
  if (water) controls.values["impactMax"] = controls.values["impactMin"];
  auto values = controls.values.dump();
  require(water ? waves.load(old.device.Get(), old.context.Get(), width, height, error)
                : fireworks.load(old.device.Get(), width, height, error), error.c_str());
  auto module = LoadLibraryW((dir / "wallpaper.dll").c_str());
  require(module != nullptr, "load independent pack");
  auto get = reinterpret_cast<A75GetWallpaper>(GetProcAddress(module, "Action75GetWallpaper"));
  require(get != nullptr, "locate wallpaper ABI");
  auto api = get(ACTION75_WALLPAPER_ABI);
  require(api != nullptr, "load wallpaper ABI");
  A75Create create{sizeof(A75Create),width,height,next.device.Get(),next.context.Get(),dir.c_str(),values.c_str()};
  auto instance = api->create(&create);
  require(instance != nullptr, "create independent scene");
  for (int phase=0; phase<2; ++phase) {
    if (phase) {
      if (water) controls.values["impactMin"] = controls.values["impactMax"] = 1.8;
      else { controls.values["keyInterval"] = 3; controls.values["shape"] = 8; }
      values = controls.values.dump();
      A75Event settings{sizeof(A75Event),A75_SETTINGS,width,height,1,0,0,"",values.c_str()};
      api->on_event(instance,&settings);
    }
    auto appearance = PackSettingsStore::evaluate(controls);
    if (water) waves.configure(appearance); else fireworks.configure(appearance);
    for (int i=0;i<15;++i) {
      double now = phase + i / 60.;
      if (i == 1 || i == 3 || i == 5 || i == 9) {
        float x = .17f + i * .063f, y = .54f;
        const bool pointer = i == 9;
        A75Event key{sizeof(A75Event),pointer ? A75_POINTER_DOWN : A75_KEY_DOWN,width,height,now,x,y,"KeyQ","{}"};
        api->on_event(instance,&key);
        if (water) waves.push({x,y});
        else if (pointer || fireworks.readyForKeyBurst()) fireworks.push(now,x,y);
      }
      old.begin();
      require(water ? waves.draw(old.context.Get(),width,height,now,false,error)
                    : fireworks.draw(old.context.Get(),width,height,now,error), "legacy frame");
      next.begin();
      A75Frame frame{sizeof(A75Frame),width,height,now,next.view.Get()};
      require(api->render(instance,&frame)>=-1, "independent frame");
      if(old.pixels()!=next.pixels()) {
        std::cerr << dir.filename() << " phase " << phase << " frame " << i << '\n';
        throw std::runtime_error("independent frame differs from reference");
      }
    }
  }
  api->destroy(instance);
  FreeLibrary(module);
}

static void verifyBackgroundControls(const fs::path &dir) {
  Output output;
  auto profile = PackSettingsStore::packageControls(dir / "settings.json");
  const auto path = dir / "wallpaper.dll";
  auto module = LoadLibraryW(path.c_str());
  require(module != nullptr, "load wallpaper for appearance test");
  auto get = reinterpret_cast<A75GetWallpaper>(GetProcAddress(module, "Action75GetWallpaper"));
  require(get != nullptr, "missing wallpaper export");
  auto api = get(ACTION75_WALLPAPER_ABI);
  auto defaults = profile.values.dump();
  A75Create create{sizeof(create),width,height,output.device.Get(),output.context.Get(),dir.c_str(),defaults.c_str()};
  void *instance = api->create(&create);
  require(instance != nullptr, "create wallpaper for appearance test");
  const auto render = [&](const nlohmann::json &values) {
    auto text = values.dump();
    A75Event settings{sizeof(settings),A75_SETTINGS,width,height,0,0,0,"",text.c_str()};
    api->on_event(instance,&settings);
    output.begin();
    A75Frame frame{sizeof(frame),width,height,0,output.view.Get()};
    require(api->render(instance,&frame)>=-1, "render appearance");
    return output.pixels();
  };
  const auto original = render(profile.values);
  unsigned checked = 0;
  for (const auto &group : profile.schema.at("groups")) for (const auto &c : group.at("controls")) {
    if (!c.contains("appearance")) continue;
    const bool bg = c.value("surface",std::string()) == "background";
    const auto role = c.at("appearance").get<std::string>();
    const auto id = c.at("id").get<std::string>();
    auto values = profile.values;
    if (role=="style") values[id]=c.at("options").back().at("value");
    else if (role=="hue") values[id]=120;
    else if (role=="saturation" || role=="brightness") values[id]=0;
    else if (c.at("type")=="color") values[id]={220,50,20};
    else continue;
    const bool changed = render(values) != original;
    if (changed != bg) throw std::runtime_error(dir.string()+" incorrect idle appearance for "+id);
    if(bg) ++checked;
    require(render(profile.values)==original,"restore neutral background");
  }
  require(checked==5,"missing background appearance role");
  api->destroy(instance);
  FreeLibrary(module);
}

int wmain(int argc,wchar_t **argv) {
  try {
    require(argc==2 || argc==3, "expected library directory and optional previous release");
    if (argc==3) {
      compare(fs::absolute(fs::path(argv[2]) / "stillwater"),true);
      compare(fs::absolute(fs::path(argv[2]) / "night-fireworks"),false);
    }
    compare(fs::path(argv[1]) / "stillwater",true);
    compare(fs::path(argv[1]) / "night-fireworks",false);
    for (auto id : {"stillwater","glass-shatter","night-fireworks"})
      verifyBackgroundControls(fs::absolute(fs::path(argv[1]) / id));
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
