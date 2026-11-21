#include "Action75Wallpaper12.h"

#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Data::Json;

constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr size_t maxBursts = 8;
constexpr UINT sparksPerBurst = 36; // Also used by SparkVS.
constexpr UINT skyWidthLimit = 1280;
constexpr UINT skyHeightLimit = 720;

struct Background {
    float mode = 0;
    float brightness = 1;
    float saturation = 1;
    float hue = 0;
    std::array<float, 3> tint{1, 1, 1};
    float disk = 1;

    bool operator==(const Background&) const = default;
};

struct Settings {
    Background background;
    float brightness = 1;
    float radius = 1;
    float duration = 0.83f;
    std::array<float, 3> tint{1, 1, 1};
};

struct RootConstants {
    float aspect;
    float mode;
    float skyBrightness;
    float skySaturation;
    std::array<float, 3> skyTint;
    float hueCos;
    float hueSin;
    float diskBrightness;
    float novaBrightness;
    float novaRadius;
    std::array<float, 3> novaTint;
    float reserved = 0;
};
static_assert(sizeof(RootConstants) == 64);

struct Burst {
    double born;
    float x;
    float y;
    float seed;
};

struct BurstGpu {
    float x;
    float y;
    float age;
    float seed;
};
static_assert(sizeof(BurstGpu) == 16);

class Apartment {
public:
    Apartment() : result_(RoInitialize(RO_INIT_MULTITHREADED)) {
        if (result_ != RPC_E_CHANGED_MODE) {
            winrt::check_hresult(result_);
        }
    }
    ~Apartment() {
        if (SUCCEEDED(result_)) {
            RoUninitialize();
        }
    }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

private:
    HRESULT result_;
};

float number(const JsonObject& object, const wchar_t* key, float current, float lo, float hi) {
    const auto value = object.TryLookup(key);
    if (!value || value.ValueType() != JsonValueType::Number || !std::isfinite(value.GetNumber())) {
        return current;
    }
    return static_cast<float>(std::clamp(value.GetNumber(), double{lo}, double{hi}));
}

std::array<float, 3> color(const JsonObject& object, const wchar_t* key, std::array<float, 3> current) {
    const auto value = object.TryLookup(key);
    if (!value || value.ValueType() != JsonValueType::Array) {
        return current;
    }
    const auto array = value.GetArray();
    if (array.Size() != 3) {
        return current;
    }
    std::array<float, 3> result{};
    for (uint32_t channel = 0; channel < array.Size(); ++channel) {
        const auto item = array.GetAt(channel);
        if (item.ValueType() != JsonValueType::Number || !std::isfinite(item.GetNumber())) {
            return current;
        }
        result[channel] = static_cast<float>(std::clamp(item.GetNumber(), 0.0, 255.0) / 255.0);
    }
    return result;
}

void readSettings(Settings& settings, const char* source) {
    if (!source || !*source) {
        return;
    }
    const Apartment apartment;
    JsonObject object{nullptr};
    if (!JsonObject::TryParse(winrt::to_hstring(source), object)) {
        return;
    }
    auto& bg = settings.background;
    bg.mode = number(object, L"skyMode", bg.mode, 0, 1) >= 0.5f ? 1.0f : 0.0f;
    bg.brightness = number(object, L"skyBrightness", bg.brightness, 0, 1.65f);
    bg.hue = number(object, L"skyHue", bg.hue, 0, 360);
    bg.saturation = number(object, L"skySaturation", bg.saturation, 0, 1.5f);
    bg.tint = color(object, L"skyColor", bg.tint);
    bg.disk = number(object, L"diskBrightness", bg.disk, 0, 1.6f);
    settings.brightness = number(object, L"novaBrightness", settings.brightness, 0, 1.6f);
    settings.radius = number(object, L"novaRadius", settings.radius, 0.55f, 1.45f);
    settings.duration = number(object, L"novaDuration", settings.duration, 0.45f, 1.35f);
    settings.tint = color(object, L"novaColor", settings.tint);
}

std::vector<char> shader(const std::filesystem::path& pack, const wchar_t* file) {
    std::ifstream stream(pack / L"shaders" / file, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length <= 0 || length > 1024 * 1024) {
        throw std::runtime_error("Invalid shader file");
    }
    std::vector<char> bytes(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(bytes.data(), length)) {
        throw std::runtime_error("Cannot read shader file");
    }
    return bytes;
}

ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE texture{};
    texture.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture.NumDescriptors = 1;
    texture.BaseShaderRegister = 1;
    texture.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(RootConstants) / sizeof(uint32_t);
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &texture};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized;
    winrt::check_hresult(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                    &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    winrt::check_hresult(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                     serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    return root;
}

ComPtr<ID3D12PipelineState> pipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                    const std::vector<char>& vertex, const std::vector<char>& pixel,
                                    bool additive) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vertex.data(), vertex.size()};
    desc.PS = {pixel.data(), pixel.size()};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = additive;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = additive ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = additive
        ? D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE
        : D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = colorFormat;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> state;
    winrt::check_hresult(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&state)));
    return state;
}

ComPtr<ID3D12DescriptorHeap> descriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                           D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type;
    desc.NumDescriptors = 1;
    desc.Flags = flags;
    ComPtr<ID3D12DescriptorHeap> heap;
    winrt::check_hresult(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
    return heap;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}

void target(ID3D12GraphicsCommandList* commands, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height) {
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
}

class Wallpaper {
public:
    explicit Wallpaper(const A75Create12& init) {
        readSettings(settings_, init.settings_json);
        auto* device = init.device;
        const std::filesystem::path pack(init.pack_dir);
        root_ = rootSignature(device);
        const auto screen = shader(pack, L"ScreenVS.cso");
        skyPipeline_ = pipeline(device, root_.Get(), screen, shader(pack, L"SkyPS.cso"), false);
        copyPipeline_ = pipeline(device, root_.Get(), screen, shader(pack, L"CopyPS.cso"), false);
        novaPipeline_ = pipeline(device, root_.Get(), shader(pack, L"NovaVS.cso"), shader(pack, L"NovaPS.cso"), true);
        sparkPipeline_ = pipeline(device, root_.Get(), shader(pack, L"SparkVS.cso"), shader(pack, L"SparkPS.cso"), true);
        createSky(device, init.width, init.height);
        createUpload(device);
    }

    ~Wallpaper() {
        if (mapped_) {
            upload_->Unmap(0, nullptr);
        }
    }

    void event(const A75Event& event) {
        switch (event.type) {
        case A75_KEY_DOWN:
            if (!paused_ && std::isfinite(event.seconds) &&
                std::isfinite(event.x) && std::isfinite(event.y) &&
                event.x >= 0 && event.x <= 1 && event.y >= 0 && event.y <= 1) {
                trigger(event);
            }
            break;
        case A75_SETTINGS: {
            auto updated = settings_;
            readSettings(updated, event.settings_json);
            skyDirty_ |= updated.background != settings_.background;
            settings_ = updated;
            break;
        }
        case A75_PAUSE:
            paused_ = true;
            [[fallthrough]];
        case A75_INPUT_RESET:
            count_ = 0;
            break;
        case A75_RESUME:
            paused_ = false;
            break;
        }
    }

    int32_t render(const A75Frame12& frame) {
        const auto size = static_cast<float>(frame.width) / static_cast<float>(frame.height);
        const auto& bg = settings_.background;
        const float hue = bg.hue * (std::numbers::pi_v<float> / 180.0f);
        const RootConstants constants{size, bg.mode, bg.brightness, bg.saturation, bg.tint, std::cos(hue),
                                      std::sin(hue), bg.disk, settings_.brightness, settings_.radius,
                                      settings_.tint};
        auto* commands = frame.commands;
        commands->SetGraphicsRootSignature(root_.Get());
        commands->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (skyDirty_ || aspect_ != size) {
            drawSky(commands);
            skyDirty_ = false;
            aspect_ = size;
        }

        target(commands, frame.rtv, frame.width, frame.height);
        commands->SetPipelineState(copyPipeline_.Get());
        ID3D12DescriptorHeap* heaps[]{skySrv_.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootDescriptorTable(2, skySrv_->GetGPUDescriptorHandleForHeapStart());
        commands->DrawInstanced(3, 1, 0, 0);

        const auto active = prepareBursts(frame.seconds);
        if (!active || paused_ || settings_.brightness == 0) {
            if (settings_.brightness == 0) {
                count_ = 0;
            }
            return -1;
        }

        commands->SetGraphicsRootShaderResourceView(1, upload_->GetGPUVirtualAddress());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        commands->SetPipelineState(novaPipeline_.Get());
        commands->DrawInstanced(4, active, 0, 0);
        commands->SetPipelineState(sparkPipeline_.Get());
        commands->DrawInstanced(4, active * sparksPerBurst, 0, 0);
        return 0;
    }

private:
    void createSky(ID3D12Device* device, UINT width, UINT height) {
        const double scale = std::min({1.0, double{skyWidthLimit} / width, double{skyHeightLimit} / height});
        skyWidth_ = std::max(1u, static_cast<UINT>(std::lround(width * scale)));
        skyHeight_ = std::max(1u, static_cast<UINT>(std::lround(height * scale)));
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = skyWidth_;
        desc.Height = skyHeight_;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = colorFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        winrt::check_hresult(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&sky_)));
        skyRtv_ = descriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        skySrv_ = descriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                 D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        device->CreateRenderTargetView(sky_.Get(), nullptr, skyRtv_->GetCPUDescriptorHandleForHeapStart());
        device->CreateShaderResourceView(sky_.Get(), nullptr, skySrv_->GetCPUDescriptorHandleForHeapStart());
    }

    void createUpload(ID3D12Device* device) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = maxBursts * sizeof(BurstGpu);
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        winrt::check_hresult(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload_)));
        const D3D12_RANGE readRange{0, 0};
        void* data = nullptr;
        winrt::check_hresult(upload_->Map(0, &readRange, &data));
        mapped_ = static_cast<BurstGpu*>(data);
    }

    void drawSky(ID3D12GraphicsCommandList* commands) {
        transition(commands, sky_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        target(commands, skyRtv_->GetCPUDescriptorHandleForHeapStart(), skyWidth_, skyHeight_);
        commands->SetPipelineState(skyPipeline_.Get());
        commands->DrawInstanced(3, 1, 0, 0);
        transition(commands, sky_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void trigger(const A75Event& event) {
        if (settings_.brightness == 0) {
            return;
        }
        const auto oldest = std::min_element(bursts_.begin(), bursts_.begin() + count_,
            [](const Burst& lhs, const Burst& rhs) { return lhs.born < rhs.born; });
        const size_t slot = count_ == maxBursts ? size_t(oldest - bursts_.begin()) : count_++;
        sequence_ = sequence_ * 1664525u + 1013904223u;
        const float seed = static_cast<float>(sequence_ >> 16) * (2.0f * std::numbers::pi_v<float> / 65536.0f);
        bursts_[slot] = {event.seconds, event.x, event.y, seed};
    }

    UINT prepareBursts(double seconds) {
        UINT active = 0;
        for (size_t index = 0; index < count_; ++index) {
            const Burst burst = bursts_[index];
            const double age = std::max(0.0, seconds - burst.born) / settings_.duration;
            if (age < 1.0) {
                bursts_[active] = burst;
                mapped_[active] = {burst.x, burst.y, static_cast<float>(age), burst.seed};
                ++active;
            }
        }
        count_ = active;
        return active;
    }

    Settings settings_;
    std::array<Burst, maxBursts> bursts_{};
    size_t count_ = 0;
    uint32_t sequence_ = 75;
    bool paused_ = false;
    bool skyDirty_ = true;
    float aspect_ = 0;
    UINT skyWidth_ = 0;
    UINT skyHeight_ = 0;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> skyPipeline_;
    ComPtr<ID3D12PipelineState> copyPipeline_;
    ComPtr<ID3D12PipelineState> novaPipeline_;
    ComPtr<ID3D12PipelineState> sparkPipeline_;
    ComPtr<ID3D12Resource> sky_;
    ComPtr<ID3D12Resource> upload_;
    ComPtr<ID3D12DescriptorHeap> skyRtv_;
    ComPtr<ID3D12DescriptorHeap> skySrv_;
    BurstGpu* mapped_ = nullptr;
};

void* __cdecl create(const A75Create12* init) noexcept {
    if (!init || init->size < sizeof(*init) || !init->device || !init->pack_dir ||
        !init->width || !init->height) {
        return nullptr;
    }
    try {
        return new Wallpaper(*init);
    } catch (...) {
        return nullptr;
    }
}

void __cdecl event(void* instance, const A75Event* input) noexcept {
    if (instance && input && input->size >= sizeof(*input)) {
        try {
            static_cast<Wallpaper*>(instance)->event(*input);
        } catch (...) {
            // Invalid settings or an unavailable JSON parser leave the previous settings intact.
        }
    }
}

int32_t __cdecl render(void* instance, const A75Frame12* frame) noexcept {
    if (!instance || !frame || frame->size < sizeof(*frame) || !frame->commands ||
        !frame->width || !frame->height || !std::isfinite(frame->seconds)) {
        return -2;
    }
    return static_cast<Wallpaper*>(instance)->render(*frame);
}

void __cdecl destroy(void* instance) noexcept {
    delete static_cast<Wallpaper*>(instance);
}
} // namespace

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 wallpaper{sizeof(A75Wallpaper12), ACTION75_WALLPAPER_ABI12,
                                         create, event, render, destroy, nullptr};
    return abi == ACTION75_WALLPAPER_ABI12 ? &wallpaper : nullptr;
}
