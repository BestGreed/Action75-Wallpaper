#include "EffekseerBackend.h"
#include "NativeRenderer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;

// An offscreen WARP diagnostic: no HWND, no input hooks, no monitor changes.
// The command-line asset must be a vendor diagnostic, never labelled FM content.
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: action75_effekseer_probe vendor-effect.efkefc\n");
        return 2;
    }
    constexpr UINT width = 256;
    constexpr UINT height = 128;
    action75::NativeRenderer nativeRenderer;
    std::string error;
    if (!nativeRenderer.initialize(nullptr, width, height, true, error)) {
        std::fprintf(stderr, "NativeRenderer WARP initialization failed: %s\n", error.c_str());
        return 3;
    }
    auto* device = nativeRenderer.device();
    auto* context = nativeRenderer.context();
    nativeRenderer.beginFrame();
    D3D11_TEXTURE2D_DESC desc{};
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Resource> targetResource;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return 4;
    rtv->GetResource(&targetResource);
    if (FAILED(targetResource.As(&target))) return 4;
    target->GetDesc(&desc);

    action75::EffekseerBackend backend;
    if (!backend.Initialize(device, context, 2048)) {
        std::fprintf(stderr, "backend init: %s\n", backend.LastError().c_str());
        return 5;
    }
    backend.Resize(width, height);
    const int effectId = backend.LoadEffect(std::filesystem::path(argv[1]));
    if (effectId < 1) {
        std::fprintf(stderr, "effect load: %s\n", backend.LastError().c_str());
        return 6;
    }
    if (backend.LoadEffect(std::filesystem::path(argv[1])) != effectId) return 7;
    constexpr float spawnX = 75.0f;
    constexpr float spawnY = 43.0f;
    if (backend.PlayAt(effectId, spawnX, spawnY, 2.0f) < 0) return 8;
    if (!backend.HasActiveEffects()) return 9;
    auto missing = std::filesystem::path(argv[1]);
    missing += L".missing";
    if (backend.LoadEffect(missing) != -1 || backend.ActiveEffectCount() != 1) return 16;
    const auto anotherFile = std::filesystem::path(argv[1]).parent_path() / L"square_b.efkefc";
    if (std::filesystem::exists(anotherFile) && anotherFile != std::filesystem::path(argv[1])) {
        const int anotherId = backend.LoadEffect(anotherFile);
        if (anotherId < 1 || backend.PlayAt(anotherId, 180.0f, 80.0f) < 0) return 17;
        if (!backend.UnloadEffect(anotherId) || backend.ActiveEffectCount() != 1) return 18;
        if (backend.PlayAt(anotherId, 180.0f, 80.0f) != -1) return 19;
    }

    for (int frame = 0; frame < 10; ++frame) {
        nativeRenderer.beginFrame();
        backend.Update(1.0f / 60.0f);
        if (!backend.Draw()) return 10;
    }
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 11;
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 12;
    unsigned litPixels = 0;
    std::uint64_t sumX = 0;
    std::uint64_t sumY = 0;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (unsigned x = 0; x < width; ++x) {
            if (row[x * 4] || row[x * 4 + 1] || row[x * 4 + 2]) {
                ++litPixels;
                sumX += x;
                sumY += y;
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    const double centerX = litPixels ? double(sumX) / litPixels : 0.0;
    const double centerY = litPixels ? double(sumY) / litPixels : 0.0;
    std::printf("Effekseer 1.80.8 offscreen WARP: effectId=%d active=%zu particles=%d litPixels=%u center=(%.2f,%.2f)\n",
        effectId, backend.ActiveEffectCount(), backend.ActiveParticleCount(), litPixels, centerX, centerY);
    backend.StopAll();
    if (backend.HasActiveEffects()) return 13;
    if (!backend.UnloadEffect(effectId) || backend.PlayAt(effectId, spawnX, spawnY) != -1) return 20;
    backend.UnloadEffects();
    backend.Shutdown();
    if (!litPixels) {
        std::fprintf(stderr, "No diagnostic pixels were drawn.\n");
        return 14;
    }
    if (std::filesystem::path(argv[1]).stem().wstring().find(L"square_") == 0 &&
        (std::abs(centerX - spawnX) > 1.0 || std::abs(centerY - spawnY) > 1.0)) {
        std::fprintf(stderr, "Vendor square diagnostic was not centered at its requested top-left pixel coordinates.\n");
        return 15;
    }
    // Use a separate bake of the external vendor fixture. Test the real D3D11
    // render-to-texture cache path, not the visual fidelity of unpublished Paintball.
    if (!nativeRenderer.loadCanvasBackground(std::filesystem::path(argv[1]), error)) {
        std::fprintf(stderr, "Canvas bake: %s\n", error.c_str());
        return 21;
    }
    auto* canvas = nativeRenderer.background();
    if (!canvas || nativeRenderer.isAnimated()) return 22;
    ComPtr<ID3D11RenderTargetView> afterBake;
    context->OMGetRenderTargets(1, &afterBake, nullptr);
    if (afterBake.Get() != rtv.Get()) return 23;
    afterBake.Reset();

    // Read at the Canvas's full reference size: a small fixture sprite can
    // disappear under filtering when a 1920x720 texture is sampled at 256x128.
    constexpr unsigned canvasWidth = 1920, canvasHeight = 720;
    if (!nativeRenderer.resize(canvasWidth, canvasHeight, error) ||
        nativeRenderer.background() != canvas)
        return 26;
    nativeRenderer.beginFrame();
    target.Reset();
    targetResource.Reset();
    rtv.Reset();
    staging.Reset();
    context->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return 26;
    rtv->GetResource(&targetResource);
    if (FAILED(targetResource.As(&target))) return 26;
    target->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return 26;

    const auto sampleCanvas = [&](std::array<std::uint32_t, 49>& sample) {
        nativeRenderer.beginFrame();
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE read{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &read)))
            return false;
        std::size_t i = 0;
        for (unsigned y = canvasHeight / 2 - 3; y <= canvasHeight / 2 + 3; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(read.pData) + y * read.RowPitch;
            for (unsigned x = canvasWidth / 2 - 3; x <= canvasWidth / 2 + 3; ++x) {
                const auto* p = row + x * 4;
                sample[i++] = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                              (std::uint32_t(p[2]) << 16);
            }
        }
        context->Unmap(staging.Get(), 0);
        return true;
    };
    std::array<std::uint32_t, 49> first{}, second{};
    if (!sampleCanvas(first) ||
        std::none_of(first.begin(), first.end(), [](auto pixel) { return pixel != 0; }))
        return 24;
    if (nativeRenderer.loadCanvasBackground(missing, error) ||
        nativeRenderer.background() != canvas || !sampleCanvas(second) || first != second)
        return 25;
    if (!nativeRenderer.resize(width / 2, height / 2, error) ||
        nativeRenderer.background() != canvas || nativeRenderer.isAnimated())
        return 26;
    nativeRenderer.clearBackground();
    if (nativeRenderer.background() || nativeRenderer.isAnimated()) return 27;
    std::puts("Cached Canvas WARP diagnostic: visible pixels, stable reuse, rollback, resize, and clear.");
    return 0;
}
