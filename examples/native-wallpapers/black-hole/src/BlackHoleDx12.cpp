#include "BlackHoleDx12.h"
#include "BlackHoleDx12Shaders.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <wincodec.h>

namespace action75 {
namespace {
using Microsoft::WRL::ComPtr;
using namespace black_hole;
constexpr auto ShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto Target = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr auto ColorTarget = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
constexpr UINT SceneOffset = 0;
constexpr UINT StarOffset = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
static_assert(sizeof(SceneConstants) <= StarOffset);

void require(HRESULT hr, const char *operation) {
  if (FAILED(hr))
    throw std::runtime_error(operation);
}

struct Pixels {
  UINT width = 0, height = 0;
  std::vector<BYTE> rgba;
};

Pixels loadArtwork(const wchar_t *directory) {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> image;
  require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)), "Create WIC factory");
  const auto path = std::filesystem::path(directory) / L"nebula.png";
  require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder), "Open nebula.png");
  require(decoder->GetFrame(0, &frame), "Read nebula image");
  require(factory->CreateFormatConverter(&image), "Create nebula color converter");
  require(image->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                            nullptr, 0, WICBitmapPaletteTypeCustom), "Convert nebula to RGBA");
  Pixels pixels;
  require(image->GetSize(&pixels.width, &pixels.height), "Read nebula dimensions");
  const auto count = std::uint64_t(pixels.width) * pixels.height;
  if (!count || count > 4194304)
    throw std::runtime_error("Invalid nebula dimensions");
  pixels.rgba.resize(static_cast<std::size_t>(count) * 4);
  require(image->CopyPixels(nullptr, pixels.width * 4, static_cast<UINT>(pixels.rgba.size()),
                            pixels.rgba.data()), "Decode nebula pixels");
  return pixels;
}

bool supports(ID3D12Device *device, DXGI_FORMAT format, UINT features) {
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
  return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
         (support.Support1 & features) == features;
}

ComPtr<ID3D12Resource> resource(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  ComPtr<ID3D12Resource> result;
  require(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&result)), "Allocate black hole resource");
  return result;
}

ComPtr<ID3D12Resource> texture(ID3D12Device *device, DXGI_FORMAT format, UINT width, UINT height,
                              UINT16 levels, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = levels;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  return resource(device, desc, D3D12_HEAP_TYPE_DEFAULT, state);
}

ComPtr<ID3D12Resource> uploadBuffer(ID3D12Device *device, UINT64 size) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return resource(device, desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
}

ComPtr<ID3D12DescriptorHeap> descriptors(ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                       UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
  const D3D12_DESCRIPTOR_HEAP_DESC desc{type, count, flags, 0};
  ComPtr<ID3D12DescriptorHeap> heap;
  require(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "Allocate black hole descriptors");
  return heap;
}

void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *image,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {image, subresource, before, after};
  commands->ResourceBarrier(1, &barrier);
}

void viewport(ID3D12GraphicsCommandList *commands, unsigned width, unsigned height) {
  const D3D12_VIEWPORT view{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  commands->RSSetViewports(1, &view);
  commands->RSSetScissorRects(1, &scissor);
}

void fullscreen(ID3D12GraphicsCommandList *commands, ID3D12PipelineState *pso,
                D3D12_CPU_DESCRIPTOR_HANDLE target, unsigned width, unsigned height) {
  viewport(commands, width, height);
  commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
  commands->SetPipelineState(pso);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->DrawInstanced(3, 1, 0, 0);
}

template <std::size_t N> D3D12_SHADER_BYTECODE code(const unsigned char (&bytes)[N]) {
  return {bytes, N};
}
} // namespace

BlackHoleDx12::BlackHoleDx12(ID3D12Device *device, const wchar_t *directory)
    : device_(device) {
  constexpr UINT renderAndSample = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
  hdrFormat_ = supports(device_, DXGI_FORMAT_R11G11B10_FLOAT, renderAndSample | D3D12_FORMAT_SUPPORT1_BLENDABLE)
      ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
  starFormat_ = supports(device_, DXGI_FORMAT_R8G8_UNORM, renderAndSample | D3D12_FORMAT_SUPPORT1_MIP)
      ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  rtvs_ = descriptors(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
  srvs_ = descriptors(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SrvCount,
                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
  rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  srvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  writeSrv(nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, NullSrv, 0, 1);
  createRootSignature();
  createPipelineStates();

  constants_ = uploadBuffer(device_, StarOffset + sizeof(StarConstants) * MaxStars);
  const D3D12_RANGE noRead{0, 0};
  void *data = nullptr;
  require(constants_->Map(0, &noRead, &data), "Map black hole constants");
  mapped_ = static_cast<unsigned char *>(data);

  const auto pixels = loadArtwork(directory);
  upload(artwork_, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.width, pixels.height,
         pixels.width * 4, pixels.rgba.data(), ArtworkSrv);
  const auto noise = turbulenceTexture();
  upload(noise_, DXGI_FORMAT_R8G8_UNORM, NoiseSize, NoiseSize, NoiseSize * 2, noise.data(), NoiseSrv);

  constexpr auto skyFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  sky_ = texture(device_, skyFormat, SkyWidth, SkyHeight, MipLevels, ColorTarget, Target);
  stars_ = texture(device_, starFormat_, SkyWidth, SkyHeight, MipLevels, ColorTarget, Target);
  createSky(sky_.Get(), skyFormat, SkyRtv, SkySrv, SkyMipSrv);
  createSky(stars_.Get(), starFormat_, StarsRtv, StarsSrv, StarsMipSrv);
}

void BlackHoleDx12::createRootSignature() {
  D3D12_ROOT_PARAMETER params[7]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1] = params[0];
  params[1].Descriptor.ShaderRegister = 1;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_DESCRIPTOR_RANGE textures[5]{};
  for (UINT i = 0; i < std::size(textures); ++i) {
    textures[i] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, 0};
    params[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i + 2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[i + 2].DescriptorTable = {1, &textures[i]};
  }

  D3D12_STATIC_SAMPLER_DESC samplers[3]{};
  constexpr D3D12_TEXTURE_ADDRESS_MODE addresses[] = {
      D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
  for (UINT i = 0; i < std::size(samplers); ++i) {
    auto &sampler = samplers[i];
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = addresses[i];
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = i;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  const D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(std::size(params)), params,
      static_cast<UINT>(std::size(samplers)), samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> binary;
  require(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &binary, nullptr),
          "Serialize black hole root signature");
  require(device_->CreateRootSignature(0, binary->GetBufferPointer(), binary->GetBufferSize(),
                                       IID_PPV_ARGS(&root_)), "Create black hole root signature");
}

void BlackHoleDx12::createPipelineStates() {
  using namespace black_hole_dx12;
  const auto create = [&](D3D12_SHADER_BYTECODE pixel, DXGI_FORMAT format, bool additive = false,
                          bool secondOutput = false) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root_.Get();
    state.VS = additive ? code(StarVs) : code(ScreenVs);
    state.PS = pixel;
    state.BlendState.RenderTarget[0] = {additive, FALSE, D3D12_BLEND_ONE,
      additive ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ZERO, D3D12_BLEND_ONE,
      D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    state.SampleMask = UINT_MAX;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                       D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    state.DepthStencilState.BackFace = state.DepthStencilState.FrontFace;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = secondOutput ? 2 : 1;
    state.RTVFormats[0] = format;
    if (secondOutput)
      state.RTVFormats[1] = format;
    state.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> result;
    require(device_->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&result)),
            "Create black hole pipeline state");
    return result;
  };

  scene_ = create(code(ScenePs), hdrFormat_);
  star_ = create(code(StarPs), hdrFormat_, true);
  lightSource_ = create(code(LightSourcePs), hdrFormat_, true, true);
  blurHorizontal_ = create(code(BloomHorizontalPs), hdrFormat_);
  blurVertical_ = create(code(BloomVerticalPs), hdrFormat_);
  composite_ = create(code(CompositePs), DXGI_FORMAT_R8G8B8A8_UNORM);
  bakeArtwork_ = create(code(BakeArtworkPs), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  bakeStars_ = create(code(BakeStarsPs), starFormat_);
  mipArtwork_ = create(code(SkyMipPs), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  mipStars_ = create(code(SkyMipPs), starFormat_);
}

D3D12_CPU_DESCRIPTOR_HANDLE BlackHoleDx12::rtv(UINT index) const {
  auto handle = rtvs_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * rtvStride_;
  return handle;
}

void BlackHoleDx12::writeSrv(ID3D12Resource *image, DXGI_FORMAT format, UINT slot, UINT firstMip, UINT mips) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MostDetailedMip = firstMip;
  desc.Texture2D.MipLevels = mips;
  auto handle = srvs_->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(slot) * srvStride_;
  device_->CreateShaderResourceView(image, &desc, handle);
}

void BlackHoleDx12::writeRtv(ID3D12Resource *image, DXGI_FORMAT format, UINT slot, UINT mip) {
  D3D12_RENDER_TARGET_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = mip;
  device_->CreateRenderTargetView(image, &desc, rtv(slot));
}

void BlackHoleDx12::bindTexture(ID3D12GraphicsCommandList *commands, UINT shaderRegister, UINT index) const {
  auto handle = srvs_->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(index) * srvStride_;
  commands->SetGraphicsRootDescriptorTable(2 + shaderRegister, handle);
}

void BlackHoleDx12::upload(Upload &out, DXGI_FORMAT format, unsigned width, unsigned height,
                           unsigned rowBytes, const void *pixels, Srv slot) {
  out.texture = texture(device_, format, width, height, 1, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  const auto desc = out.texture->GetDesc();
  UINT64 size = 0;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &out.footprint, nullptr, nullptr, &size);
  out.buffer = uploadBuffer(device_, size);
  const D3D12_RANGE noRead{0, 0};
  void *data = nullptr;
  require(out.buffer->Map(0, &noRead, &data), "Map nebula or noise upload");
  auto *destination = static_cast<unsigned char *>(data) + out.footprint.Offset;
  const auto *source = static_cast<const unsigned char *>(pixels);
  for (UINT y = 0; y < height; ++y)
    std::memcpy(destination + size_t(y) * out.footprint.Footprint.RowPitch,
                source + size_t(y) * rowBytes, rowBytes);
  out.buffer->Unmap(0, nullptr);
  writeSrv(out.texture.Get(), format, slot, 0, 1);
}

void BlackHoleDx12::Upload::record(ID3D12GraphicsCommandList *commands) const {
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = buffer.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = texture.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(commands, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, ShaderRead);
}

void BlackHoleDx12::createSky(ID3D12Resource *image, DXGI_FORMAT format, Rtv firstRtv, Srv all, Srv firstMip) {
  writeSrv(image, format, all, 0, MipLevels);
  for (UINT i = 0; i < MipLevels; ++i) {
    writeRtv(image, format, firstRtv + i, i);
    writeSrv(image, format, firstMip + i, i, 1);
  }
}

void BlackHoleDx12::bakeSky(ID3D12GraphicsCommandList *commands, ID3D12Resource *image, Rtv firstRtv,
                           Srv firstMip, ID3D12PipelineState *bake, ID3D12PipelineState *mip) {
  fullscreen(commands, bake, rtv(firstRtv), SkyWidth, SkyHeight);
  transition(commands, image, Target, ShaderRead, 0);
  for (UINT i = 1; i < MipLevels; ++i) {
    bindTexture(commands, 2, firstMip + i - 1);
    fullscreen(commands, mip, rtv(firstRtv + i), std::max(1u, SkyWidth >> i), std::max(1u, SkyHeight >> i));
    transition(commands, image, Target, ShaderRead, i);
  }
  bindTexture(commands, 2, NullSrv);
}

void BlackHoleDx12::resize(unsigned width, unsigned height) {
  if (width_ == width && height_ == height)
    return;
  const unsigned blurWidth = (width + 3) / 4, blurHeight = (height + 3) / 4;
  hdr_ = texture(device_, hdrFormat_, width, height, 1, ColorTarget, ShaderRead);
  bloomA_ = texture(device_, hdrFormat_, blurWidth, blurHeight, 1, ColorTarget, ShaderRead);
  bloomB_ = texture(device_, hdrFormat_, blurWidth, blurHeight, 1, ColorTarget, ShaderRead);
  writeRtv(hdr_.Get(), hdrFormat_, HdrRtv, 0);
  writeRtv(bloomA_.Get(), hdrFormat_, BloomARtv, 0);
  writeRtv(bloomB_.Get(), hdrFormat_, BloomBRtv, 0);
  writeSrv(hdr_.Get(), hdrFormat_, HdrSrv, 0, 1);
  writeSrv(bloomA_.Get(), hdrFormat_, BloomASrv, 0, 1);
  writeSrv(bloomB_.Get(), hdrFormat_, BloomBSrv, 0, 1);
  width_ = width;
  height_ = height;
}

void BlackHoleDx12::draw(const A75Frame12 &frame, bool paused) {
  resize(frame.width, frame.height);
  const unsigned blurWidth = (width_ + 3) / 4, blurHeight = (height_ + 3) / 4;
  SceneConstants scene;
  std::array<StarConstants, MaxStars> stars;
  const unsigned visibleStars = prepareFrame(width_, height_, blurWidth, blurHeight, frame.seconds,
                                             paused, scene, stars.data());
  // The ABI waits for the previous frame before reusing the mapped constants.
  std::memcpy(mapped_ + SceneOffset, &scene, sizeof(scene));
  if (visibleStars)
    std::memcpy(mapped_ + StarOffset, stars.data(), sizeof(StarConstants) * visibleStars);

  auto *commands = frame.commands;
  ID3D12DescriptorHeap *heaps[] = {srvs_.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(root_.Get());
  const auto gpu = constants_->GetGPUVirtualAddress();
  commands->SetGraphicsRootConstantBufferView(0, gpu + SceneOffset);
  commands->SetGraphicsRootConstantBufferView(1, gpu + StarOffset);
  bindTexture(commands, 2, NullSrv);
  bindTexture(commands, 3, NullSrv);

  if (!baked_) {
    bindTexture(commands, 0, NullSrv);
    bindTexture(commands, 1, NullSrv);
    bindTexture(commands, 4, NullSrv);
    artwork_.record(commands);
    noise_.record(commands);
    bindTexture(commands, 2, ArtworkSrv);
    bakeSky(commands, sky_.Get(), SkyRtv, SkyMipSrv, bakeArtwork_.Get(), mipArtwork_.Get());
    bakeSky(commands, stars_.Get(), StarsRtv, StarsMipSrv, bakeStars_.Get(), mipStars_.Get());
    baked_ = true;
  } else if (artwork_.buffer) {
    // Keep the first frame's input textures and PSOs alive until that work completes.
    artwork_ = {};
    noise_.buffer.Reset();
    bakeArtwork_.Reset();
    bakeStars_.Reset();
    mipArtwork_.Reset();
    mipStars_.Reset();
  }

  const auto drawStars = [&](ID3D12PipelineState *pso) {
    commands->SetPipelineState(pso);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    commands->DrawInstanced(4, visibleStars, 0, 0);
  };

  bindTexture(commands, 0, SkySrv);
  bindTexture(commands, 1, NoiseSrv);
  bindTexture(commands, 4, StarsSrv);
  if (scene.hasEmissionMaps > 0) {
    // The two quarter-resolution targets also hold the light from both lens paths.
    transition(commands, bloomA_.Get(), ShaderRead, Target);
    transition(commands, bloomB_.Get(), ShaderRead, Target);
    const D3D12_CPU_DESCRIPTOR_HANDLE maps[] = {rtv(BloomARtv), rtv(BloomBRtv)};
    constexpr float black[4]{};
    for (const auto map : maps)
      commands->ClearRenderTargetView(map, black, 0, nullptr);
    viewport(commands, blurWidth, blurHeight);
    commands->OMSetRenderTargets(2, maps, FALSE, nullptr);
    drawStars(lightSource_.Get());
    transition(commands, bloomA_.Get(), Target, ShaderRead);
    transition(commands, bloomB_.Get(), Target, ShaderRead);
    bindTexture(commands, 2, BloomASrv);
    bindTexture(commands, 3, BloomBSrv);
  }

  transition(commands, hdr_.Get(), ShaderRead, Target);
  fullscreen(commands, scene_.Get(), rtv(HdrRtv), width_, height_);
  if (visibleStars)
    drawStars(star_.Get());
  transition(commands, hdr_.Get(), Target, ShaderRead);

  bindTexture(commands, 2, HdrSrv);
  bindTexture(commands, 3, NullSrv);
  transition(commands, bloomA_.Get(), ShaderRead, Target);
  fullscreen(commands, blurHorizontal_.Get(), rtv(BloomARtv), blurWidth, blurHeight);
  transition(commands, bloomA_.Get(), Target, ShaderRead);
  bindTexture(commands, 2, BloomASrv);
  transition(commands, bloomB_.Get(), ShaderRead, Target);
  fullscreen(commands, blurVertical_.Get(), rtv(BloomBRtv), blurWidth, blurHeight);
  transition(commands, bloomB_.Get(), Target, ShaderRead);

  bindTexture(commands, 2, HdrSrv);
  bindTexture(commands, 3, BloomBSrv);
  fullscreen(commands, composite_.Get(), frame.rtv, width_, height_);
}
} // namespace action75
