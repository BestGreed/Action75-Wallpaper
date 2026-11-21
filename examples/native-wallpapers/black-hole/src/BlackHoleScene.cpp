#include "BlackHoleScene.h"
#include "BlackHoleDx11.h"
#include <cstring>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace action75 {
using namespace black_hole;
namespace {
bool check(HRESULT result, const char *operation, std::string &error) {
  if (SUCCEEDED(result))
    return true;
  std::ostringstream message;
  message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
  error = message.str();
  return false;
}

void bindRaster(ID3D11DeviceContext *context, ID3D11RasterizerState *raster) {
  context->RSSetState(raster);
  context->OMSetDepthStencilState(nullptr, 0);
  context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->GSSetShader(nullptr, nullptr, 0);
  context->HSSetShader(nullptr, nullptr, 0);
  context->DSSetShader(nullptr, nullptr, 0);
}

} // namespace

bool BlackHoleScene::load(ID3D11Device *device, ID3D11DeviceContext *context,
                         ID3D11ShaderResourceView *artwork, std::string &error) {
  const auto vertex = [&](auto &slot, const auto &bytes, const char *name) {
    return check(device->CreateVertexShader(bytes, sizeof(bytes), nullptr, slot.GetAddressOf()), name, error);
  };
  const auto pixel = [&](auto &slot, const auto &bytes, const char *name) {
    return check(device->CreatePixelShader(bytes, sizeof(bytes), nullptr, slot.GetAddressOf()), name, error);
  };
  using namespace black_hole_dx11;
  ComPtr<ID3D11PixelShader> bakeNebula, bakeStars;
  if (!vertex(screenVs_, ScreenVs, "ScreenVs") || !pixel(scenePs_, ScenePs, "ScenePs") ||
      !vertex(starVs_, StarVs, "StarVs") || !pixel(starPs_, StarPs, "StarPs") ||
      !pixel(lightSourcePs_, LightSourcePs, "LightSourcePs") ||
      !pixel(bloomHorizontal_, BloomHorizontalPs, "BloomHorizontalPs") ||
      !pixel(bloomVertical_, BloomVerticalPs, "BloomVerticalPs") ||
      !pixel(composite_, CompositePs, "CompositePs") || !pixel(bakeNebula, BakeArtworkPs, "BakeArtworkPs") ||
      !pixel(bakeStars, BakeStarsPs, "BakeStarsPs"))
    return false;

  constexpr UINT hdrSupport = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                              D3D11_FORMAT_SUPPORT_BLENDABLE;
  UINT supported = 0;
  if (SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R11G11B10_FLOAT, &supported)) &&
      (supported & hdrSupport) == hdrSupport)
    hdrFormat_ = DXGI_FORMAT_R11G11B10_FLOAT;

  D3D11_BUFFER_DESC buffer{};
  buffer.ByteWidth = sizeof(SceneConstants);
  buffer.Usage = D3D11_USAGE_DYNAMIC;
  buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (!check(device->CreateBuffer(&buffer, nullptr, &sceneConstants_), "Black hole constants", error))
    return false;
  buffer.ByteWidth = sizeof(StarConstants) * MaxStars;
  if (!check(device->CreateBuffer(&buffer, nullptr, &starConstants_), "Star instances", error))
    return false;

  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                           D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                           D3D11_COLOR_WRITE_ENABLE_ALL};
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (!check(device->CreateRasterizerState(&raster, &raster_), "Black hole rasterizer", error) ||
      !check(device->CreateBlendState(&blend, &starBlend_), "Star blending", error) ||
      !check(device->CreateSamplerState(&sampler, &skySampler_), "Sky sampler", error))
    return false;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  if (!check(device->CreateSamplerState(&sampler, &turbulenceSampler_), "Disk sampler", error))
    return false;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
  if (!check(device->CreateSamplerState(&sampler, &emissionSampler_), "Emission map sampler", error))
    return false;

  const auto noise = turbulenceTexture();
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = texture.Height = NoiseSize;
  texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
  texture.Format = DXGI_FORMAT_R8G8_UNORM;
  texture.Usage = D3D11_USAGE_IMMUTABLE;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA noiseData{noise.data(), 2 * NoiseSize, 0};
  ComPtr<ID3D11Texture2D> noiseTexture;
  if (!check(device->CreateTexture2D(&texture, &noiseData, &noiseTexture), "Disk turbulence", error) ||
      !check(device->CreateShaderResourceView(noiseTexture.Get(), nullptr, &turbulence_),
             "Disk turbulence view", error))
    return false;

  texture.Width = SkyWidth;
  texture.Height = SkyHeight;
  texture.MipLevels = 0;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  texture.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  const D3D11_VIEWPORT viewport{0, 0, SkyWidth, SkyHeight, 0, 1};
  bindRaster(context, raster_.Get());
  context->RSSetViewports(1, &viewport);
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  const auto bake = [&](DXGI_FORMAT format, ID3D11PixelShader *shader, ID3D11ShaderResourceView **view) {
    texture.Format = format;
    ComPtr<ID3D11Texture2D> plate;
    ComPtr<ID3D11RenderTargetView> target;
    if (!check(device->CreateTexture2D(&texture, nullptr, &plate), "Sky texture", error) ||
        !check(device->CreateRenderTargetView(plate.Get(), nullptr, &target), "Sky target", error) ||
        !check(device->CreateShaderResourceView(plate.Get(), nullptr, view), "Sky view", error))
      return false;
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->PSSetShader(shader, nullptr, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->GenerateMips(*view);
    return true;
  };
  context->PSSetShaderResources(2, 1, &artwork);
  auto *skySampler = skySampler_.Get();
  context->PSSetSamplers(0, 1, &skySampler);
  const bool baked = bake(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, bakeNebula.Get(), sky_.GetAddressOf());
  ID3D11ShaderResourceView *empty = nullptr;
  context->PSSetShaderResources(2, 1, &empty);

  constexpr UINT mapSupport = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                              D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
  supported = 0;
  const auto starFormat = SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R8G8_UNORM, &supported)) &&
                                  (supported & mapSupport) == mapSupport
                              ? DXGI_FORMAT_R8G8_UNORM
                              : DXGI_FORMAT_R8G8B8A8_UNORM;
  return baked && bake(starFormat, bakeStars.Get(), skyStars_.GetAddressOf());
}

bool BlackHoleScene::resize(ID3D11DeviceContext *context, unsigned width, unsigned height,
                           std::string &error) {
  if (width == width_ && height == height_)
    return true;
  ComPtr<ID3D11Device> device;
  context->GetDevice(&device);
  const auto create = [&](unsigned w, unsigned h, Surface &surface) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = hdrFormat_;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    return check(device->CreateTexture2D(&desc, nullptr, &texture), "Black hole HDR surface", error) &&
           check(device->CreateRenderTargetView(texture.Get(), nullptr, &surface.target),
                 "Black hole HDR target", error) &&
           check(device->CreateShaderResourceView(texture.Get(), nullptr, &surface.view),
                 "Black hole HDR view", error);
  };
  const unsigned blurWidth = (width + 3) / 4, blurHeight = (height + 3) / 4;
  Surface image, horizontal, vertical;
  if (!create(width, height, image) || !create(blurWidth, blurHeight, horizontal) ||
      !create(blurWidth, blurHeight, vertical))
    return false;
  hdr_ = std::move(image);
  bloom_[0] = std::move(horizontal);
  bloom_[1] = std::move(vertical);
  width_ = width;
  height_ = height;
  bloomWidth_ = blurWidth;
  bloomHeight_ = blurHeight;
  return true;
}

bool BlackHoleScene::draw(ID3D11DeviceContext *context, unsigned width, unsigned height, double now,
                         bool paused, std::string &error) {
  if (!width || !height)
    return true;
  if (!resize(context, width, height, error))
    return false;
  SceneConstants scene;
  std::array<StarConstants, MaxStars> instances;
  const auto visibleStars = prepareFrame(width, height, bloomWidth_, bloomHeight_, now, paused,
                                         scene, instances.data());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (visibleStars) {
    if (!check(context->Map(starConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
               "Map star instances", error))
      return false;
    std::memcpy(mapped.pData, instances.data(), sizeof(StarConstants) * visibleStars);
    context->Unmap(starConstants_.Get(), 0);
  }
  if (!check(context->Map(sceneConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
             "Map black hole constants", error))
    return false;
  std::memcpy(mapped.pData, &scene, sizeof(scene));
  context->Unmap(sceneConstants_.Get(), 0);

  const D3D11_VIEWPORT full{0, 0, scene.width, scene.height, 0, 1};
  const D3D11_VIEWPORT reduced{0, 0, static_cast<float>(bloomWidth_), static_cast<float>(bloomHeight_), 0, 1};
  ComPtr<ID3D11RenderTargetView> output;
  context->OMGetRenderTargets(1, &output, nullptr);
  const auto pass = [&](ID3D11RenderTargetView *target, const D3D11_VIEWPORT &viewport,
                        ID3D11PixelShader *shader) {
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &viewport);
    context->PSSetShader(shader, nullptr, 0);
  };

  bindRaster(context, raster_.Get());
  ID3D11Buffer *constants = sceneConstants_.Get();
  ID3D11ShaderResourceView *skyViews[] = {sky_.Get(), turbulence_.Get()};
  ID3D11SamplerState *samplers[] = {skySampler_.Get(), turbulenceSampler_.Get(), emissionSampler_.Get()};
  context->VSSetShader(screenVs_.Get(), nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &constants);
  context->PSSetShaderResources(0, 2, skyViews);
  auto *starFieldView = skyStars_.Get();
  context->PSSetShaderResources(4, 1, &starFieldView);
  context->PSSetSamplers(0, 3, samplers);
  const auto drawStars = [&](ID3D11PixelShader *shader) {
    ID3D11Buffer *starBuffers[] = {constants, starConstants_.Get()};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(starVs_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 2, starBuffers);
    context->PSSetShader(shader, nullptr, 0);
    context->OMSetBlendState(starBlend_.Get(), nullptr, 0xffffffff);
    context->DrawInstanced(4, visibleStars, 0, 0);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(screenVs_.Get(), nullptr, 0);
    ID3D11Buffer *empty[] = {nullptr, nullptr};
    context->VSSetConstantBuffers(0, 2, empty);
  };
  if (scene.hasEmissionMaps > 0) {
    // The bloom buffers are scratch surfaces at this stage. Store the light
    // seen by the short and long paths before reusing them for the bloom passes.
    ID3D11RenderTargetView *maps[] = {bloom_[0].target.Get(), bloom_[1].target.Get()};
    const float black[4]{};
    context->ClearRenderTargetView(maps[0], black);
    context->ClearRenderTargetView(maps[1], black);
    context->OMSetRenderTargets(2, maps, nullptr);
    context->RSSetViewports(1, &reduced);
    drawStars(lightSourcePs_.Get());
  }
  pass(hdr_.target.Get(), full, scenePs_.Get());
  if (scene.hasEmissionMaps > 0) {
    ID3D11ShaderResourceView *emissions[] = {bloom_[0].view.Get(), bloom_[1].view.Get()};
    context->PSSetShaderResources(2, 2, emissions);
  }
  context->Draw(3, 0);
  ID3D11ShaderResourceView *emptyEmissions[2]{};
  context->PSSetShaderResources(2, 2, emptyEmissions);
  if (visibleStars)
    drawStars(starPs_.Get());

  // The first blur also extracts highlights and downsamples to a quarter of the output size.
  pass(bloom_[0].target.Get(), reduced, bloomHorizontal_.Get());
  auto *source = hdr_.view.Get();
  context->PSSetShaderResources(2, 1, &source);
  context->Draw(3, 0);
  pass(bloom_[1].target.Get(), reduced, bloomVertical_.Get());
  source = bloom_[0].view.Get();
  context->PSSetShaderResources(2, 1, &source);
  context->Draw(3, 0);
  pass(output.Get(), full, composite_.Get());
  ID3D11ShaderResourceView *finish[] = {hdr_.view.Get(), bloom_[1].view.Get()};
  context->PSSetShaderResources(2, 2, finish);
  context->Draw(3, 0);
  ID3D11ShaderResourceView *unbound[5]{};
  context->PSSetShaderResources(0, 5, unbound);
  ID3D11Buffer *empty = nullptr;
  context->PSSetConstantBuffers(0, 1, &empty);
  return true;
}
} // namespace action75
