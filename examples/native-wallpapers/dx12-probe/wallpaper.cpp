#include "Action75Wallpaper12.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace {
struct Probe {
    float x = 0.5f, y = 0.5f;
    double litUntil = -1.0;
};

void* __cdecl create(const A75Create12* host) noexcept {
    if (!host || host->size < sizeof(*host) || !host->device)
        return nullptr;
    return new (std::nothrow) Probe;
}

void __cdecl event(void* instance, const A75Event* e) noexcept {
    if (!instance || !e || e->size < sizeof(*e) || e->type != A75_KEY_DOWN)
        return;
    if (!std::isfinite(e->x) || !std::isfinite(e->y) || !std::isfinite(e->seconds))
        return;
    auto& probe = *static_cast<Probe*>(instance);
    probe.x = std::clamp(e->x, 0.0f, 1.0f);
    probe.y = std::clamp(e->y, 0.0f, 1.0f);
    probe.litUntil = e->seconds + 0.45;
}

int32_t __cdecl render(void* instance, const A75Frame12* frame) noexcept {
    if (!instance || !frame || frame->size < sizeof(*frame) || !frame->commands ||
        !frame->rtv.ptr || !frame->width || !frame->height)
        return -2;

    constexpr float background[] = {0.02f, 0.07f, 0.12f, 1.0f};
    frame->commands->ClearRenderTargetView(frame->rtv, background, 0, nullptr);
    const auto& probe = *static_cast<const Probe*>(instance);
    if (frame->seconds >= probe.litUntil)
        return -1;

    constexpr LONG halfSize = 44;
    const LONG x = static_cast<LONG>(probe.x * frame->width);
    const LONG y = static_cast<LONG>(probe.y * frame->height);
    const D3D12_RECT rect{
        std::max(0L, x - halfSize), std::max(0L, y - halfSize),
        std::min(static_cast<LONG>(frame->width), x + halfSize),
        std::min(static_cast<LONG>(frame->height), y + halfSize)};
    constexpr float highlight[] = {0.03f, 0.90f, 0.68f, 1.0f};
    frame->commands->ClearRenderTargetView(frame->rtv, highlight, 1, &rect);
    return static_cast<int32_t>((probe.litUntil - frame->seconds) * 1000) + 1;
}

void __cdecl destroy(void* instance) noexcept {
    delete static_cast<Probe*>(instance);
}

const A75Wallpaper12 api{sizeof(A75Wallpaper12), ACTION75_WALLPAPER_ABI12,
                         create, event, render, destroy, nullptr};
}

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
    return abi == ACTION75_WALLPAPER_ABI12 ? &api : nullptr;
}
