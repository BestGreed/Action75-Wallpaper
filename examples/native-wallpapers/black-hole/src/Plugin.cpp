#include "Action75Wallpaper.h"
#include "BlackHoleScene.h"
#include "BlackHoleProfile.h"
#include <json.hpp>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace action75;
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Point = BlackHoleScene::Position;

void require(HRESULT result) {
  if (FAILED(result))
    throw std::runtime_error("Nebula texture could not be loaded");
}

ComPtr<ID3D11ShaderResourceView> loadSky(ID3D11Device *device, const wchar_t *directory) {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> image;
  require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  const auto path = std::filesystem::path(directory) / L"nebula.png";
  require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
  require(decoder->GetFrame(0, &frame));
  require(factory->CreateFormatConverter(&image));
  require(image->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                            nullptr, 0, WICBitmapPaletteTypeCustom));
  UINT width = 0, height = 0;
  require(image->GetSize(&width, &height));
  if (!width || !height || std::uint64_t(width) * height > 4194304)
    throw std::runtime_error("Invalid nebula dimensions");
  std::vector<BYTE> pixels(size_t(width) * height * 4);
  require(image->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
  D3D11_TEXTURE2D_DESC td{};
  td.Width = width;
  td.Height = height;
  td.ArraySize = td.MipLevels = td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  require(device->CreateTexture2D(&td, &data, &texture));
  require(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
  return view;
}

struct Drag { Point start, center; };

struct Instance {
  BlackHoleScene scene;
  ComPtr<ID3D11DeviceContext> context;
  std::optional<Drag> drag;
  Point profileCenter{.58f,.46f};
  void *host;
  decltype(A75Create::commit_settings) commit;
  bool paused = false;

  explicit Instance(const A75Create &create)
      : context(create.context), host(create.host), commit(create.commit_settings) {
    auto sky = loadSky(create.device, create.pack_dir);
    std::string error;
    if (!scene.load(create.device, context.Get(), sky.Get(), error))
      throw std::runtime_error(error);
    configure(create.settings_json);
  }

  void configure(const char *values) {
    const Json profile = values && *values ? Json::parse(values) : Json::object();
    const auto settings = BlackHoleProfile::fromSettings(profile);
    profileCenter = settings.blackHole.center;
    scene.configure(settings.blackHole, settings.starSize, settings.starBrightness, !drag);
  }

  void move(Point cursor) {
    scene.setCenter({drag->center[0] + cursor[0] - drag->start[0],
                     drag->center[1] + cursor[1] - drag->start[1]});
  }

  void cancel() {
    if (drag) scene.setCenter(profileCenter);
    drag.reset();
  }

  void event(const A75Event &e) {
    Point point{e.x, e.y};
    switch (e.type) {
    case A75_KEY_DOWN:
      if (!paused) scene.push(e.seconds, point, e.width, e.height);
      break;
    case A75_POINTER_DOWN:
      if (!paused && scene.covers(point,e.width,e.height)) drag = Drag{point,scene.center()};
      break;
    case A75_POINTER_MOVE:
      if (drag) move(point);
      break;
    case A75_POINTER_UP:
      if (drag) {
        move(point);
        const auto center = scene.center();
        const auto start = drag->center;
        drag.reset();
        if (commit && center != start) {
          auto pct = [](float v) { return std::round(v * 1000.0) / 10.0; };
          const auto patch = Json{{"centerX",pct(center[0])},{"centerY",pct(center[1])}}.dump();
          commit(host,patch.c_str());
        }
      }
      break;
    case A75_SETTINGS: configure(e.settings_json); break;
    case A75_PAUSE: cancel(); paused = true; break;
    case A75_RESUME: paused = false; break;
    case A75_INPUT_RESET: cancel(); scene.clear(); break;
    }
  }
};

void * __cdecl createInstance(const A75Create *c) noexcept {
  try {
    if (!c || c->size < sizeof(*c) || !c->device || !c->context || !c->pack_dir) return nullptr;
    return new Instance(*c);
  } catch (...) { return nullptr; }
}
void __cdecl onEvent(void *ptr,const A75Event *e) noexcept {
  try { if (ptr && e && e->size >= sizeof(*e)) static_cast<Instance *>(ptr)->event(*e); }
  catch (...) { /* Keep the last complete appearance. */ }
}
int32_t __cdecl render(void *ptr, const A75Frame *f) noexcept {
  try {
    if (!ptr || !f || f->size < sizeof(*f) || !f->target) return -2;
    auto &s = *static_cast<Instance *>(ptr);
    s.context->OMSetRenderTargets(1, &f->target, nullptr);
    std::string error;
    if (!s.scene.draw(s.context.Get(),f->width,f->height,f->seconds,s.paused,error)) return -2;
    return s.paused ? -1 : 0;
  } catch (...) { return -2; }
}
void __cdecl destroy(void *ptr) noexcept { delete static_cast<Instance *>(ptr); }
int32_t __cdecl hitTest(void *ptr,float x,float y,uint32_t w,uint32_t h) noexcept {
  if (!ptr || !w || !h) return 0;
  const auto &s = *static_cast<const Instance *>(ptr);
  return !s.paused && (s.drag || s.scene.covers({x,y},w,h));
}
const A75Wallpaper api{sizeof(A75Wallpaper),ACTION75_WALLPAPER_ABI,createInstance,onEvent,render,destroy,hitTest};
}

extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  return abi == ACTION75_WALLPAPER_ABI ? &api : nullptr;
}
