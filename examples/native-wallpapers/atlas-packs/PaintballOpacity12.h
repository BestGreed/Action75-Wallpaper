#pragma once

#include "Graphics.h"
#include "PaintballOpacityShaders12.h"

namespace atlas_packs {

class PaintballOpacityPass {
public:
  PaintballOpacityPass(ID3D12Device* device, unsigned width, unsigned height)
      : width_(width), height_(height) {
    auto texture = textureDescription(width, height);
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> scratch;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&scratch)),
        "Allocate the Paintball foreground target");
    image_.attach(device, std::move(scratch));

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&views_)),
          "Allocate the Paintball foreground RTV");
    device->CreateRenderTargetView(image_.resource(), nullptr, views_->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> binary;
    check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &binary, nullptr),
          "Serialize the Paintball opacity root signature");
    check(device->CreateRootSignature(0, binary->GetBufferPointer(), binary->GetBufferSize(), IID_PPV_ARGS(&root_)),
          "Create the Paintball opacity root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_.Get();
    desc.VS = {kPaintballOpacityVS, sizeof(kPaintballOpacityVS)};
    desc.PS = {kPaintballOpacityPS, sizeof(kPaintballOpacityPS)};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace =
        {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    desc.BlendState.RenderTarget[0] = {
        TRUE, FALSE, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_)),
          "Create the Paintball opacity pipeline");
  }

  bool matches(const A75Frame12& frame) const { return frame.width == width_ && frame.height == height_; }

  A75Frame12 begin(const A75Frame12& frame) {
    // Preserve the authored blending against the backdrop, including additive flashes.
    auto* list = frame.commands;
    auto* scratch = image_.resource();
    transition(list, scratch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(list, frame.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(scratch, frame.target);
    transition(list, frame.target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list, scratch, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto foreground = frame;
    foreground.target = scratch;
    foreground.rtv = views_->GetCPUDescriptorHandleForHeapStart();
    return foreground;
  }

  void blend(const A75Frame12& frame, float opacity) {
    auto* list = frame.commands;
    transition(list, image_.resource(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    bindTarget(frame);
    ID3D12DescriptorHeap* descriptors[]{image_.heap()};
    list->SetDescriptorHeaps(1, descriptors);
    list->SetGraphicsRootSignature(root_.Get());
    list->SetGraphicsRootDescriptorTable(0, image_.heap()->GetGPUDescriptorHandleForHeapStart());
    list->SetPipelineState(pipeline_.Get());
    const float factor[]{opacity, opacity, opacity, opacity};
    list->OMSetBlendFactor(factor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
  }

private:
  unsigned width_, height_;
  Texture image_;
  ComPtr<ID3D12DescriptorHeap> views_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pipeline_;
};

} // namespace atlas_packs
