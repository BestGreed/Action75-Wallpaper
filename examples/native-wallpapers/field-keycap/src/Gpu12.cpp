#include "Gpu12.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace wallpaper {
namespace {
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties{};
  properties.Type = type;
  properties.CreationNodeMask = properties.VisibleNodeMask = 1;
  return properties;
}

ComPtr<ID3D12Resource> uploadBuffer(ID3D12Device *device, UINT64 bytes) {
  auto memory = heap(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &desc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&result)), "Allocate upload buffer");
  return result;
}
} // namespace

void check(HRESULT result, const char *operation) {
  if (FAILED(result))
    throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
                             std::to_string(static_cast<long>(result)) + ")");
}

ComPtr<ID3D12Resource> texture(ID3D12Device *device, unsigned width, unsigned height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                              const D3D12_CLEAR_VALUE *clear) {
  auto memory = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = format;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_NONE, &desc, state, clear,
                                       IID_PPV_ARGS(&result)), "Allocate texture");
  return result;
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  if (before == after)
    return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &barrier);
}

void setTarget(ID3D12GraphicsCommandList *commands, D3D12_CPU_DESCRIPTOR_HANDLE target,
               unsigned width, unsigned height) {
  D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
}

Graphics12::Graphics12(ID3D12Device *device, std::size_t constantSize, bool useTextures)
    : device_(device), constantSize_(constantSize) {
  D3D12_DESCRIPTOR_RANGE textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, TextureSlots, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &textures};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC samplers[2]{};
  for (unsigned index = 0; index < 2; ++index) {
    auto &sampler = samplers[index];
    const auto address = index == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = address;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = index;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{useTextures ? 2u : 1u, parameters, useTextures ? 2u : 0u,
                                useTextures ? samplers : nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> serialized;
  check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
        "Serialize wallpaper root signature");
  check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                    IID_PPV_ARGS(&signature_)), "Create wallpaper root signature");
  constexpr auto alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  constants_ = uploadBuffer(device, (constantSize + alignment - 1) & ~(std::size_t(alignment) - 1));
  const D3D12_RANGE noReads{0, 0};
  check(constants_->Map(0, &noReads, &mapped_), "Map wallpaper constants");
}

Graphics12::~Graphics12() {
  if (mapped_)
    constants_->Unmap(0, nullptr);
}

ComPtr<ID3D12PipelineState> Graphics12::pipeline(D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
                                               DXGI_FORMAT format, bool weighted) const {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = signature_.Get();
  desc.VS = vertex;
  desc.PS = pixel;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  if (weighted) {
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ONE;
  }
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> result;
  check(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)), "Create wallpaper pipeline");
  return result;
}

void Graphics12::bind(ID3D12GraphicsCommandList *commands, const void *constants) const {
  std::memcpy(mapped_, constants, constantSize_);
  commands->SetGraphicsRootSignature(signature_.Get());
  commands->SetGraphicsRootConstantBufferView(0, constants_->GetGPUVirtualAddress());
}

TextureUpload::TextureUpload(ID3D12Device *device, unsigned width, unsigned height, DXGI_FORMAT format)
    : texture_(texture(device, width, height, format, D3D12_RESOURCE_FLAG_NONE,
                       D3D12_RESOURCE_STATE_COPY_DEST)) {
  const auto desc = texture_->GetDesc();
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint_, &rows_, &rowBytes_, &bytes);
  upload_ = uploadBuffer(device, bytes);
}

void TextureUpload::stage(const void *pixels, std::size_t stride) {
  const D3D12_RANGE noReads{0, 0};
  void *mapped = nullptr;
  check(upload_->Map(0, &noReads, &mapped), "Map texture upload");
  auto *destination = static_cast<std::byte *>(mapped) + footprint_.Offset;
  auto *source = static_cast<const std::byte *>(pixels);
  for (UINT row = 0; row < rows_; ++row)
    std::memcpy(destination + row * footprint_.Footprint.RowPitch, source + row * stride, rowBytes_);
  upload_->Unmap(0, nullptr);
}

void TextureUpload::record(ID3D12GraphicsCommandList *commands) {
  transition(commands, texture_.Get(), state_, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
  destination.pResource = texture_.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.pResource = upload_.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = footprint_;
  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  transition(commands, texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state_);
}
} // namespace wallpaper
