#include "Gpu.h"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace surface {
namespace {

constexpr unsigned viewLimit = 64, targetLimit = 8;
constexpr std::size_t constantCapacity = 64 * 1024;

void check(HRESULT result, const char *what) {
  if (FAILED(result))
    throw std::runtime_error(what);
}

D3D12_RENDER_TARGET_BLEND_DESC blending(Blend blend) {
  D3D12_RENDER_TARGET_BLEND_DESC out{};
  out.BlendEnable = blend != Blend::Opaque;
  out.SrcBlend = D3D12_BLEND_ONE;
  out.DestBlend = D3D12_BLEND_ZERO;
  out.BlendOp = out.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  out.SrcBlendAlpha = D3D12_BLEND_ONE;
  out.DestBlendAlpha = D3D12_BLEND_ZERO;
  out.LogicOp = D3D12_LOGIC_OP_NOOP;
  out.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (blend == Blend::Alpha || blend == Blend::Coverage) {
    out.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    out.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    out.SrcBlendAlpha = blend == Blend::Coverage ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
    out.DestBlendAlpha = blend == Blend::Coverage ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
  } else if (blend == Blend::Add || blend == Blend::Pressure) {
    out.DestBlend = D3D12_BLEND_ONE;
    if (blend == Blend::Pressure) {
      out.DestBlendAlpha = D3D12_BLEND_ONE;
      out.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
    }
  }
  return out;
}

} // namespace

Gpu::Gpu(ID3D12Device *device, D3D12_FILTER clampFilter) : device_(device) {
  std::array<D3D12_ROOT_PARAMETER,9> parameters{};
  for (unsigned i = 0; i < 2; ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[i].Descriptor.ShaderRegister = i;
  }
  std::array<D3D12_DESCRIPTOR_RANGE,7> ranges{};
  for (unsigned i = 0; i < ranges.size(); ++i) {
    const bool input = i < 4;
    ranges[i] = {input ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                 1, input ? i : i - 4, 0, 0};
    parameters[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[i + 2].DescriptorTable = {1, &ranges[i]};
  }
  std::array<D3D12_STATIC_SAMPLER_DESC,2> samplers{};
  for (unsigned i = 0; i < samplers.size(); ++i) {
    auto &sampler = samplers[i];
    sampler.ShaderRegister = i;
    sampler.Filter = i == 0 ? clampFilter : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxAnisotropy = 1;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
  }
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = static_cast<unsigned>(parameters.size());
  rootDesc.pParameters = parameters.data();
  rootDesc.NumStaticSamplers = static_cast<unsigned>(samplers.size());
  rootDesc.pStaticSamplers = samplers.data();
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> bytes, error;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &error),
        "Serialize surface root signature");
  check(device_->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                     IID_PPV_ARGS(&root_)), "Create surface root signature");

  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = viewLimit;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&visible_)), "Create surface view heap");
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&cpu_)), "Create surface CPU view heap");
  viewStride_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = targetLimit;
  check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs_)), "Create surface render target heap");
  targetStride_ = device_->GetDescriptorHandleIncrementSize(heap.Type);
  nullSrv_ = srv(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);

  constants_ = buffer(constantCapacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  const D3D12_RANGE noRead{0, 0};
  void *data = nullptr;
  check(constants_->Map(0, &noRead, &data), "Map surface frame constants");
  mapped_ = static_cast<std::byte *>(data);
}

Gpu::~Gpu() {
  if (mapped_)
    constants_->Unmap(0, nullptr);
}

void Gpu::begin(ID3D12GraphicsCommandList *commands) {
  // The ABI completes the preceding GPU submission before the next render.
  offset_ = 0;
  ID3D12DescriptorHeap *heaps[]{visible_.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(root_.Get());
  commands->SetComputeRootSignature(root_.Get());
}

D3D12_GPU_VIRTUAL_ADDRESS Gpu::constantBytes(const void *data, std::size_t size) {
  const auto stride = (size + 255) & ~std::size_t{255};
  if (stride > constantCapacity - offset_)
    throw std::runtime_error("Surface frame constants exceed the upload buffer");
  std::memcpy(mapped_ + offset_, data, size);
  const auto address = constants_->GetGPUVirtualAddress() + offset_;
  offset_ += stride;
  return address;
}

ComPtr<ID3D12PipelineState> Gpu::graphics(D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                         DXGI_FORMAT format, Blend blend) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_.Get();
  desc.VS = vs;
  desc.PS = ps;
  desc.BlendState.RenderTarget[0] = blending(blend);
  desc.SampleMask = std::numeric_limits<unsigned>::max();
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pipeline;
  check(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create surface graphics PSO");
  return pipeline;
}

ComPtr<ID3D12PipelineState> Gpu::compute(D3D12_SHADER_BYTECODE cs) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_.Get();
  desc.CS = cs;
  ComPtr<ID3D12PipelineState> pipeline;
  check(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create surface compute PSO");
  return pipeline;
}

View Gpu::allocateView() {
  if (viewCount_ == viewLimit)
    throw std::runtime_error("Surface descriptor heap is full");
  const auto offset = viewCount_++ * viewStride_;
  View view{cpu_->GetCPUDescriptorHandleForHeapStart(), visible_->GetGPUDescriptorHandleForHeapStart()};
  view.cpu.ptr += offset;
  view.gpu.ptr += offset;
  return view;
}

void Gpu::writeSrv(View view, ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip, unsigned levels) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MostDetailedMip = mip;
  desc.Texture2D.MipLevels = levels;
  device_->CreateShaderResourceView(resource, &desc, view.cpu);
  auto visible = visible_->GetCPUDescriptorHandleForHeapStart();
  visible.ptr += view.cpu.ptr - cpu_->GetCPUDescriptorHandleForHeapStart().ptr;
  device_->CopyDescriptorsSimple(1, visible, view.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Gpu::writeUav(View view, ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = mip;
  device_->CreateUnorderedAccessView(resource, nullptr, &desc, view.cpu);
  auto visible = visible_->GetCPUDescriptorHandleForHeapStart();
  visible.ptr += view.cpu.ptr - cpu_->GetCPUDescriptorHandleForHeapStart().ptr;
  device_->CopyDescriptorsSimple(1, visible, view.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

View Gpu::srv(ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip, unsigned levels) {
  auto view = allocateView();
  writeSrv(view, resource, format, mip, levels);
  return view;
}

View Gpu::uav(ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip) {
  auto view = allocateView();
  writeUav(view, resource, format, mip);
  return view;
}

void Gpu::texture(Texture &out, unsigned width, unsigned height, DXGI_FORMAT format,
                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial, unsigned mips) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(mips);
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                  D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  ComPtr<ID3D12Resource> resource;
  check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial,
                                         nullptr, IID_PPV_ARGS(&resource)), "Create surface texture");
  if (!out.srv.cpu.ptr)
    out.srv = allocateView();
  writeSrv(out.srv, resource.Get(), format, 0, mips);
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    if (!out.uav.cpu.ptr)
      out.uav = allocateView();
    writeUav(out.uav, resource.Get(), format, 0);
  }
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
    if (!out.rtv.ptr) {
      if (targetCount_ == targetLimit)
        throw std::runtime_error("Surface render target heap is full");
      out.rtv = rtvs_->GetCPUDescriptorHandleForHeapStart();
      out.rtv.ptr += targetCount_++ * targetStride_;
    }
    device_->CreateRenderTargetView(resource.Get(), nullptr, out.rtv);
  }
  out.resource = std::move(resource);
  out.state = initial;
}

ComPtr<ID3D12Resource> Gpu::buffer(std::size_t bytes, D3D12_HEAP_TYPE type,
                                  D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> resource;
  check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                         nullptr, IID_PPV_ARGS(&resource)), "Create surface buffer");
  return resource;
}

StructuredBuffer Gpu::structured(const void *data, unsigned count, unsigned stride) {
  const std::size_t bytes = std::size_t{count} * stride;
  StructuredBuffer out{
      buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST),
      buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ),
      allocateView()};
  const D3D12_RANGE noRead{0, 0};
  void *mapped = nullptr;
  check(out.staging->Map(0, &noRead, &mapped), "Map surface geometry staging buffer");
  std::memcpy(mapped, data, bytes);
  out.staging->Unmap(0, nullptr);
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.NumElements = count;
  desc.Buffer.StructureByteStride = stride;
  device_->CreateShaderResourceView(out.resource.Get(), &desc, out.srv.cpu);
  auto visible = visible_->GetCPUDescriptorHandleForHeapStart();
  visible.ptr += out.srv.cpu.ptr - cpu_->GetCPUDescriptorHandleForHeapStart().ptr;
  device_->CopyDescriptorsSimple(1, visible, out.srv.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return out;
}

void Gpu::prepare(ID3D12GraphicsCommandList *commands, StructuredBuffer &buffer) {
  if (!buffer.pending) {
    buffer.staging.Reset();
    return;
  }
  commands->CopyBufferRegion(buffer.resource.Get(), 0, buffer.staging.Get(), 0,
                             buffer.resource->GetDesc().Width);
  transition(commands, buffer.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  buffer.pending = false;
}

void Gpu::target(ID3D12GraphicsCommandList *commands, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 unsigned width, unsigned height) {
  const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
}

void Gpu::transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
                     D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to, unsigned subresource) {
  if (from == to)
    return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, subresource, from, to};
  commands->ResourceBarrier(1, &barrier);
}

void Gpu::transition(ID3D12GraphicsCommandList *commands, Texture &texture,
                     D3D12_RESOURCE_STATES to) {
  transition(commands, texture.resource.Get(), texture.state, to);
  texture.state = to;
}

} // namespace surface
