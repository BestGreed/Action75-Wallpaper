#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace author_particles {
// The stock renderer's additive nodes can accumulate RGB above their Max alpha.
// Resolve a complete draw to a bounded premultiplied layer before presenting it
// to ABI2. The original fully opaque route never uses this layer.
struct OpacityLayer {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> target;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap,srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    unsigned width=0,height=0;
    static void check(HRESULT result,const char* what) {if(FAILED(result)) throw std::runtime_error(what);}
    void initialize(ID3D12Device* supplied) {device=supplied;}
    void ensure(unsigned w,unsigned h) {
        if(target && w==width && h==height) return;
        if(!root) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=1;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvHeap)),"Particle opacity RTV heap");
            hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&srvHeap)),"Particle opacity SRV heap");
            D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
            D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[0].DescriptorTable={1,&range};params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,5};params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
            Microsoft::WRL::ComPtr<ID3DBlob> serialized,error,vs,ps;
            check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"Particle opacity root serialization");
            check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"Particle opacity root");
            constexpr char shader[]=R"(
Texture2D<float4> layer:register(t0);
cbuffer Parameters:register(b0) {float4 background;float opacity;};
float4 VS(uint id:SV_VertexID):SV_Position {
    return float4(id==2?3:-1,id==1?3:-1,0,1);
}
float4 PS(float4 position:SV_Position):SV_Target {
    float4 effect=layer.Load(int3(int2(position.xy),0));
    float alpha=max(effect.a,max(effect.r,max(effect.g,effect.b)))*opacity;
    return float4(effect.rgb*opacity+background.rgb*(1-alpha),alpha+background.a*(1-alpha));
}
)";
            check(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"VS","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error),"Particle opacity vertex shader");
            check(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"PS","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error),"Particle opacity pixel shader");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.Get();
            p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
            p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;
            p.SampleMask=UINT_MAX;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            p.NumRenderTargets=1;p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;p.SampleDesc.Count=1;
            check(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)),"Particle opacity pipeline");
        }
        width=w;height=h;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear{};clear.Format=d.Format;
        target.Reset();check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&target)),"Particle persistent opacity target");
        device->CreateRenderTargetView(target.Get(),nullptr,rtvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=d.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(target.Get(),&view,srvHeap->GetCPUDescriptorHandleForHeapStart());
    }
    D3D12_RESOURCE_BARRIER barrier(D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=target.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=from;b.Transition.StateAfter=to;return b;
    }
    void begin(ID3D12GraphicsCommandList* list,unsigned w,unsigned h) {
        ensure(w,h);auto b=barrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);list->ResourceBarrier(1,&b);
        const auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();const float transparent[4]{};
        list->ClearRenderTargetView(rtv,transparent,0,nullptr);list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    }
    void composite(const A75Frame12& frame,const std::array<float,4>& background,float opacity) {
        auto* list=frame.commands;auto b=barrier(D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        list->OMSetRenderTargets(1,&frame.rtv,FALSE,nullptr);list->SetPipelineState(pipeline.Get());list->SetGraphicsRootSignature(root.Get());
        ID3D12DescriptorHeap* heaps[]{srvHeap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootDescriptorTable(0,srvHeap->GetGPUDescriptorHandleForHeapStart());
        const float values[]{background[0],background[1],background[2],background[3],opacity};list->SetGraphicsRoot32BitConstants(1,5,values,0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    }
};
}
