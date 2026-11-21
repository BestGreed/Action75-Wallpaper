#include "Action75Wallpaper12.h"
#include <nlohmann/json.hpp>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
struct Beacon {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<float,3> tint{74.f/255,224.f/255,189.f/255};
    float strength=.85f, x=.5f, y=.5f;
    double last=-1;
    bool paused=false;

    void configure(const char *text) {
        if (!text) return;
        auto values=nlohmann::json::parse(text);
        if (values.contains("signal_rgb")) {
            auto rgb=values.at("signal_rgb").get<std::array<int,3>>();
            for (unsigned i=0;i<rgb.size();++i) tint[i]=std::clamp(rgb[i],0,255)/255.f;
        }
        strength=std::clamp(values.value("radiance",strength),.15f,1.f);
    }
    bool initialize(const A75Create12 &c) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants={0,0,10};
        param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{1,&param,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr)) ||
            FAILED(c.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))) return false;
        auto shader=[&](const wchar_t *name) {
            std::ifstream file(std::filesystem::path(c.pack_dir)/L"shaders"/name,std::ios::binary);
            return std::vector<char>(std::istreambuf_iterator<char>(file),{});
        };
        const auto vs=shader(L"tide-vs.cso"),ps=shader(L"tide-ps.cso");
        if (vs.empty() || ps.empty()) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature=root.Get();
        state.VS={vs.data(),vs.size()}; state.PS={ps.data(),ps.size()};
        state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable=TRUE;
        auto &blend=state.BlendState.RenderTarget[0];
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        state.SampleMask=UINT_MAX;
        state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets=1; state.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        state.SampleDesc.Count=1;
        configure(c.settings_json);
        return SUCCEEDED(c.device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&pipeline)));
    }
};

void *__cdecl create(const A75Create12 *c) noexcept {
    try {
        if (!c || c->size<sizeof(*c) || !c->device || !c->pack_dir) return nullptr;
        auto instance=std::make_unique<Beacon>();
        return instance->initialize(*c) ? instance.release() : nullptr;
    } catch (...) { return nullptr; }
}
void __cdecl event(void *ptr,const A75Event *e) noexcept {
    if (!ptr || !e || e->size<sizeof(*e)) return;
    auto &b=*static_cast<Beacon*>(ptr);
    switch(e->type) {
    case A75_KEY_DOWN:
        if (!b.paused) { b.x=e->x; b.y=e->y; b.last=e->seconds; }
        break;
    case A75_SETTINGS: try { b.configure(e->settings_json); } catch (...) {} break;
    case A75_PAUSE: b.paused=true; b.last=-1; break;
    case A75_RESUME: b.paused=false; break;
    case A75_INPUT_RESET: b.last=-1; break;
    }
}
int32_t __cdecl render(void *ptr,const A75Frame12 *f) noexcept {
    if (!ptr || !f || f->size<sizeof(*f) || !f->commands || !f->width || !f->height) return -2;
    auto &b=*static_cast<Beacon*>(ptr);
    const float age=b.last<0 ? 0 : float(std::max(0.,f->seconds-b.last));
    const bool lit=!b.paused && b.last>=0 && age<2.05f;
    const float constants[]={float(f->width),float(f->height),b.x,b.y,b.tint[0],b.tint[1],b.tint[2],
                             b.strength,age,lit?1.f:0.f};
    f->commands->SetGraphicsRootSignature(b.root.Get());
    f->commands->SetPipelineState(b.pipeline.Get());
    f->commands->SetGraphicsRoot32BitConstants(0,10,constants,0);
    f->commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    f->commands->DrawInstanced(3,1,0,0);
    return lit?0:-1;
}
void __cdecl destroy(void *ptr) noexcept { delete static_cast<Beacon*>(ptr); }
}
extern "C" const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 api{sizeof(api),ACTION75_WALLPAPER_ABI12,create,event,render,destroy,nullptr};
    return abi==api.abi ? &api : nullptr;
}
