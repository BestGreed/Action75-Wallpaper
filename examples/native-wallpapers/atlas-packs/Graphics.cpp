#include "Graphics.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <wincodec.h>

namespace atlas_packs {
void check(HRESULT result, const char* operation) {
  if (FAILED(result)) {
    std::ostringstream error;
    error << operation << " (0x" << std::hex << static_cast<unsigned long>(result) << ')';
    throw std::runtime_error(error.str());
  }
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result{};
  result.Type = type;
  result.CreationNodeMask = result.VisibleNodeMask = 1;
  return result;
}

D3D12_RESOURCE_DESC textureDescription(unsigned width, unsigned height) {
  D3D12_RESOURCE_DESC result{};
  result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  result.Width = width;
  result.Height = height;
  result.DepthOrArraySize = result.MipLevels = 1;
  result.Format = format;
  result.SampleDesc.Count = 1;
  return result;
}

void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  commands->ResourceBarrier(1, &barrier);
}

void bindTarget(const A75Frame12& frame) {
  const D3D12_VIEWPORT viewport{0, 0, float(frame.width), float(frame.height), 0, 1};
  const D3D12_RECT scissor{0, 0, LONG(frame.width), LONG(frame.height)};
  frame.commands->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
  frame.commands->RSSetViewports(1, &viewport);
  frame.commands->RSSetScissorRects(1, &scissor);
}

Image decodeImage(const std::filesystem::path& path, unsigned limit) {
  // The player usually already owns a COM apartment. The standalone ABI probe
  // is also allowed to construct an instance on a fresh thread.
  struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() {
      if (SUCCEEDED(result))
        CoUninitialize();
    }
  } apartment;
  if (apartment.result != RPC_E_CHANGED_MODE)
    check(apartment.result, "Enter image decoder COM apartment");
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
        "Create WIC imaging factory");
  check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                           WICDecodeMetadataCacheOnLoad, &decoder), "Open image");
  check(decoder->GetFrame(0, &frame), "Read image frame");
  Image image;
  check(frame->GetSize(&image.width, &image.height), "Read image dimensions");
  if (!image.width || !image.height || image.width > limit || image.height > limit ||
      uint64_t(image.width) * image.height > 32 * 1024 * 1024)
    throw std::runtime_error("Wallpaper image exceeds dimensions limit");
  check(factory->CreateFormatConverter(&converter), "Create WIC RGBA converter");
  check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                              nullptr, 0, WICBitmapPaletteTypeCustom), "Convert image to RGBA");
  image.pixels.resize(size_t(image.width) * image.height * 4);
  check(converter->CopyPixels(nullptr, image.width * 4, UINT(image.pixels.size()), image.pixels.data()),
        "Decode RGBA image");
  return image;
}

Upload::Upload(ID3D12Device* device, size_t bytes) : size_(bytes) {
  auto heap = heapProperties(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC info{};
  info.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  info.Width = (bytes + 255) & ~uint64_t(255);
  info.Height = info.DepthOrArraySize = info.MipLevels = info.SampleDesc.Count = 1;
  info.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &info,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&resource_)), "Allocate upload buffer");
  void* mapped = nullptr;
  D3D12_RANGE read{0, 0};
  check(resource_->Map(0, &read, &mapped), "Map upload buffer");
  mapped_ = static_cast<uint8_t*>(mapped);
}

void Upload::write(const void* data, size_t size) const {
  if (size > size_)
    throw std::runtime_error("Upload buffer overflow");
  std::memcpy(mapped_, data, size);
}

void Texture::load(ID3D12Device* device, const Image& image) {
  ComPtr<ID3D12Resource> texture;
  const auto info = textureDescription(image.width, image.height);
  const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &info,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&texture)), "Allocate wallpaper texture");
  attach(device, std::move(texture));
  UINT64 size = 0;
  device->GetCopyableFootprints(&info, 0, 1, 0, &layout_, nullptr, nullptr, &size);
  upload_ = Upload(device, size);
  const size_t rowBytes = size_t(image.width) * 4;
  auto* dst = upload_.bytes() + layout_.Offset;
  for (unsigned y = 0; y < image.height; ++y)
    std::memcpy(dst + y * layout_.Footprint.RowPitch, image.pixels.data() + y * rowBytes, rowBytes);
}

void Texture::attach(ID3D12Device* device, ComPtr<ID3D12Resource> source) {
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 1;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_)), "Allocate texture descriptor");
  device->CreateShaderResourceView(source.Get(), nullptr, heap_->GetCPUDescriptorHandleForHeapStart());
  resource_ = std::move(source);
}

void Texture::prepare(ID3D12GraphicsCommandList* commands) {
  if (!upload_.resource())
    return;
  if (copied_) {
    // ABI 2 fences the prior frame before another callback or destroy.
    upload_ = {};
    return;
  }
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = resource_.Get();
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = upload_.resource();
  source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source.PlacedFootprint = layout_;
  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  transition(commands, resource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  copied_ = true;
}

Pipeline::Pipeline(ID3D12Device* device, D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
                   Blend blend, bool texture, D3D12_INPUT_LAYOUT_DESC input) {
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rootDesc{
      texture ? 2u : 1u, parameters, texture ? 1u : 0u, texture ? &sampler : nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
  };
  ComPtr<ID3DBlob> signature;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr),
        "Serialize wallpaper root signature");
  check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                    IID_PPV_ARGS(&root_)), "Create wallpaper root signature");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_.Get();
  desc.VS = vertex;
  desc.PS = pixel;
  desc.InputLayout = input;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = 1;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  auto& target = desc.BlendState.RenderTarget[0];
  target.BlendEnable = blend != Blend::Opaque;
  target.SrcBlend = blend == Blend::Multiply ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_ONE;
  target.DestBlend = blend == Blend::Multiply ? D3D12_BLEND_INV_SRC1_COLOR : D3D12_BLEND_ONE;
  target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  target.SrcBlendAlpha = D3D12_BLEND_ONE;
  target.DestBlendAlpha = blend == Blend::Multiply ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
  target.LogicOp = D3D12_LOGIC_OP_NOOP;
  target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline_)), "Create wallpaper pipeline");
}

void Pipeline::bind(ID3D12GraphicsCommandList* commands, const Upload& constants,
                    const Texture* texture) const {
  commands->SetGraphicsRootSignature(root_.Get());
  commands->SetPipelineState(pipeline_.Get());
  commands->SetGraphicsRootConstantBufferView(0, constants.address());
  if (texture) {
    auto* heap = texture->heap();
    commands->SetDescriptorHeaps(1, &heap);
    commands->SetGraphicsRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
  }
}
} // namespace atlas_packs
