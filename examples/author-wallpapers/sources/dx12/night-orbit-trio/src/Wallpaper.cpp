#include "Fireworks12.h"
#include <memory>
#include <cmath>
namespace {
struct Wallpaper {
  action75::Fireworks12 scene;
  action75::Configuration config;
  bool paused=false;
  double offset=0,pausedScene=0;
  double time(double wall)const noexcept{return paused?pausedScene:wall-offset;}
};
void* __cdecl create(const A75Create12*c) noexcept {
  try {
    if(!c||c->size<sizeof(*c)||!c->device||!c->pack_dir||!c->width||!c->height)return nullptr;
    auto w=std::make_unique<Wallpaper>();w->config.apply(c->settings_json);
    if(!w->scene.load(c->device,c->width,c->height,c->pack_dir))return nullptr;
    w->scene.configure(w->config.appearance);return w.release();
  }catch(...){return nullptr;}
}
void __cdecl event(void*ptr,const A75Event*e) noexcept {
  if(!ptr||!e||e->size<sizeof(*e)||!std::isfinite(e->seconds))return;
  auto&w=*static_cast<Wallpaper*>(ptr);
  try {
    switch(e->type) {
    case A75_SETTINGS:w.config.apply(e->settings_json);w.scene.configure(w.config.appearance);break;
    case A75_INPUT_RESET:w.scene.clear();break;
    case A75_PAUSE:if(!w.paused){w.pausedScene=w.time(e->seconds);w.paused=true;}break;
    case A75_RESUME:if(w.paused){w.offset=e->seconds-w.pausedScene;w.paused=false;}break;
    case A75_KEY_DOWN:
      if(w.paused)break;
      if(e->code&&*e->code&&!w.scene.readyForKeyBurst())break;
      w.scene.push(w.time(e->seconds),e->x,e->y);break;
    case A75_POINTER_DOWN:if(!w.paused)w.scene.push(w.time(e->seconds),e->x,e->y);break;
    }
  }catch(...){/* Keep the last validated settings and image on malformed input. */}
}
int32_t __cdecl render(void*ptr,const A75Frame12*f) noexcept {
  if(!ptr||!f||f->size<sizeof(*f)||!f->commands||!f->target||!f->width||!f->height||!std::isfinite(f->seconds))return -2;
  try {
    auto&w=*static_cast<Wallpaper*>(ptr);
    if(!w.scene.draw(*f,w.time(f->seconds)))return -2;
    return !w.paused&&w.scene.count()?0:-1;
  }catch(...){return -2;}
}
void __cdecl destroy(void*ptr) noexcept {delete static_cast<Wallpaper*>(ptr);}
int32_t __cdecl hit(void*,float,float,uint32_t,uint32_t) noexcept{return 1;}
}
extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
  static const A75Wallpaper12 api{sizeof(api),ACTION75_WALLPAPER_ABI12,create,event,render,destroy,hit};
  return abi==ACTION75_WALLPAPER_ABI12?&api:nullptr;
}
