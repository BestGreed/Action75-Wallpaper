#include "Action75Wallpaper12.h"
#include "game.hpp"
#include "appearance.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
using Pixel = uint32_t;
struct Scene {
    key_game::Game game;
    ID3D12GraphicsCommandList *commands = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE target{};
    // The RTX 4060 driver can terminate the worker if a clear has more than 32 rectangles.
    std::array<D3D12_RECT, 32> rectangles{};
    UINT pending = 0;
    Pixel batchColor = 0;
    unsigned w = 0, h = 0;
    bool amber = false, pause_held = false;
    double previous = -1;
    enum Station {Sky,Building,Window,Track,Floor,Dash,StationCount};
    enum Object {Signal,Coin,Glint,Hazard,Shield,Visor,Boots,Digits,ObjectCount};
    static constexpr int buildingCount = 26;
    static constexpr int dashCount = 31;
    struct StationRectangle { D3D12_RECT bounds; Station color; };
    // Up to eight windows and one facade per building, plus track, floor and dashes.
    std::array<StationRectangle, buildingCount * 9 + 2 + dashCount> stationGeometry{};
    size_t stationRectangles = 0;
    static constexpr Pixel stationColors[][StationCount]={
        {0xff101b2b,0xff1c3040,0xff30505a,0xff86efce,0xff273646,0xff788881},
        {0xff332238,0xff533346,0xffb77663,0xfff3ad7d,0xff4a3546,0xffab8981},
        {0xff293849,0xff3c5368,0xff7196aa,0xffb3dfed,0xff394858,0xff899caa}
    };
    static constexpr Pixel objectColors[][ObjectCount]={
        {0xff86efce,0xffffc256,0xffffefd2,0xffff7268,0xffc2bae7,0xff182f42,0xffe2efdb,0xffdcefeb},
        {0xff68ddd8,0xffffda6b,0xfffeefc2,0xfffa56b0,0xff9a82ff,0xff202650,0xffc9eeff,0xffe5eeff},
        {0xfff6b674,0xffffe0a0,0xffffffd1,0xffed6850,0xffe0ab90,0xff3f2924,0xfff2e3c4,0xfff7ead2}
    };
    Pixel station[StationCount]{};
    Pixel composites[ObjectCount][StationCount]{};
    SurfaceLook stationLook,runnerLook;
    int stationStyle=0,runnerStyle=0;
    double backgroundOpacity = 1, foregroundOpacity = 1;
    bool blendWithStation = false;
    static Pixel adjust(Pixel color,const SurfaceLook& look){
        if(look.neutral())return color;
        auto rgb=look.transform({float((color>>16)&255),float((color>>8)&255),float(color&255)});
        auto channel=[](float v){return Pixel(std::lround(std::clamp(v,0.f,255.f)));};
        return 0xff000000u | (channel(rgb[0])<<16) | (channel(rgb[1])<<8) | channel(rgb[2]);
    }
    static Pixel premultiplied(Pixel front, double frontWeight, Pixel back = 0, double backWeight = 0) {
        const auto channel = [=](unsigned shift) {
            const auto value = ((front >> shift) & 255) * frontWeight + ((back >> shift) & 255) * backWeight;
            return Pixel(std::lround(value)) << shift;
        };
        const auto alpha = Pixel(std::lround(255 * (frontWeight + backWeight)));
        return (alpha << 24) | channel(16) | channel(8) | channel(0);
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
            const auto opacity = [&](const char* id, double& value) {
                const auto setting = j.find(id);
                if (setting != j.end() && setting->is_number())
                    value = std::clamp(setting->get<double>(), 0., 100.) / 100;
            };
            opacity("backgroundOpacity", backgroundOpacity);
            opacity("foregroundOpacity", foregroundOpacity);
        }
        Pixel base[StationCount]{};
        for (size_t i = 0; i < StationCount; ++i) {
            base[i] = adjust(amber && i == Track ? 0xffffbb59 : stationColors[stationStyle][i], stationLook);
            station[i] = premultiplied(base[i], backgroundOpacity);
        }
        const double visibleStation = backgroundOpacity * (1 - foregroundOpacity);
        blendWithStation = visibleStation > 0 && foregroundOpacity > 0;
        for (size_t i = 0; i < ObjectCount; ++i) {
            const auto front = adjust(amber && i == Signal ? 0xffffbb59 : objectColors[runnerStyle][i], runnerLook);
            for (size_t j = 0; j < StationCount; ++j)
                composites[i][j] = premultiplied(front, foregroundOpacity, base[j], visibleStation);
        }
    }
    void flushRectangles() {
        if (!pending)
            return;
        const float channels[] = {
            float((batchColor >> 16) & 255) / 255, float((batchColor >> 8) & 255) / 255,
            float(batchColor & 255) / 255, float(batchColor >> 24) / 255
        };
        commands->ClearRenderTargetView(target, channels, pending, rectangles.data());
        pending = 0;
    }
    D3D12_RECT bounds(float x, float y, float width, float height) const {
        const int x0 = std::clamp(int(x*w/960), 0, int(w));
        const int x1 = std::clamp(int((x+width)*w/960), x0, int(w));
        const int y0 = std::clamp(int(y*h/360), 0, int(h));
        const int y1 = std::clamp(int((y+height)*h/360), y0, int(h));
        return {x0, y0, x1, y1};
    }
    static bool hasArea(const D3D12_RECT& area) {
        return area.left < area.right && area.top < area.bottom;
    }
    void queue(const D3D12_RECT& area, Pixel color) {
        if (color != batchColor || pending == rectangles.size())
            flushRectangles();
        batchColor = color;
        rectangles[pending++] = area;
    }
    void objectRect(float x, float y, float width, float height, Object color) {
        const auto area = bounds(x, y, width, height);
        if (!hasArea(area)) return;
        queue(area, composites[color][Sky]);
        if (!blendWithStation) return;
        // Shade each intersection against the original station layer. A later
        // object or window wins without accumulating opacity on overlaps.
        for (size_t i = 0; i < stationRectangles; ++i) {
            const auto& cell = stationGeometry[i];
            const D3D12_RECT overlap{
                std::max(area.left, cell.bounds.left), std::max(area.top, cell.bounds.top),
                std::min(area.right, cell.bounds.right), std::min(area.bottom, cell.bounds.bottom)
            };
            if (hasArea(overlap)) queue(overlap, composites[color][cell.color]);
        }
    }
    void number(unsigned value, float x, float y) {
        static constexpr unsigned masks[] = {63,6,91,79,102,109,125,7,127,111};
        static constexpr int segments[][4] = {{2,0,8,2},{10,2,2,7},{10,11,2,7},{2,18,8,2},{0,11,2,7},{0,2,2,7},{2,9,8,2}};
        do {
            const unsigned mask = masks[value%10];
            for (int i=0;i<7;++i) if (mask & (1u<<i))
                objectRect(x+segments[i][0],y+segments[i][1],segments[i][2],segments[i][3],Digits);
            x-=16;
            value/=10;
        } while (value);
    }
    void resize(unsigned width, unsigned height) {
        if (w == width && h == height) return;
        w = width;
        h = height;
        stationRectangles = 0;
        const auto add = [&](float x, float y, float boxWidth, float boxHeight, Station color) {
            const auto area = bounds(x, y, boxWidth, boxHeight);
            if (hasArea(area)) stationGeometry[stationRectangles++] = {area, color};
        };
        // Draw the disjoint facades in one group and then their windows.
        const auto heightOf = [](int index) { return 24 + (index * 43) % 86; };
        for (int i = 0; i < buildingCount; ++i) {
            const int height = heightOf(i);
            add(i * 38, 255 - height, 32, height, Building);
        }
        for (int i = 0; i < buildingCount; ++i) {
            const int height = heightOf(i);
            for (int j = 7; j < height - 6; j += 13)
                add(i * 38 + 6, 255 - height + j, 5, 3, Window);
        }
        add(0,270,960,7,Track); add(0,277,960,83,Floor);
        for (int i=0;i<dashCount;++i) add(i*32,286,14,3,Dash);
    }
    void drawObjects() {
        objectRect(game.coin_x-7,240,14,14,Coin); objectRect(game.coin_x-2,243,4,8,Glint);
        for (int i=0;i<19;++i) objectRect(game.hazard_x-(i+1)*.7f,251+i,(i+1)*1.4f,1,Hazard);
        const Object suit = game.immunity>0 ? Shield : Signal;
        objectRect(game.x-11,game.y-14,22,18,suit); objectRect(game.x-6,game.y-10,17,6,Visor);
        objectRect(game.x-10,game.y+4,20,5,Boots); objectRect(game.x-9,game.y+9,6,4,suit);
        objectRect(game.x+4,game.y+9,6,4,suit);
        objectRect(24,24,9,9,Coin); number(game.score,91,20);
        objectRect(128,24,9,9,Hazard); number(game.hits,195,20);
        if (game.paused || game.sleeping) {
            objectRect(454,137,14,50,Signal);
            objectRect(486,137,14,50,Signal);
        }
    }
    void draw() {
        queue({0, 0, LONG(w), LONG(h)}, station[Sky]);
        if (backgroundOpacity > 0) {
            for (size_t i = 0; i < stationRectangles; ++i) {
                const auto& cell = stationGeometry[i];
                queue(cell.bounds, station[cell.color]);
            }
        }
        if (foregroundOpacity > 0) drawObjects();
        flushRectangles();
    }
};

void * __cdecl create(const A75Create12 *cfg) {
    if (!cfg || cfg->size<sizeof(*cfg) || !cfg->device) return nullptr;
    try {
        auto scene = std::make_unique<Scene>();
        scene->settings(cfg->settings_json);
        return scene.release();
    } catch (...) {
        return nullptr;
    }
}
void __cdecl event(void *instance, const A75Event *e) {
    if (!instance || !e || e->size<sizeof(*e)) return;
    Scene &s = *static_cast<Scene*>(instance);
    auto &g = s.game;
    if (e->type==A75_SETTINGS) {
        try { s.settings(e->settings_json); } catch (...) {}
        return;
    }
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
int32_t __cdecl render(void *instance, const A75Frame12 *frame) {
    if (!instance || !frame || frame->size<sizeof(*frame) || !frame->commands || !frame->rtv.ptr ||
        !frame->width || !frame->height) return -2;
    auto &s=*static_cast<Scene*>(instance);
    if (frame->width>16384 || frame->height>16384) return -2;
    s.resize(frame->width, frame->height);
    s.game.step(s.previous<0 ? 0 : float(frame->seconds-s.previous));
    s.commands = frame->commands;
    s.target = frame->rtv;
    s.draw();
    s.commands = nullptr;
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
extern "C" const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 api={sizeof(A75Wallpaper12),ACTION75_WALLPAPER_ABI12,
                                    create,event,render,destroy,nullptr};
    return abi==api.abi ? &api : nullptr;
}

