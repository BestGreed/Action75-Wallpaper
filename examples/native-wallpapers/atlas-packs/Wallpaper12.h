#pragma once

#include "Graphics.h"
#include "Settings.h"
#include <exception>

namespace atlas_packs {
class Backdrop {
public:
  explicit Backdrop(ID3D12Device* device);
  void setImage(ID3D12Device* device, const fs::path& file);
  void setCanvas(ID3D12Device* device, ComPtr<ID3D12Resource> texture);
  void configure(const Appearance& settings);
  void draw(const A75Frame12& frame);

private:
  Texture texture_;
  Upload constants_;
  Pipeline pipeline_;
  bool solid_ = false;
  std::array<float, 8> appearance_{1, 1, 0, 0, 1, 1, 1, 0};
  unsigned opaqueWidth_ = 0, opaqueHeight_ = 0;
};

template <class Layer> class Wallpaper12 {
public:
  explicit Wallpaper12(const A75Create12& create)
      : pack_(create.pack_dir), recipe_(readJson(pack_ / L"scene.json")),
        schema_(readJson(pack_ / L"settings.json")), backdrop_(create.device), layer_(create, recipe_) {
    if constexpr (!Layer::canvas)
      backdrop_.setImage(create.device, assetPath(pack_, recipe_.at("background")));
    configure(create.settings_json, 0);
  }

  void onEvent(const A75Event& event) noexcept {
    try {
      switch (event.type) {
      case A75_SETTINGS:
        configure(event.settings_json, event.seconds);
        break;
      case A75_KEY_DOWN:
        if (!paused_ && intensity_ != 0)
          layer_.key(event);
        break;
      case A75_INPUT_RESET:
        layer_.clear();
        break;
      case A75_PAUSE:
        paused_ = true;
        break;
      case A75_RESUME:
        paused_ = false;
        break;
      }
    } catch (const std::exception& error) {
      OutputDebugStringA(error.what());
      failed_ = true;
    } catch (...) {
      failed_ = true;
    }
  }

  int32_t render(const A75Frame12& frame) {
    if (failed_)
      return -2;
    backdrop_.draw(frame);
    return layer_.draw(frame, paused_);
  }

private:
  void configure(const char* values, double now) {
    const auto appearance = evaluate(schema_, values);
    backdrop_.configure(appearance);
    if constexpr (Layer::canvas)
      layer_.configure(appearance, now, backdrop_);
    else
      layer_.configure(appearance, now);
    intensity_ = appearance.intensity;
    if (intensity_ == 0)
      layer_.clear();
  }

  const fs::path pack_;
  const json recipe_, schema_;
  Backdrop backdrop_;
  Layer layer_;
  float intensity_ = 1;
  bool paused_ = false, failed_ = false;
};

template <class Layer> const A75Wallpaper12* api12(uint32_t abi) {
  using State = Wallpaper12<Layer>;
  static const A75Wallpaper12 table{
      sizeof(A75Wallpaper12), ACTION75_WALLPAPER_ABI12,
      [](const A75Create12* c) -> void* {
        if (!c || c->size < sizeof(*c) || !c->device || !c->pack_dir || !c->settings_json ||
            !c->width || !c->height)
          return nullptr;
        try {
          return new State(*c);
        } catch (const std::exception& error) {
          OutputDebugStringA(error.what());
        } catch (...) {
        }
        return nullptr;
      },
      [](void* s, const A75Event* e) {
        if (s && e && e->size >= sizeof(*e))
          static_cast<State*>(s)->onEvent(*e);
      },
      [](void* s, const A75Frame12* f) -> int32_t {
        if (!s || !f || f->size < sizeof(*f) || !f->commands || !f->target || !f->rtv.ptr ||
            !f->width || !f->height)
          return -2;
        try {
          return static_cast<State*>(s)->render(*f);
        } catch (const std::exception& error) {
          OutputDebugStringA(error.what());
        } catch (...) {
        }
        return -2;
      },
      [](void* s) { delete static_cast<State*>(s); },
      nullptr,
  };
  return abi == ACTION75_WALLPAPER_ABI12 ? &table : nullptr;
}
} // namespace atlas_packs
