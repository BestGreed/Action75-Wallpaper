#include "Action75Wallpaper12.h"
#include "EchoState.hpp"
#include "Image.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <nlohmann/json.hpp>
#include <cfloat>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#ifdef SOUNDINGS_LEGACY_COMPARE
#include "SpriteBurst.h"
#include <d3dcompiler.h>
#endif

using Microsoft::WRL::ComPtr;
using soundings::checked;
using Pixels=std::vector<std::uint8_t>;
static void require(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {D3D12_HEAP_PROPERTIES p{};p.Type=type;p.CreationNodeMask=p.VisibleNodeMask=1;return p;}
static D3D12_RESOURCE_DESC buffer(UINT64 bytes) {D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;}

struct Dx12 {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12Resource> target,readback;
    ComPtr<ID3D12DescriptorHeap> views;ComPtr<ID3D12InfoQueue> debug;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0,serial=0;
    HMODULE module=nullptr;const A75Wallpaper12* api=nullptr;void* instance=nullptr;
    UINT width=1920,height=720;
    Dx12(const std::filesystem::path& dir) {
        ComPtr<ID3D12Debug> layer;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create DXGI factory");
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"Get independent WARP adapter");
        checked(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Create native DX12 WARP device");
        device.As(&debug);D3D12_COMMAND_QUEUE_DESC q{};checked(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Create test queue");
        checked(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Create test allocator");
        checked(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create test list");checked(list->Close(),"Close initial test list");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create test fence");
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto p=heap(D3D12_HEAP_TYPE_DEFAULT);checked(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"Create test target");
        D3D12_DESCRIPTOR_HEAP_DESC v{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};checked(device->CreateDescriptorHeap(&v,IID_PPV_ARGS(&views)),"Create test RTV");
        device->CreateRenderTargetView(target.Get(),nullptr,views->GetCPUDescriptorHandleForHeapStart());
        device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);d=buffer(bytes);p=heap(D3D12_HEAP_TYPE_READBACK);
        checked(device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Create test readback");
        module=LoadLibraryW((dir/L"wallpaper.dll").c_str());require(module,"Load standalone package DLL");
        require(!GetProcAddress(module,"Action75GetWallpaper"),"Package must not export ABI1");
        const auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));
        require(get&&!get(1)&&(api=get(2))&&api->abi==2,"Negotiate ABI2 only");
        A75Create12 c{sizeof(c),width,height,device.Get(),dir.c_str(),"{}",nullptr,nullptr};instance=api->create(&c);require(instance,"Create author DX12 instance");
    }
    ~Dx12() {if(instance)api->destroy(instance);if(module)FreeLibrary(module);}
    void event(UINT type,double time,float x=.38f,float y=.57f,const std::string& settings="{}") {
        A75Event e{sizeof(e),type,width,height,time,x,y,"KeyG",settings.c_str()};api->on_event(instance,&e);
    }
    Pixels frame(double time,int expected) {
        checked(allocator->Reset(),"Reset test allocator");checked(list->Reset(allocator.Get(),nullptr),"Reset test command list");
        const auto rtv=views->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};D3D12_RECT sc{0,0,LONG(width),LONG(height)};
        list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);
        A75Frame12 f{sizeof(f),width,height,time,list.Get(),target.Get(),rtv};require(api->render(instance,&f)==expected,"Author frame scheduling differs");
        D3D12_RESOURCE_BARRIER b{};b.Transition={target.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=target.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
        checked(list->Close(),"Close test draw");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
        checked(queue->Signal(fence.Get(),++serial),"Signal test frame");checked(fence->SetEventOnCompletion(serial,nullptr),"Wait for completed frame");
        std::uint8_t* mapped=nullptr;D3D12_RANGE range{0,std::size_t(bytes)};checked(readback->Map(0,&range,reinterpret_cast<void**>(&mapped)),"Map test frame");
        Pixels pixels(std::size_t(width)*height*4);for(UINT y=0;y<height;++y)std::memcpy(pixels.data()+std::size_t(y)*width*4,mapped+std::size_t(y)*footprint.Footprint.RowPitch,width*4);
        D3D12_RANGE none{};readback->Unmap(0,&none);return pixels;
    }
    unsigned errors() {
        unsigned errors=0;if(!debug)return 0;
        for(UINT64 i=0;i<debug->GetNumStoredMessages();++i) {
            SIZE_T bytes=0;debug->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(data.data());
            if(SUCCEEDED(debug->GetMessage(i,msg,&bytes))&&(msg->Severity==D3D12_MESSAGE_SEVERITY_ERROR||msg->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)) {++errors;std::cerr<<msg->pDescription<<'\n';}
        }
        return errors;
    }
};

#ifdef SOUNDINGS_LEGACY_COMPARE
// Only this verification target links the frozen old DX11 SpriteBurst. The
// shipping author DLL has no DX11 implementation, private header or player lib.
struct Legacy {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;ComPtr<ID3D11ShaderResourceView> background;
    action75::SpriteBurst echoes;UINT width=1920,height=720;
    float brightness=1,saturation=1,hue=0;std::array<int,3> tint{255,255,255};bool solid=false;
    Legacy(const std::filesystem::path& assets) {
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create legacy comparison WARP");
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        checked(device->CreateTexture2D(&d,nullptr,&target),"Legacy target");checked(device->CreateRenderTargetView(target.Get(),nullptr,&view),"Legacy RTV");
        d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;checked(device->CreateTexture2D(&d,nullptr,&readback),"Legacy readback");
        const auto image=soundings::readImage(assets/L"depths.png");d.Width=image.width;d.Height=image.height;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_IMMUTABLE;d.CPUAccessFlags=0;
        D3D11_SUBRESOURCE_DATA initial{image.rgba.data(),image.width*4,0};ComPtr<ID3D11Texture2D> texture;
        checked(device->CreateTexture2D(&d,&initial,&texture),"Legacy original backdrop");checked(device->CreateShaderResourceView(texture.Get(),nullptr,&background),"Legacy backdrop view");
        const auto source=std::filesystem::path(SOUNDINGS_LEGACY_REFERENCE)/L"Background.hlsl";
        ComPtr<ID3DBlob> code,errors;checked(D3DCompileFromFile(source.c_str(),nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors),"Compile frozen background VS");
        checked(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs),"Legacy background VS");
        checked(D3DCompileFromFile(source.c_str(),nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors),"Compile frozen background PS");
        checked(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps),"Legacy background PS");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=32;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;checked(device->CreateBuffer(&cb,nullptr,&constants),"Legacy background constants");
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=FLT_MAX;s.MaxAnisotropy=1;s.ComparisonFunc=D3D11_COMPARISON_NEVER;
        checked(device->CreateSamplerState(&s,&sampler),"Legacy background sampler");
        std::string error;require(echoes.load(device.Get(),assets/L"echo.png",{6,4,.85f,300,false},error),error.c_str());require(echoes.volumeAtlas(),"Frozen legacy must use 3D atlas for comparison");
    }
    void settings(const nlohmann::json& v) {
        echoes.setScale(v.value("reach",1.f));brightness=v.value("backgroundBrightness",1.f);saturation=v.value("backgroundSaturation",1.f);
        hue=v.value("backgroundHue",0.f)/360.f;tint=v.value("backgroundColor",std::array<int,3>{255,255,255});solid=v.value("backgroundStyle",0)==1;
    }
    Pixels frame(double time) {
        auto* rtv=view.Get();context->OMSetRenderTargets(1,&rtv,nullptr);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
        context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
        if(solid) {
            const float r=tint[0]/255.f,g=tint[1]/255.f,b=tint[2]/255.f,peak=std::max({r,g,b}),chroma=peak-std::min({r,g,b});
            std::array<float,4> color{r,g,b,1};
            if(chroma>0&&(hue!=0||saturation!=1)) {
                float h=peak==r?(g-b)/chroma:peak==g?(b-r)/chroma+2:(r-g)/chroma+4;h=h/6+hue;
                const float sat=std::clamp(chroma/peak*saturation,0.f,1.f);constexpr float offsets[]={0,2.f/3,1.f/3};
                for(unsigned i=0;i<3;++i) {const float hv=h+offsets[i],pure=std::clamp(std::abs((hv-std::floor(hv))*6-3)-1,0.f,1.f);color[i]=peak*(1+sat*(pure-1));}
            }
            const float exposure=brightness<=1?brightness:brightness/(1+(brightness-1)*peak);for(unsigned i=0;i<3;++i)color[i]*=exposure;
            context->ClearRenderTargetView(rtv,color.data());
        } else {
            const bool enabled=brightness!=1||saturation!=1||hue!=0||tint!=std::array<int,3>{255,255,255};
            const float c[]={brightness,saturation,hue,enabled?1.f:0.f,tint[0]/255.f,tint[1]/255.f,tint[2]/255.f,0};context->UpdateSubresource(constants.Get(),0,nullptr,c,0,0);
            context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
            auto* cb=constants.Get();auto* tex=background.Get();auto* s=sampler.Get();context->PSSetConstantBuffers(0,1,&cb);context->PSSetShaderResources(0,1,&tex);context->PSSetSamplers(0,1,&s);context->Draw(3,0);
            ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
        }
        std::string error;require(echoes.draw(context.Get(),width,height,time,error),error.c_str());context->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Map legacy frame");
        Pixels pixels(std::size_t(width)*height*4);for(UINT y=0;y<height;++y)std::memcpy(pixels.data()+std::size_t(y)*width*4,static_cast<std::uint8_t*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,width*4);
        context->Unmap(readback.Get(),0);return pixels;
    }
};
#endif

int wmain(int argc,wchar_t** argv) {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);
    try {
        require(argc==3,"Pass candidate pack and evidence directory");const auto pack=std::filesystem::absolute(argv[1]),output=std::filesystem::absolute(argv[2]);
        Dx12 native(pack);nlohmann::json report={{"renderer","D3D12/WARP"},{"width",native.width},{"height",native.height},{"cases",nlohmann::json::array()}};
#ifdef SOUNDINGS_LEGACY_COMPARE
        Legacy legacy(pack);report["baseline"]="Frozen real SpriteBurst.cpp + original Background.hlsl, independent D3D11 WARP verification target only";
#endif
        auto compare=[&](const std::string& name,double time,int expected,bool save=false,double legacyTime=-1) {
            auto next=native.frame(time,expected);nlohmann::json item={{"name",name},{"seconds",time},{"schedule",expected}};
#ifdef SOUNDINGS_LEGACY_COMPARE
            auto prior=legacy.frame(legacyTime<0?time:legacyTime);unsigned maximum=0;std::uint64_t total=0,changed=0;
            for(std::size_t i=0;i<next.size();++i) {const auto difference=unsigned(std::abs(int(next[i])-int(prior[i])));maximum=std::max(maximum,difference);total+=difference;changed+=difference!=0;}
            item["maxChannelError"]=maximum;item["meanChannelError"]=double(total)/next.size();item["changedChannels"]=changed;
            if(save)soundings::writeImage(output/(name+"-legacy.png"),native.width,native.height,prior);
            if(maximum>1) {soundings::writeImage(output/(name+"-dx12.png"),native.width,native.height,next);report["cases"].push_back(item);std::ofstream(output/L"failed-comparison.json")<<report.dump(2);throw std::runtime_error("Legacy comparison exceeded one UNORM step: "+name);}
#else
            (void)legacyTime;
#endif
            if(save)soundings::writeImage(output/(name+"-dx12.png"),native.width,native.height,next);report["cases"].push_back(item);return next;
        };
        const auto idle=compare("idle",0,-1,true);
        native.event(A75_KEY_DOWN,1);
#ifdef SOUNDINGS_LEGACY_COMPARE
        legacy.echoes.push(1,.38f,.57f);
#endif
        Pixels keyFrame;
        for(double age:{0.,.01,.037,.1,.218,.43,.68,.815,.849}) {
            auto pixels=compare("single-"+std::to_string(age),1+age,0,age==.43);
            if(age==.43)keyFrame=std::move(pixels);
        }
        require(keyFrame!=idle,"A real key must draw the original echo atlas");
        auto extent=[&](const Pixels& pixels) {
            std::array<unsigned,5> box{native.width,native.height,0,0,0};
            for(UINT y=0;y<native.height;++y)for(UINT x=0;x<native.width;++x) {
                const auto at=(std::size_t(y)*native.width+x)*4;
                if(pixels[at]==idle[at]&&pixels[at+1]==idle[at+1]&&pixels[at+2]==idle[at+2])continue;
                box[0]=std::min(box[0],x);box[1]=std::min(box[1],y);box[2]=std::max(box[2],x+1);box[3]=std::max(box[3],y+1);++box[4];
            }
            return box;
        };
        const auto localized=extent(keyFrame);require(localized[4]>10&&localized[2]-localized[0]<=302&&localized[3]-localized[1]<=302,"Echo must stay localized to the key at its declared diameter");
        std::array<unsigned,2> scaleWidths{};unsigned scaleIndex=0;
        for(float reach:{.5f,1.4f}) {
            const nlohmann::json settings={{"reach",reach}};native.event(A75_SETTINGS,1.43,.38f,.57f,settings.dump());
#ifdef SOUNDINGS_LEGACY_COMPARE
            legacy.settings(settings);
#endif
            const auto box=extent(compare("reach-"+std::to_string(reach),1.43,0,true));scaleWidths[scaleIndex++]=box[2]-box[0];
        }
        require(scaleWidths[1]>scaleWidths[0]*2.4&&scaleWidths[1]<scaleWidths[0]*3.2,"Reach must scale real visible atlas geometry");
        report["reachPixelWidths"]=scaleWidths;report["defaultKeyChangedPixels"]=localized[4];
        const auto settings=nlohmann::json{{"reach",1.4},{"backgroundBrightness",1.8},{"backgroundHue",120},{"backgroundSaturation",1.6},{"backgroundColor",{140,230,190}}};
        native.event(A75_SETTINGS,1.43,.38f,.57f,settings.dump());
#ifdef SOUNDINGS_LEGACY_COMPARE
        legacy.settings(settings);
#endif
        compare("graded-image",1.43,0,true);
        const auto solid=nlohmann::json{{"reach",1.4},{"backgroundStyle",1},{"backgroundBrightness",1.7},{"backgroundHue",75},{"backgroundSaturation",.4},{"backgroundColor",{18,96,180}}};
        native.event(A75_SETTINGS,1.43,.38f,.57f,solid.dump());
#ifdef SOUNDINGS_LEGACY_COMPARE
        legacy.settings(solid);
#endif
        compare("graded-solid",1.43,0,true);
        const auto defaults=nlohmann::json{{"reach",1},{"backgroundStyle",0},{"backgroundBrightness",1},{"backgroundHue",0},{"backgroundSaturation",1},{"backgroundColor",{255,255,255}}};
        native.event(A75_SETTINGS,2,.38f,.57f,defaults.dump());
#ifdef SOUNDINGS_LEGACY_COMPARE
        legacy.settings(defaults);
#endif
        require(compare("expired",2,-1)==idle,"Expired scene must equal original backdrop");
        for(unsigned i=0;i<32;++i) {
            const float x=.08f+(i%8)*.12f,y=.2f+(i/8)*.18f;const double time=3+i*.001;
            native.event(A75_KEY_DOWN,time,x,y);
#ifdef SOUNDINGS_LEGACY_COMPARE
            legacy.echoes.push(time,x,y);
#endif
        }
        const auto multi=compare("32-keys-capacity-24",3.3,0,true);
        native.event(A75_KEY_UP,3.3);require(compare("key-up-no-extra-burst",3.3,0)==multi,"Key up must not emit a burst");
        native.event(A75_PAUSE,3.3);require(native.frame(10,-1)==multi,"Pause must freeze the effect clock");
        native.event(A75_KEY_DOWN,10);require(native.frame(10.1,-1)==multi,"Paused input must not emit");
        native.event(A75_RESUME,10.2);compare("resumed-clock",10.21,0,false,3.31);
        native.event(A75_INPUT_RESET,10.22);
#ifdef SOUNDINGS_LEGACY_COMPARE
        legacy.echoes.clear();
#endif
        require(compare("input-reset",10.22,-1,false,3.32)==idle,"Input reset must clear active events");
        require(native.errors()==0,"D3D12 debug layer reported errors");report["debugLayerAvailable"]=bool(native.debug);report["debugErrors"]=0;
        report["pauseResumeClockFrozen"]=true;report["keyUpDoesNotEmit"]=true;report["inputResetClears"]=true;report["status"]="passed";
        std::filesystem::create_directories(output);std::ofstream(output/L"verification.json")<<report.dump(2);
        std::cout<<"PASS: "<<report["cases"].size()<<" native DX12 frame comparisons, settings, 24-hit budget, expiry/sleep, pause clock, reset.\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
