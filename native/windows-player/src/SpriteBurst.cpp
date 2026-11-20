#include "SpriteBurst.h"
#ifdef ACTION75_PACKED_ATLAS
#include "AtlasShaderBytes.h"
#else
#include "SpriteBurstShader.h"
#endif
#include <wincodec.h>
#include <d3dcompiler.h>
#include <cstring>
#include <vector>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace action75 {
namespace {
constexpr std::size_t Count = SpriteBurstState::capacity;
static_assert(Count == 24 && sizeof(SpriteBurstSample) == 16);
struct alignas(16) Constants {
  float width, height, diameter, invColumns;
  float invRows, atlasWidth, atlasHeight, columns;
  std::array<float, 4> tint;
  std::array<float, 4> grading;
  std::array<SpriteBurstSample, Count> samples;
  std::array<std::array<float,4>,Count> bounds;
  std::array<std::array<float,4>,Count> palettes;
};
static_assert(sizeof(Constants) == 1216);



std::string failure(const char *where, HRESULT hr) {
  char code[16]{};
  std::snprintf(code, sizeof(code), "%08lx", static_cast<unsigned long>(hr));
  return std::string(where) + " (0x" + code + ")";
}
}

bool SpriteBurst::valid(const Params &p) noexcept {
  return p.columns >= 1 && p.columns <= 12 && p.rows >= 1 && p.rows <= 12 &&
         p.columns * p.rows >= 2 && p.columns * p.rows <= 96 &&
         std::isfinite(p.duration) && p.duration >= .12f && p.duration <= 3.0f &&
         std::isfinite(p.diameter) && p.diameter >= 48 && p.diameter <= 440;
}

bool SpriteBurst::load(ID3D11Device *device, const std::filesystem::path &file,
                       Params params, std::string &error, bool forceAtlas2D) {
#ifdef ACTION75_PACKED_ATLAS
  forceAtlas2D = true;
#endif
  if (!device || !valid(params)) { error = "Invalid sprite-burst parameters"; return false; }
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  UINT width = 0, height = 0;
  if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
  if (FAILED(hr)) { error = failure("Open burst atlas", hr); return false; }
  if (!width || !height || width > 2048 || height > 2048 ||
      width % params.columns || height % params.rows ||
      width / params.columns != height / params.rows || width / params.columns < 16) {
    error = "Atlas must be at most 2048x2048, with equal square tiles of at least 16px"; return false;
  }
  hr = wic->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr)) hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                   WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) { error = failure("Convert burst atlas", hr); return false; }
  std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4);
  hr = converter->CopyPixels(nullptr, width*4, static_cast<UINT>(pixels.size()), pixels.data());
  if (FAILED(hr)) { error = failure("Decode burst atlas", hr); return false; }
  const UINT tile=width/params.columns;
  const auto range=[&](UINT first,UINT last){
    // Expand by two source pixels: covers filtering without clipping a soft edge.
    const auto minV=(first>2?first-2:0);
    const auto maxV=std::min(tile-1,last+2);
    return std::array<float,2>{static_cast<float>(minV)/(tile-1),static_cast<float>(maxV)/(tile-1)};
  };
  for (UINT i=0;i<params.columns*params.rows;++i) {
    const UINT ox=(i%params.columns)*tile,oy=(i/params.columns)*tile;
    UINT minX=tile,minY=tile,maxX=0,maxY=0;
    for (UINT y=0; y<tile; ++y) {
      const auto *row = pixels.data() + (static_cast<std::size_t>(oy+y)*width+ox)*4;
      const auto lit = [row](UINT x) { return row[x*4] | row[x*4+1] | row[x*4+2]; };
      UINT first = 0;
      while (first < tile && !lit(first)) ++first;
      if (first == tile) continue;
      minX = std::min(minX, first);
      minY = std::min(minY, y);
      maxY = y;
      // Only pixels to the right of the known bound can extend it.
      UINT last = tile-1;
      while (last > maxX && last > first && !lit(last)) --last;
      maxX = std::max(maxX, last);
    }
    if (minX==tile) bounds_[i]={1,1,0,0}; // Empty box identity.
    else {
      const auto x=range(minX,maxX), y=range(minY,maxY);
      bounds_[i]={x[0],y[0],x[1],y[1]};
    }
  }
  const UINT frameCount=params.columns*params.rows;
  UINT support=0;
  if (!forceAtlas2D && SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM,&support)) &&
      (support&D3D11_FORMAT_SUPPORT_TEXTURE3D) && (support&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) {
    // Repack at selection time; steady-state width*height*4 logical bytes
    // remain unchanged. The asset format remains the small 2D PNG atlas.
    const UINT rowBytes=tile*4, sliceBytes=rowBytes*tile;
    std::vector<unsigned char> slices(static_cast<std::size_t>(sliceBytes)*frameCount);
    for (UINT i=0;i<frameCount;++i) {
      const UINT ox=(i%params.columns)*tile,oy=(i/params.columns)*tile;
      for (UINT y=0;y<tile;++y) {
        auto *dst=slices.data()+static_cast<std::size_t>(sliceBytes)*i+static_cast<std::size_t>(rowBytes)*y;
        const auto *src=pixels.data()+(static_cast<std::size_t>(oy+y)*width+ox)*4;
        std::memcpy(dst,src,rowBytes);
      }
    }
    D3D11_TEXTURE3D_DESC td{};
    td.Width=td.Height=tile; td.Depth=frameCount; td.MipLevels=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage=D3D11_USAGE_IMMUTABLE;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem=slices.data(); initial.SysMemPitch=rowBytes; initial.SysMemSlicePitch=sliceBytes;
    ComPtr<ID3D11Texture3D> gpu;
    hr=device->CreateTexture3D(&td,&initial,&gpu);
    if (SUCCEEDED(hr)) hr=device->CreateShaderResourceView(gpu.Get(),nullptr,&texture_);
    volumeAtlas_=SUCCEEDED(hr);
  }
  if (!volumeAtlas_) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width=width; td.Height=height; td.ArraySize=1; td.MipLevels=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_IMMUTABLE; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=pixels.data(); initial.SysMemPitch=width*4;
    ComPtr<ID3D11Texture2D> gpu;
    hr=device->CreateTexture2D(&td,&initial,&gpu);
    if (SUCCEEDED(hr)) hr=device->CreateShaderResourceView(gpu.Get(),nullptr,&texture_);
    if (FAILED(hr)) {error=failure("Upload burst atlas",hr);return false;}
  }
#ifdef ACTION75_PACKED_ATLAS
  hr = device->CreateVertexShader(AtlasVS, sizeof(AtlasVS), nullptr, &vs_);
  if (SUCCEEDED(hr)) hr = device->CreatePixelShader(AtlasPS, sizeof(AtlasPS), nullptr, &ps_);
  if (FAILED(hr)) { error = failure("Create burst shader", hr); return false; }
#else
  const D3D_SHADER_MACRO legacy[]={{"SPRITE_ATLAS_2D","1"},{nullptr,nullptr}};
  for (const bool vertex : {true,false}) {
    ComPtr<ID3DBlob> shader, messages;
    hr = D3DCompile(SpriteBurstShader, sizeof(SpriteBurstShader)-1, "SpriteBurst", volumeAtlas_?nullptr:legacy, nullptr,
        vertex ? "vs" : "ps", vertex ? "vs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &messages);
    if (FAILED(hr)) {
      if (volumeAtlas_) {
        texture_.Reset(); vs_.Reset(); ps_.Reset(); volumeAtlas_=false;
        return load(device,file,params,error,true);
      }
      error = failure("Compile burst shader", hr);
      if (messages) error += ": " + std::string(static_cast<const char*>(messages->GetBufferPointer()),messages->GetBufferSize());
      return false;
    }
    if (vertex) hr = device->CreateVertexShader(shader->GetBufferPointer(),shader->GetBufferSize(),nullptr,&vs_);
    else hr = device->CreatePixelShader(shader->GetBufferPointer(),shader->GetBufferSize(),nullptr,&ps_);
    if (FAILED(hr)) {
      if (volumeAtlas_) {
        texture_.Reset(); vs_.Reset(); ps_.Reset(); volumeAtlas_=false;
        return load(device,file,params,error,true);
      }
      error = failure("Create burst shader", hr); return false;
    }
  }
#endif
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth=static_cast<UINT>(sizeof(Constants)); cb.Usage=D3D11_USAGE_DYNAMIC;
  cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
  hr=device->CreateBuffer(&cb,nullptr,&constants_);
  D3D11_SAMPLER_DESC sample{};
  sample.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  sample.MaxLOD=D3D11_FLOAT32_MAX; sample.MaxAnisotropy=1; sample.ComparisonFunc=D3D11_COMPARISON_NEVER;
  if (SUCCEEDED(hr)) hr=device->CreateSamplerState(&sample,&sampler_);
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
  if (SUCCEEDED(hr)) hr=device->CreateRasterizerState(&raster,&raster_);
  D3D11_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable=FALSE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
  if (SUCCEEDED(hr)) hr=device->CreateDepthStencilState(&depth,&depth_);
  D3D11_BLEND_DESC blend{};
  auto &rt=blend.RenderTarget[0];
  rt.BlendEnable=TRUE;
  rt.SrcBlend=params.multiply ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
  rt.DestBlend=params.multiply ? D3D11_BLEND_INV_SRC_COLOR : D3D11_BLEND_ONE;
  rt.BlendOp=D3D11_BLEND_OP_ADD;
  rt.SrcBlendAlpha=D3D11_BLEND_ZERO; rt.DestBlendAlpha=D3D11_BLEND_ONE; rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
  if (SUCCEEDED(hr)) hr=device->CreateBlendState(&blend,&blend_);
  if (FAILED(hr)) { error=failure("Create burst pipeline",hr); return false; }
  p_=params; atlasWidth_=static_cast<float>(width); atlasHeight_=static_cast<float>(height);
  return true;
}

bool SpriteBurst::draw(ID3D11DeviceContext *c,unsigned width,unsigned height,
                       double now,std::string &error) {
  Constants values{static_cast<float>(width),static_cast<float>(height),
    p_.diameter*static_cast<float>(height)/720.0f*scale_,1.0f/static_cast<float>(p_.columns),
    1.0f/static_cast<float>(p_.rows),atlasWidth_,atlasHeight_,static_cast<float>(p_.columns),tint_,grading_,{},{},{}};
  values.tint[3]=p_.multiply ? 1.0f : 0.0f;
  const auto frames=p_.columns*p_.rows;
  std::array<std::uint32_t,Count> paletteIndices{};
  const auto n=state_.sample(now,duration(),frames,values.samples,&paletteIndices);
  if (!n) return true;
  UINT drawn=0;
  for (std::size_t i=0;i<n;++i) {
    const auto pos=static_cast<UINT>(values.samples[i].frame);
    const auto &a=bounds_[pos], &b=bounds_[std::min(frames-1,pos+1)];
    const std::array<float,4> bounds={std::min(a[0],b[0]),std::min(a[1],b[1]),
       std::max(a[2],b[2]),std::max(a[3],b[3])};
    if (bounds[0]>=bounds[2] || bounds[1]>=bounds[3]) continue;
    values.samples[drawn]=values.samples[i];
    // Reuse the existing shader gain, preserving atlas data and draw count.
    values.samples[drawn].gain *= brightness_ * intensity_;
    values.palettes[drawn]=palette_.empty() ? std::array<float,4>{1,1,1,0}
                                         : palette_[paletteIndices[i]%palette_.size()];
    values.bounds[drawn++]=bounds;
  }
  if (!drawn) return true;
  D3D11_MAPPED_SUBRESOURCE mapped{};
  HRESULT hr=c->Map(constants_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
  if (FAILED(hr)) { error=failure("Update burst constants",hr); return false; }
  std::memcpy(mapped.pData,&values,sizeof values);
  c->Unmap(constants_.Get(),0);
  c->RSSetState(raster_.Get());
  c->OMSetDepthStencilState(depth_.Get(),0);
  c->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->GSSetShader(nullptr,nullptr,0);
  c->HSSetShader(nullptr,nullptr,0);
  c->DSSetShader(nullptr,nullptr,0);
  c->VSSetShader(vs_.Get(),nullptr,0);
  c->PSSetShader(ps_.Get(),nullptr,0);
  auto *b=constants_.Get(); auto *s=sampler_.Get(); auto *tex=texture_.Get();
  c->VSSetConstantBuffers(0,1,&b);
  c->PSSetConstantBuffers(0,1,&b);
  c->PSSetSamplers(0,1,&s);
  c->PSSetShaderResources(0,1,&tex);
  c->DrawInstanced(4,drawn,0,0);
  ID3D11ShaderResourceView *nullView=nullptr;
  c->PSSetShaderResources(0,1,&nullView);
  c->OMSetBlendState(nullptr,nullptr,0xffffffff);
  c->OMSetDepthStencilState(nullptr,0);
  return true;
}
void SpriteBurst::setColorGrade(float saturation, float hueDegrees,
                                const std::vector<std::array<std::uint8_t,3>>& palette,
                                bool usePalette) {
  const float sat=std::isfinite(saturation) ? std::clamp(saturation,0.0f,2.5f) : 1.0f;
  const float hue=std::isfinite(hueDegrees) ? std::clamp(hueDegrees,-180.0f,180.0f) : 0.0f;
  const float radians=hue*.01745329251994329577f;
  grading_={sat,std::cos(radians),std::sin(radians),hue!=0 ? 1.0f : 0.0f};
  palette_.clear();
  if (usePalette) for(std::size_t i=0;i<(std::min)(palette.size(),std::size_t{9});++i)
    // These sprite atlases contain display-encoded emissive RGB. Paintball's
    // custom material instead receives linear targets in EffekseerBackend.
    palette_.push_back({palette[i][0]/255.0f,palette[i][1]/255.0f,palette[i][2]/255.0f,1});
}
}
