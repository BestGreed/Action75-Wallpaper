// Original deterministic FireworksScene seed generation and motion, D3D12 submission.
#include "Fireworks12.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
namespace action75 {
namespace {
#include "PatternData.inc"
} // namespace

using Microsoft::WRL::ComPtr;
namespace {
D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
  d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
bool buffer(ID3D12Device*device,UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>&out) {
  D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CreationNodeMask=h.VisibleNodeMask=1;
  const auto d=bufferDesc(bytes);
  return SUCCEEDED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
void barrier(ID3D12GraphicsCommandList*list,ID3D12Resource*resource,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  v.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
}
void viewport12(ID3D12GraphicsCommandList*list,unsigned w,unsigned h) {
  const D3D12_VIEWPORT viewport{0,0,float(w),float(h),0,1};const D3D12_RECT rect{0,0,LONG(w),LONG(h)};
  list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&rect);
}
}

D3D12_CPU_DESCRIPTOR_HANDLE Fireworks12::rtv(unsigned i)const {
  auto h=renderViews_->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*rtvStride_;return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Fireworks12::srv(unsigned i)const {
  auto h=resources_->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*srvStride_;return h;
}

bool Fireworks12::load(ID3D12Device*device,unsigned width,unsigned height,const std::filesystem::path&pack) {
  if(!device)return false;device_=device;
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R11G11B10_FLOAT};
  const auto required=D3D12_FORMAT_SUPPORT1_BLENDABLE|D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE|D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
  if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) &&
    (support.Support1&required)==required)format_=DXGI_FORMAT_R11G11B10_FLOAT;
  std::array<D3D12_ROOT_PARAMETER,4> params{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor={0,0};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={1,0,4};
  params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[2].DescriptorTable={1,&range};
  params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[3].Descriptor={3,0};
  params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sampler.MaxLOD=D3D12_FLOAT32_MAX;
  sampler.ShaderRegister=0;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{UINT(params.size()),params.data(),1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rootBytes;
  if(FAILED(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&rootBytes,nullptr)) ||
    FAILED(device->CreateRootSignature(0,rootBytes->GetBufferPointer(),rootBytes->GetBufferSize(),IID_PPV_ARGS(&root_))))return false;
  const auto code=[&](const wchar_t*name){std::ifstream input(pack/L"shaders"/name,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(input),{});};
  const auto sparkVs=code(L"spark-vs.cso"),screenVs=code(L"screen-vs.cso"),sparkPs=code(L"spark-ps.cso"),
    blurPs=code(L"blur-ps.cso"),compositePs=code(L"composite-ps.cso"),idlePs=code(L"idle-ps.cso");
  const auto pipeline=[&](const std::vector<char>&vs,const std::vector<char>&ps,DXGI_FORMAT format,bool additive,ComPtr<ID3D12PipelineState>&out) {
    if(vs.empty()||ps.empty())return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};d.pRootSignature=root_.Get();d.VS={vs.data(),vs.size()};d.PS={ps.data(),ps.size()};
    d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;d.RasterizerState.DepthClipEnable=TRUE;
    auto&b=d.BlendState.RenderTarget[0];b.BlendEnable=additive;b.SrcBlend=D3D12_BLEND_ONE;
    b.DestBlend=additive?D3D12_BLEND_ONE:D3D12_BLEND_ZERO;b.BlendOp=D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlendAlpha=D3D12_BLEND_ZERO;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;d.SampleMask=UINT_MAX;
    d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;d.NumRenderTargets=1;d.RTVFormats[0]=format;d.SampleDesc.Count=1;
    return SUCCEEDED(device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&out)));
  };
  if(!pipeline(sparkVs,sparkPs,format_,true,spark_)||!pipeline(screenVs,blurPs,format_,false,blur_)||
    !pipeline(screenVs,compositePs,DXGI_FORMAT_R8G8B8A8_UNORM,false,composite_)||
    !pipeline(screenVs,idlePs,DXGI_FORMAT_R8G8B8A8_UNORM,false,idle_))return false;
  D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3};
  if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&renderViews_))))return false;
  views={D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  if(FAILED(device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&resources_))))return false;
  rtvStride_=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  srvStride_=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const auto seeds=generate();const auto bytes=seeds.size()*sizeof(Spark);
  for(unsigned shape=0;shape<shapeCount;++shape)for(unsigned i=0;i<patternSize;++i){const auto&s=seeds[shape*patternSize+i];timings_[shape][i]={s.origin[3],s.burn[0],s.burn[2],0};}
  if(!buffer(device,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,patterns_)||
    !buffer(device,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,patternUpload_)||
    !buffer(device,(sizeof(Frame)+255)&~UINT64(255),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,frame_))return false;
  void*mapped=nullptr;const D3D12_RANGE empty{0,0};
  if(FAILED(patternUpload_->Map(0,&empty,&mapped)))return false;std::memcpy(mapped,seeds.data(),bytes);patternUpload_->Unmap(0,nullptr);
  if(FAILED(frame_->Map(0,&empty,reinterpret_cast<void**>(&constants_))))return false;
  std::memset(constants_,0,(sizeof(Frame)+255)&~size_t(255));
  configure(PackAppearance{});return resize(width,height);
}

bool Fireworks12::resize(unsigned width,unsigned height) {
  if(width_==width&&height_==height&&light_[0])return true;
  const unsigned smallWidth=std::max(1u,(width+3)/4),smallHeight=std::max(1u,(height+3)/4);
  std::array<ComPtr<ID3D12Resource>,3> next;
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
  for(unsigned i=0;i<3;++i){
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=i?smallWidth:width;d.Height=i?smallHeight:height;
    d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format_;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};clear.Format=format_;
    if(FAILED(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&next[i]))))return false;
  }
  light_=std::move(next);width_=width;height_=height;blurWidth_=smallWidth;blurHeight_=smallHeight;
  auto h=resources_->GetCPUDescriptorHandleForHeapStart();
  for(unsigned i=0;i<3;++i){device_->CreateRenderTargetView(light_[i].Get(),nullptr,rtv(i));
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format_;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(light_[i].Get(),&d,h);h.ptr+=srvStride_;}
  D3D12_SHADER_RESOURCE_VIEW_DESC nil{};nil.Format=format_;nil.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;nil.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;nil.Texture2D.MipLevels=1;
  device_->CreateShaderResourceView(nullptr,&nil,h);return true;
}

#include "Simulation.inc"

void Fireworks12::filter(ID3D12GraphicsCommandList*list,unsigned input,unsigned output,const Float4&step) {
  barrier(list,light_[output].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  const auto target=rtv(output);list->OMSetRenderTargets(1,&target,FALSE,nullptr);
  list->SetGraphicsRoot32BitConstants(1,4,step.data(),0);list->SetGraphicsRootDescriptorTable(2,srv(input));list->DrawInstanced(3,1,0,0);
  barrier(list,light_[output].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool Fireworks12::draw(const A75Frame12&f,double now) {
  if(!resize(f.width,f.height))return false;
  discardExpired(now);
  constexpr unsigned requested[]{152,308,patternSize},budget[]{2200,3800,maxVisibleSparks};
  const auto quality=std::min(appearance_.quality,2u);
  const unsigned particles=count_?std::min(static_cast<unsigned>(static_cast<float>(requested[quality])*appearance_.density),budget[quality]/count_):0;
  visibleSparks_=particles*count_;const auto w=float(f.width),h=float(f.height);
  data_.screen={w,h,float(std::fmod(now,120.0)),appearance_.horizon};
  data_.exposure={float(particles),brightness_*intensity_,appearance_.embers,appearance_.shimmer};
  data_.atmosphere={appearance_.bloom,appearance_.smoke,appearance_.reflection,0};
  const float pressure=1+.016f*float(count_>6?count_-6:0);
  for(unsigned i=0;i<count_;++i){const auto&burst=bursts_[i];auto&record=data_.burst[i];record=burst.render;
    record.location[2]=float(now-burst.time)*burst.speed;const float edge=float(std::max(0.0,burst.expires-now));
    record.rotation[3]=std::min(1.f,edge*5)/pressure;record.palette={float(burst.paletteSlot*colorsPerPalette),float(paletteSizes_[burst.paletteSlot]),0,0};}
  std::memcpy(constants_,&data_,offsetof(Frame,burst)+count_*sizeof(DrawBurst));
  auto*list=f.commands;
  if(!uploaded_){list->CopyBufferRegion(patterns_.Get(),0,patternUpload_.Get(),0,patternUpload_->GetDesc().Width);
    barrier(list,patterns_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploaded_=true;}
  list->SetGraphicsRootSignature(root_.Get());list->SetGraphicsRootConstantBufferView(0,frame_->GetGPUVirtualAddress());
  list->SetGraphicsRootShaderResourceView(3,patterns_->GetGPUVirtualAddress());
  ID3D12DescriptorHeap*heaps[]={resources_.Get()};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootDescriptorTable(2,srv(0));
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);viewport12(list,f.width,f.height);
  if(!visibleSparks_){list->OMSetRenderTargets(1,&f.rtv,FALSE,nullptr);list->SetPipelineState(idle_.Get());list->DrawInstanced(3,1,0,0);return true;}
  barrier(list,light_[0].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  const auto target=rtv(0);list->OMSetRenderTargets(1,&target,FALSE,nullptr);const float dark[4]{};list->ClearRenderTargetView(target,dark,0,nullptr);
  list->SetPipelineState(spark_.Get());list->DrawInstanced(42,visibleSparks_,0,0);
  barrier(list,light_[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  if(appearance_.bloom>0||appearance_.smoke>0){list->SetPipelineState(blur_.Get());viewport12(list,blurWidth_,blurHeight_);
    filter(list,0,1,{1.f/w,1.f/h,1,0});filter(list,1,2,{.8f/float(blurWidth_),0,0,0});filter(list,2,1,{0,.8f/float(blurHeight_),0,0});}
  list->OMSetRenderTargets(1,&f.rtv,FALSE,nullptr);viewport12(list,f.width,f.height);
  list->SetPipelineState(composite_.Get());list->SetGraphicsRootDescriptorTable(2,srv(0));list->DrawInstanced(3,1,0,0);return true;
}
} // namespace action75
