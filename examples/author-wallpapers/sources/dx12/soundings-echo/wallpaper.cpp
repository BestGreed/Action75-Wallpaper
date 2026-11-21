#include "Action75Wallpaper12.h"
#include "EchoState.hpp"
#include "Image.hpp"
#include <nlohmann/json.hpp>
#include <wrl/client.h>
#include <array>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

namespace soundings {
using Microsoft::WRL::ComPtr;
using Bound=std::array<float,4>;
struct alignas(16) Constants {
    float width,height,diameter,invColumns;
    float invRows,atlasWidth,atlasHeight,columns;
    Bound tint{1,1,1,0},opacity{1,1,0,0};
    std::array<Sample,24> samples{};
    std::array<Bound,24> bounds{},palettes{};
    Bound backgroundAppearance{},backgroundTint{};
};
static_assert(sizeof(Constants)==1248);
inline D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};p.Type=type;p.CreationNodeMask=p.VisibleNodeMask=1;return p;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
    d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
class Echo {
public:
    void initialize(const A75Create12& create) {
        device_=create.device;
        const auto dir=std::filesystem::path(create.pack_dir);
        const auto atlas=readImage(dir/L"echo.png");
        if(atlas.width%6||atlas.height%4||atlas.width/6!=atlas.height/4||atlas.width/6<16) throw std::runtime_error("Original echo atlas is not 6x4 square frames");
        atlasWidth_=float(atlas.width);atlasHeight_=float(atlas.height);const UINT tile=atlas.width/6;
        std::vector<std::uint8_t> slices(std::size_t(tile)*tile*24*4);
        for(UINT i=0;i<24;++i) {
            UINT minX=tile,minY=tile,maxX=0,maxY=0;
            for(UINT y=0;y<tile;++y) {
                auto* dst=slices.data()+(std::size_t(i)*tile+y)*tile*4;
                const auto* src=atlas.rgba.data()+(std::size_t(i/6*tile+y)*atlas.width+i%6*tile)*4;
                std::memcpy(dst,src,tile*4);
                UINT first=0;while(first<tile && !(src[first*4]|src[first*4+1]|src[first*4+2])) ++first;
                if(first==tile) continue;
                minX=std::min(minX,first);minY=std::min(minY,y);maxY=y;
                UINT last=tile-1;while(last>maxX && last>first && !(src[last*4]|src[last*4+1]|src[last*4+2])) --last;
                maxX=std::max(maxX,last);
            }
            if(minX==tile) bounds_[i]={1,1,0,0};
            else bounds_[i]={float(minX>2?minX-2:0)/(tile-1),float(minY>2?minY-2:0)/(tile-1),
                             float(std::min(tile-1,maxX+2))/(tile-1),float(std::min(tile-1,maxY+2))/(tile-1)};
        }
        const auto backdrop=readImage(dir/L"depths.png");
        D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;h.NumDescriptors=2;h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        checked(device_->CreateDescriptorHeap(&h,IID_PPV_ARGS(&descriptors_)),"Create author texture descriptors");
        ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
        D3D12_COMMAND_QUEUE_DESC q{};checked(device_->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Create one-time texture upload queue");
        checked(device_->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Create upload allocator");
        checked(device_->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Create upload command list");
        std::vector<ComPtr<ID3D12Resource>> staging;
        upload(commands.Get(),tile,tile,24,slices,true,0,textures_[0],staging);
        upload(commands.Get(),backdrop.width,backdrop.height,1,backdrop.rgba,false,1,textures_[1],staging);
        checked(commands->Close(),"Close upload commands");ID3D12CommandList* work[]={commands.Get()};queue->ExecuteCommandLists(1,work);
        ComPtr<ID3D12Fence> fence;checked(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create upload fence");
        checked(queue->Signal(fence.Get(),1),"Signal texture upload");checked(fence->SetEventOnCompletion(1,nullptr),"Wait for texture upload");

        D3D12_DESCRIPTOR_RANGE textures{};textures.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;textures.NumDescriptors=2;
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor={0,0};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&textures};
        parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD=FLT_MAX;sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;checked(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"Serialize author root signature");
        checked(device_->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_)),"Create author root signature");
        pipeline(dir,L"backgroundVS.cso",L"backgroundPS.cso",false,backgroundPipeline_);
        pipeline(dir,L"echoVS.cso",L"echoPS.cso",true,echoPipeline_);
        pipeline(dir,L"echoVS.cso",L"echoPS.cso",true,echoOverPipeline_,true);
        auto properties=heap(D3D12_HEAP_TYPE_UPLOAD);const auto desc=buffer((sizeof(Constants)+255)&~UINT64(255));
        checked(device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&constants_)),"Create frame constants");
        D3D12_RANGE none{};checked(constants_->Map(0,&none,&mapped_ ),"Map frame constants");
        configure(create.settings_json);
    }
    ~Echo() {if(constants_&&mapped_) constants_->Unmap(0,nullptr);}
    void event(const A75Event& event) {
        switch(event.type) {
        case A75_KEY_DOWN:if(!paused_) state_.push(clock(event.seconds),event.x,event.y);break;
        case A75_SETTINGS:configure(event.settings_json);break;
        case A75_PAUSE:if(!paused_) {pausedAt_=event.seconds;paused_=true;}break;
        case A75_RESUME:if(paused_) {offset_+=std::max(0.,event.seconds-pausedAt_);paused_=false;}break;
        case A75_INPUT_RESET:state_.clear();break;
        default:break;
        }
    }
    int draw(const A75Frame12& frame) {
        Constants c{};c.width=float(frame.width);c.height=float(frame.height);
        c.diameter=300.f*float(frame.height)/720.f*reach_;c.invColumns=1.f/6;c.invRows=1.f/4;
        c.atlasWidth=atlasWidth_;c.atlasHeight=atlasHeight_;c.columns=6;
        c.backgroundAppearance={brightness_,saturation_,hue_,backgroundEnabled_};
        c.opacity={backgroundOpacity_,foregroundOpacity_,0,0};
        c.backgroundTint={color_[0]/255.f,color_[1]/255.f,color_[2]/255.f,solid_?1.f:0.f};
        const unsigned live=state_.sample(clock(frame.seconds),c.samples);unsigned drawn=0;
        for(unsigned i=0;i<live;++i) {
            const auto f=unsigned(c.samples[i].frame);
            const auto& a=bounds_[f];const auto& b=bounds_[std::min(23u,f+1)];
            Bound box{std::min(a[0],b[0]),std::min(a[1],b[1]),std::max(a[2],b[2]),std::max(a[3],b[3])};
            if(box[0]>=box[2]||box[1]>=box[3]) continue;
            c.samples[drawn]=c.samples[i];c.bounds[drawn++]=box;
        }
        std::memcpy(mapped_,&c,sizeof c);
        auto* list=frame.commands;ID3D12DescriptorHeap* heaps[]={descriptors_.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetGraphicsRootSignature(root_.Get());list->SetGraphicsRootConstantBufferView(0,constants_->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(1,descriptors_->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(backgroundPipeline_.Get());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        if(drawn && foregroundOpacity_>0) {list->SetPipelineState(backgroundOpacity_==1&&foregroundOpacity_==1?echoPipeline_.Get():echoOverPipeline_.Get());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);list->DrawInstanced(4,drawn,0,0);}
        return live&&foregroundOpacity_>0&&!paused_?0:-1;
    }
private:
    double clock(double seconds) const noexcept {return (paused_?pausedAt_:seconds)-offset_;}
    void configure(const char* text) {
        if(!text) return;const auto v=nlohmann::json::parse(text);
        reach_=std::clamp(v.value("reach",reach_),.5f,1.4f);
        backgroundOpacity_=std::clamp(v.value("backgroundOpacity",backgroundOpacity_*100.f),0.f,100.f)/100.f;
        foregroundOpacity_=std::clamp(v.value("foregroundOpacity",foregroundOpacity_*100.f),0.f,100.f)/100.f;
        brightness_=std::clamp(v.value("backgroundBrightness",brightness_),0.f,2.f);
        saturation_=std::clamp(v.value("backgroundSaturation",saturation_),0.f,2.f);
        solid_=v.value("backgroundStyle",solid_?1:0)==1;
        if(v.contains("backgroundColor")) {const auto color=v.at("backgroundColor").get<std::array<int,3>>();for(unsigned i=0;i<3;++i) color_[i]=std::uint8_t(std::clamp(color[i],0,255));}
        if(v.contains("backgroundHue")) {hue_=std::fmod(v.at("backgroundHue").get<float>(),360.f);if(hue_<0)hue_+=360;hue_/=360;}
        backgroundEnabled_=brightness_!=1||saturation_!=1||hue_!=0||color_!=std::array<std::uint8_t,3>{255,255,255}?1.f:0.f;
    }
    void upload(ID3D12GraphicsCommandList* commands,UINT width,UINT height,UINT depth,const std::vector<std::uint8_t>& pixels,bool volume,UINT slot,
                ComPtr<ID3D12Resource>& texture,std::vector<ComPtr<ID3D12Resource>>& staging) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=volume?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=width;desc.Height=height;desc.DepthOrArraySize=UINT16(depth);desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        auto properties=heap(D3D12_HEAP_TYPE_DEFAULT);checked(device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"Create original image texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device_->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        properties=heap(D3D12_HEAP_TYPE_UPLOAD);const auto uploadDesc=buffer(bytes);ComPtr<ID3D12Resource> source;
        checked(device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&uploadDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source)),"Create image upload");
        void* mapped=nullptr;D3D12_RANGE none{};checked(source->Map(0,&none,&mapped),"Map image upload");
        for(UINT z=0;z<depth;++z) for(UINT y=0;y<height;++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped)+footprint.Offset+(std::size_t(z)*height+y)*footprint.Footprint.RowPitch,
                        pixels.data()+(std::size_t(z)*height+y)*width*4,width*4);
        source->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=source.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition={texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};commands->ResourceBarrier(1,&barrier);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(volume) {view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;view.Texture3D.MipLevels=1;} else {view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;}
        auto handle=descriptors_->GetCPUDescriptorHandleForHeapStart();handle.ptr+=std::size_t(slot)*device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_->CreateShaderResourceView(texture.Get(),&view,handle);staging.push_back(source);
    }
    void pipeline(const std::filesystem::path& dir,const wchar_t* vertex,const wchar_t* pixel,bool additive,ComPtr<ID3D12PipelineState>& pipeline,bool over=false) {
        const auto read=[&](const wchar_t* name) {std::ifstream file(dir/L"shaders"/name,std::ios::binary);return std::vector<char>(std::istreambuf_iterator<char>(file),{});};
        const auto vs=read(vertex),ps=read(pixel);if(vs.empty()||ps.empty()) throw std::runtime_error("Precompiled author shader missing");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};state.pRootSignature=root_.Get();state.VS={vs.data(),vs.size()};state.PS={ps.data(),ps.size()};
        state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;state.RasterizerState.DepthClipEnable=TRUE;
        auto& blend=state.BlendState.RenderTarget[0];blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;blend.BlendEnable=additive;
        blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=over?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ONE;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=over?D3D12_BLEND_ONE:D3D12_BLEND_ZERO;blend.DestBlendAlpha=over?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ONE;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        state.SampleMask=UINT_MAX;state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;state.NumRenderTargets=1;
        state.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        state.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;state.SampleDesc.Count=1;
        checked(device_->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&pipeline)),"Create author DX12 pipeline");
    }
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;ComPtr<ID3D12PipelineState> backgroundPipeline_,echoPipeline_,echoOverPipeline_;
    ComPtr<ID3D12DescriptorHeap> descriptors_;std::array<ComPtr<ID3D12Resource>,2> textures_;
    ComPtr<ID3D12Resource> constants_;void* mapped_=nullptr;
    EchoState state_;std::array<Bound,24> bounds_{};
    float atlasWidth_=0,atlasHeight_=0,reach_=1,brightness_=1,saturation_=1,hue_=0,backgroundEnabled_=0;
    float backgroundOpacity_=1,foregroundOpacity_=1;
    std::array<std::uint8_t,3> color_{255,255,255};bool solid_=false,paused_=false;
    double pausedAt_=0,offset_=0;
};
}
namespace {
void* __cdecl create(const A75Create12* c) noexcept {
    if(!c||c->size<sizeof(*c)||!c->device||!c->pack_dir) return nullptr;
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try {auto instance=std::make_unique<soundings::Echo>();instance->initialize(*c);if(SUCCEEDED(initialized))CoUninitialize();return instance.release();}
    catch(...) {if(SUCCEEDED(initialized))CoUninitialize();return nullptr;}
}
void __cdecl event(void* instance,const A75Event* e) noexcept {if(instance&&e&&e->size>=sizeof(*e))try{static_cast<soundings::Echo*>(instance)->event(*e);}catch(...) {}}
int32_t __cdecl render(void* instance,const A75Frame12* frame) noexcept {
    if(!instance||!frame||frame->size<sizeof(*frame)||!frame->commands||!frame->width||!frame->height) return -2;
    try{return static_cast<soundings::Echo*>(instance)->draw(*frame);}catch(...){return -2;}
}
void __cdecl destroy(void* instance) noexcept {delete static_cast<soundings::Echo*>(instance);}
}
extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 api{sizeof(api),ACTION75_WALLPAPER_ABI12,create,event,render,destroy,nullptr};
    return abi==ACTION75_WALLPAPER_ABI12?&api:nullptr;
}
