#include "Action75Wallpaper.h"
#include "game.hpp"
#include "appearance.hpp"
#include <cmath>
#include <cstring>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <new>
#include <vector>

namespace {
using Pixel = uint32_t;
struct Scene {
    key_game::Game game;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> clear_context;
    ID3D11RenderTargetView *target = nullptr;
    bool rgba = true;
    std::vector<Pixel> pixels;
    unsigned w = 0, h = 0;
    bool amber = false, pause_held = false;
    double previous = -1;
    enum Station {Sky,Building,Window,Track,Floor,Dash};
    enum Object {Signal,Coin,Glint,Hazard,Shield,Visor,Boots,Digits};
    static constexpr Pixel stationColors[][6]={
        {0xff101b2b,0xff1c3040,0xff30505a,0xff86efce,0xff273646,0xff788881},
        {0xff332238,0xff533346,0xffb77663,0xfff3ad7d,0xff4a3546,0xffab8981},
        {0xff293849,0xff3c5368,0xff7196aa,0xffb3dfed,0xff394858,0xff899caa}
    };
    static constexpr Pixel objectColors[][8]={
        {0xff86efce,0xffffc256,0xffffefd2,0xffff7268,0xffc2bae7,0xff182f42,0xffe2efdb,0xffdcefeb},
        {0xff68ddd8,0xffffda6b,0xfffeefc2,0xfffa56b0,0xff9a82ff,0xff202650,0xffc9eeff,0xffe5eeff},
        {0xfff6b674,0xffffe0a0,0xffffffd1,0xffed6850,0xffe0ab90,0xff3f2924,0xfff2e3c4,0xfff7ead2}
    };
    Pixel station[6]{},objects[8]{};
    SurfaceLook stationLook,runnerLook;
    int stationStyle=0,runnerStyle=0;
    static Pixel adjust(Pixel color,const SurfaceLook& look){
        if(look.neutral())return color;
        auto rgb=look.transform({float((color>>16)&255),float((color>>8)&255),float(color&255)});
        auto channel=[](float v){return Pixel(std::lround(std::clamp(v,0.f,255.f)));};
        return 0xff000000u | (channel(rgb[0])<<16) | (channel(rgb[1])<<8) | channel(rgb[2]);
    }
    void settings(const char* payload){
        auto j=nlohmann::json::parse(payload?payload:"{}",nullptr,false);
        if(j.is_object()){
            auto select=[&](const char* id,int& value){
                auto it=j.find(id);
                if(it!=j.end() && it->is_number())value=int(std::clamp(it->get<double>(),0.,2.));
            };
            auto it=j.find("amber_lights");
            if(it!=j.end() && it->is_boolean())amber=it->get<bool>();
            select("stationStyle",stationStyle);select("runnerStyle",runnerStyle);
            stationLook.read(j,"station");runnerLook.read(j,"runner");
        }
        for(size_t i=0;i<6;++i)
            station[i]=adjust(amber && i==Track ? 0xffffbb59 : stationColors[stationStyle][i],stationLook);
        for(size_t i=0;i<8;++i)
            objects[i]=adjust(amber && i==Signal ? 0xffffbb59 : objectColors[runnerStyle][i],runnerLook);
    }
    void rect(float x, float y, float width, float height, Pixel color) {
        const int x0 = std::clamp(int(x*w/960), 0, int(w));
        const int x1 = std::clamp(int((x+width)*w/960), x0, int(w));
        const int y0 = std::clamp(int(y*h/360), 0, int(h));
        const int y1 = std::clamp(int((y+height)*h/360), y0, int(h));
        if (x0 == x1 || y0 == y1) return;
        if (clear_context) {
            const float channels[] = {
                float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255,
                float(color & 255) / 255, float(color >> 24) / 255
            };
            const D3D11_RECT area{x0, y0, x1, y1};
            clear_context->ClearView(target, channels, &area, 1);
            return;
        }
        if (rgba)
            color = (color & 0xff00ff00u) | ((color & 0xffu) << 16) | ((color >> 16) & 0xffu);
        for (int iy=y0; iy<y1; ++iy)
            std::fill(pixels.begin()+size_t(iy)*w+x0, pixels.begin()+size_t(iy)*w+x1, color);
    }
    void number(unsigned value, float x, float y) {
        static constexpr unsigned masks[] = {63,6,91,79,102,109,125,7,127,111};
        static constexpr int segments[][4] = {{2,0,8,2},{10,2,2,7},{10,11,2,7},{2,18,8,2},{0,11,2,7},{0,2,2,7},{2,9,8,2}};
        do {
            const unsigned mask = masks[value%10];
            for (int i=0;i<7;++i) if (mask & (1u<<i))
                rect(x+segments[i][0],y+segments[i][1],segments[i][2],segments[i][3],objects[Digits]);
            x-=16;
            value/=10;
        } while (value);
    }
    void draw() {
        const Pixel signal = objects[Signal];
        rect(0,0,960,360,station[Sky]);
        for (int i=0; i<26; ++i) {
            const int height = 24 + (i*43)%86;
            rect(i*38,255-height,32,height,station[Building]);
            for (int j=7;j<height-6;j+=13) rect(i*38+6,255-height+j,5,3,station[Window]);
        }
        rect(0,270,960,7,station[Track]); rect(0,277,960,83,station[Floor]);
        for (int i=0;i<31;++i) rect(i*32,286,14,3,station[Dash]);
        rect(game.coin_x-7,240,14,14,objects[Coin]); rect(game.coin_x-2,243,4,8,objects[Glint]);
        for (int i=0;i<19;++i) rect(game.hazard_x-(i+1)*.7f,251+i,(i+1)*1.4f,1,objects[Hazard]);
        const Pixel suit = game.immunity>0 ? objects[Shield] : signal;
        rect(game.x-11,game.y-14,22,18,suit); rect(game.x-6,game.y-10,17,6,objects[Visor]);
        rect(game.x-10,game.y+4,20,5,objects[Boots]);
        rect(game.x-9,game.y+9,6,4,suit); rect(game.x+4,game.y+9,6,4,suit);
        rect(24,24,9,9,objects[Coin]); number(game.score,91,20);
        rect(128,24,9,9,objects[Hazard]); number(game.hits,195,20);
        if (game.paused || game.sleeping) { rect(454,137,14,50,signal); rect(486,137,14,50,signal); }
    }
};

void * __cdecl create(const A75Create *cfg) {
    if (!cfg || cfg->size<sizeof(*cfg) || !cfg->device || !cfg->context) return nullptr;
    auto *scene = new(std::nothrow) Scene;
    if (!scene) return nullptr;
    scene->context = cfg->context;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (SUCCEEDED(cfg->device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
        options.ClearView)
        scene->context.As(&scene->clear_context);
    scene->settings(cfg->settings_json);
    return scene;
}
void __cdecl event(void *instance, const A75Event *e) {
    if (!instance || !e || e->size<sizeof(*e)) return;
    Scene &s = *static_cast<Scene*>(instance);
    auto &g = s.game;
    if (e->type==A75_SETTINGS) { s.settings(e->settings_json); return; }
    if (e->type==A75_PAUSE || e->type==A75_RESUME || e->type==A75_INPUT_RESET) {
        if (e->type==A75_INPUT_RESET) g.clear_input(); else g.sleep(e->type==A75_PAUSE);
        s.pause_held=false; s.previous=-1; return;
    }
    if ((e->type!=A75_KEY_DOWN && e->type!=A75_KEY_UP) || !e->code || g.sleeping) return;
    const bool down=e->type==A75_KEY_DOWN;
    if (!std::strcmp(e->code,"Enter")) {
        if (down && !s.pause_held) g.pause(!g.paused);
        s.pause_held=down; s.previous=-1; return;
    }
    if (g.paused) return;
    if (!std::strcmp(e->code,"ArrowLeft")) g.left=down;
    if (!std::strcmp(e->code,"ArrowRight")) g.right=down;
    if (!std::strcmp(e->code,"Space")) g.jump(down);
}
int32_t __cdecl render(void *instance, const A75Frame *frame) {
    if (!instance || !frame || frame->size<sizeof(*frame) || !frame->target || !frame->width || !frame->height) return -2;
    auto &s=*static_cast<Scene*>(instance);
    if (frame->width>16384 || frame->height>16384) return -2;
    s.w=frame->width; s.h=frame->height;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    frame->target->GetDesc(&view);
    s.rgba = view.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!s.rgba && view.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return -2;
    if (!s.clear_context) {
        try { s.pixels.resize(size_t(s.w)*s.h); } catch (...) { return -2; }
    }
    s.game.step(s.previous<0 ? 0 : float(frame->seconds-s.previous));
    s.target = frame->target;
    s.draw();
    s.target = nullptr;
    if (!s.clear_context) {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        frame->target->GetResource(&resource);
        if (!resource) return -2;
        s.context->UpdateSubresource(resource.Get(), 0, nullptr, s.pixels.data(), s.w*4, 0);
    }
    const auto &g=s.game;
    const bool moving=(g.left && !g.right && g.x>g.radius) || (g.right && !g.left && g.x<g.arena_width-g.radius);
    const bool active=!g.paused && !g.sleeping && (moving || g.vy!=0 || g.y<g.floor_y-g.radius || g.immunity>0);
    s.previous=active ? frame->seconds : -1;
    return active ? 0 : -1;
}
void __cdecl destroy(void *instance) {
    delete static_cast<Scene*>(instance);
}
}
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
    static const A75Wallpaper api={sizeof(A75Wallpaper),ACTION75_WALLPAPER_ABI,create,event,render,destroy};
    return abi==api.abi ? &api : nullptr;
}

