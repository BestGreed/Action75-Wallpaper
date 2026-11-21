// This executable alone links DX11 for independent reference rendering.
// Fixed seeds are internal test seams, never hidden package settings.
#include "ParticleWallpaper.cpp"
#include <EffekseerRendererDX11.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <iostream>
#include <functional>

namespace {
using author_particles::json;
constexpr unsigned W=1920,H=720;
void check(HRESULT h,const char* what){if(FAILED(h))throw std::runtime_error(what);}
void require(bool b,const char* what){if(!b)throw std::runtime_error(what);}
using Image=std::vector<unsigned char>;
void png(const std::filesystem::path& path,const Image& image){
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    check(factory->CreateStream(&stream),"WIC stream");check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"WIC file");
    check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"WIC encoder");check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"WIC encoder initialize");
    check(encoder->CreateNewFrame(&frame,nullptr),"WIC frame");check(frame->Initialize(nullptr),"WIC frame initialize");check(frame->SetSize(W,H),"WIC size");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;check(frame->SetPixelFormat(&format),"WIC format");
    // The Windows PNG codec may negotiate BGRA even when RGBA was requested.
    // Keep captures faithful to the DX12 RGBA8 readback in either case.
    auto encoded=image;
    if(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA))for(unsigned i=0;i<encoded.size();i+=4)std::swap(encoded[i],encoded[i+2]);
    else require(IsEqualGUID(format,GUID_WICPixelFormat32bppRGBA),"Unsupported WIC PNG channel order");
    check(frame->WritePixels(H,W*4,UINT(encoded.size()),encoded.data()),"WIC pixels");check(frame->Commit(),"WIC frame commit");check(encoder->Commit(),"WIC commit");
}
json difference(const Image& a,const Image& b){
    std::uint64_t total=0,visibleA=0,visibleB=0,support=0,overlap=0;unsigned maximum=0;int left=W,top=H,right=-1,bottom=-1;
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const auto i=(y*W+x)*4;unsigned va=0,vb=0;
        for(unsigned c=0;c<3;++c){auto d=unsigned(std::abs(int(a[i+c])-int(b[i+c])));total+=d;maximum=std::max(maximum,d);va|=a[i+c];vb|=b[i+c];}
        if(va||vb)++support;if(va&&vb)++overlap;
        if(va)++visibleA;if(vb){++visibleB;left=std::min(left,int(x));top=std::min(top,int(y));right=std::max(right,int(x));bottom=std::max(bottom,int(y));}}
    return {{"meanAbsoluteRgbError",double(total)/(W*H*3)},{"supportMeanAbsoluteRgbError",support?double(total)/(support*3):0.},
        {"supportIoU",support?double(overlap)/support:1.},{"supportPixels",support},{"maximumChannelError",maximum},{"visiblePixelsDx11",visibleA},{"visiblePixelsDx12",visibleB},{"dx12Bounds",{left,top,right,bottom}}};
}
struct Surface12{
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12Resource> target,readback;ComPtr<ID3D12Fence> fence;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    HANDLE signal=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;bool first=true;
    explicit Surface12(IDXGIAdapter* adapter){
        check(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"DX12 device");D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"DX12 queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"DX12 allocator");check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"DX12 list");check(list->Close(),"Initial close");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"RTV heap");
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=W;d.Height=H;d.DepthOrArraySize=d.MipLevels=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"DX12 target");device->CreateRenderTargetView(target.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
        UINT64 size=0;device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);hp.Type=D3D12_HEAP_TYPE_READBACK;d={};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"DX12 readback");check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"DX12 fence");
    }
    ~Surface12(){CloseHandle(signal);}
    Image frame(double seconds,const std::function<int(const A75Frame12&)>& render,int* scheduling){
        check(allocator->Reset(),"Reset allocator");check(list->Reset(allocator.Get(),nullptr),"Reset list");D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=target.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;if(!first)list->ResourceBarrier(1,&b);first=false;
        const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);D3D12_VIEWPORT vp{0,0,float(W),float(H),0,1};D3D12_RECT sc{0,0,LONG(W),LONG(H)};list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);
        A75Frame12 frame{sizeof(frame),W,H,seconds,list.Get(),target.Get(),rtv};int result=render(frame);if(scheduling)*scheduling=result;require(result>=-1,"Actual DX12 render failed");
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&b);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close(),"Close frame");ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);check(queue->Signal(fence.Get(),++serial),"Signal");check(fence->SetEventOnCompletion(serial,signal),"Fence event");require(WaitForSingleObject(signal,30000)==WAIT_OBJECT_0,"GPU completion timeout");
        unsigned char* bytes=nullptr;D3D12_RANGE range{0,SIZE_T(layout.Footprint.RowPitch)*H};check(readback->Map(0,&range,reinterpret_cast<void**>(&bytes)),"DX12 map");Image image(W*H*4);for(unsigned y=0;y<H;++y)std::copy_n(bytes+layout.Offset+y*layout.Footprint.RowPitch,W*4,image.data()+y*W*4);D3D12_RANGE noWrite{};readback->Unmap(0,&noWrite);return image;
    }
    Image draw(author_particles::Instance& instance,double seconds,int* scheduling=nullptr){return frame(seconds,[&](const A75Frame12& f){return instance.draw(f);},scheduling);}
    Image draw(const A75Wallpaper12* api,void* instance,double seconds,int* scheduling=nullptr){return frame(seconds,[&](const A75Frame12& f){return api->render(instance,&f);},scheduling);}
};
struct Reference11{
    author_particles::Program program;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> rtv;
    EffekseerRendererDX11::RendererRef renderer;Effekseer::ManagerRef manager;std::vector<Effekseer::EffectRef> effects;
    Reference11(IDXGIAdapter* adapter,const std::filesystem::path& candidate,const std::filesystem::path& baseline):program(candidate){
        D3D_FEATURE_LEVEL feature;check(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&feature,&context),"Reference DX11 device");
        D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;check(device->CreateTexture2D(&d,nullptr,&target),"DX11 target");check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"DX11 RTV");d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&d,nullptr,&readback),"DX11 readback");
        renderer=EffekseerRendererDX11::Renderer::Create(device.Get(),context.Get(),8192);require(bool(renderer),"DX11 renderer");manager=Effekseer::Manager::Create(8192);manager->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);manager->SetRandFunc([](){return 12345;});
        manager->SetSpriteRenderer(renderer->CreateSpriteRenderer());manager->SetRibbonRenderer(renderer->CreateRibbonRenderer());manager->SetRingRenderer(renderer->CreateRingRenderer());manager->SetTrackRenderer(renderer->CreateTrackRenderer());manager->SetModelRenderer(renderer->CreateModelRenderer());manager->SetTextureLoader(renderer->CreateTextureLoader());manager->SetModelLoader(renderer->CreateModelLoader());manager->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>());
        for(const auto& resource:program.resources){auto e=Effekseer::Effect::Create(manager,(baseline/resource.filename()).u16string().c_str());require(bool(e),"Original reference effect");effects.push_back(e);}
        Effekseer::Matrix44 projection,camera;projection.OrthographicRH(float(W),float(H),.1f,2000.f);camera.LookAtRH({0,0,1000},{0,0,0},{0,1,0});renderer->SetProjectionMatrix(projection);renderer->SetCameraMatrix(camera);renderer->SetCameraParameter({0,0,-1},{0,0,1000});Effekseer::Manager::LayerParameter layer;layer.ViewerPosition={0,0,1000};manager->SetLayerParameter(0,layer);
    }
    void begin(unsigned index,const std::string& settings){program.configure(settings.c_str());auto h=manager->Play(effects.at(index),0,0,0);manager->SetRandomSeed(h,12345);float scale=program.baseScale*program.scale;manager->SetScale(h,scale,scale,scale);auto gain=std::uint8_t(std::lround(255*std::clamp(program.brightness,0.f,1.f)));manager->SetAllColor(h,{gain,gain,gain,255});for(unsigned i=0;i<4;++i)if(program.dynamicBound[i])manager->SetDynamicInput(h,i,program.dynamic[i]);}
    Image draw(unsigned frame){
        auto clear=program.background();context->ClearRenderTargetView(rtv.Get(),clear.data());ID3D11RenderTargetView* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};context->RSSetViewports(1,&vp);
        Effekseer::Manager::UpdateParameter u;u.DeltaFrame=frame?program.speed:0;u.UpdateInterval=1;u.SyncUpdate=true;manager->Update(u);renderer->SetTime(float(frame)*program.speed/60);require(renderer->BeginRendering(),"Reference begin");Effekseer::Manager::DrawParameter p;p.ZNear=0;p.ZFar=1;p.CameraPosition={0,0,1000};p.CameraFrontDirection={0,0,-1};p.ViewProjectionMatrix=renderer->GetCameraProjectionMatrix();manager->Draw(p);require(renderer->EndRendering(),"Reference end");
        context->CopyResource(readback.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m),"Reference map");Image image(W*H*4);for(unsigned y=0;y<H;++y)std::copy_n(static_cast<unsigned char*>(m.pData)+y*m.RowPitch,W*4,image.data()+y*W*4);context->Unmap(readback.Get(),0);return image;
    }
};
void initialize(author_particles::Instance& p,Surface12& s,const std::filesystem::path& dir,const std::string& settings){A75Create12 c{sizeof(c),W,H,s.device.Get(),dir.c_str(),settings.c_str(),nullptr,nullptr};require(p.initialize(c),"Actual package initialize");p.manager->SetRandFunc([](){return 12345;});p.program.random=0x9e3779b97f4a7c15ull;}
void event(author_particles::Instance& p,unsigned type,double seconds,const char* settings=nullptr){A75Event e{};e.size=sizeof(e);e.type=type;e.seconds=seconds;e.x=e.y=.5f;e.width=W;e.height=H;e.settings_json=settings;p.event(e);}
json playbackChecks(const std::filesystem::path& pack,Surface12& surface){
    author_particles::Instance p(pack);initialize(p,surface,pack,"{}");
    const auto patch=[&](const json& settings){auto s=settings.dump();event(p,A75_SETTINGS,0,s.c_str());};
    const auto chosen=[&](int expected,unsigned type=A75_KEY_DOWN){auto count=p.handles.size();event(p,type,0);if(expected<0){require(p.handles.size()==count,"Unexpected authored emission");return;}require(p.handles.size()==std::min<std::size_t>(64,count+1),"Missing authored emission");require(p.manager->GetEffect(p.handles.back())==p.effects.at(expected),"Wrong original .efk selected");};
    json mask={{"flower",0},{"shuffle",false},{"spacing",1}};
    for(unsigned i=1;i<p.program.toggles.size();++i)mask[p.program.toggles[i]]=i==2||i==5;
    patch(mask);chosen(1);chosen(4);chosen(1);chosen(4); // Filtered order loops.
    for(unsigned i=1;i<p.program.toggles.size();++i)mask[p.program.toggles[i]]=false;
    patch(mask);chosen(-1);chosen(-1,A75_POINTER_DOWN);patch({{"flower",8}});chosen(7); // Fixed choice overrides all-off.
    patch({{"flower",0}});mask[p.program.toggles[2]]=true;mask[p.program.toggles[5]]=true;patch(mask);
    chosen(1);patch({{"flower",8}});chosen(7);patch({{"flower",0}});chosen(4); // Fixed choice retains combo cursor.
    patch({{"spacing",3}});chosen(-1);patch({{"shuffle",true}});chosen(-1);
    auto pointerCount=p.handles.size();event(p,A75_POINTER_DOWN,0);require(p.handles.size()==pointerCount+1&&p.program.keys==2,"Pointer consumed keyboard cadence");
    require(p.manager->GetEffect(p.handles.back())==p.effects[1]||p.manager->GetEffect(p.handles.back())==p.effects[4],"Random pointer escaped selected set");
    event(p,A75_KEY_DOWN,0);require(p.manager->GetEffect(p.handles.back())==p.effects[1]||p.manager->GetEffect(p.handles.back())==p.effects[4],"Random selected disabled resource");
    patch({{"spacing",1}});unsigned seen=0;
    for(unsigned i=0;i<48;++i){event(p,A75_KEY_DOWN,0);auto effect=p.manager->GetEffect(p.handles.back());require(effect==p.effects[1]||effect==p.effects[4],"Shuffle escaped selected set");seen|=effect==p.effects[1]?1:2;}require(seen==3,"Shuffle did not visit selected resources");
    patch({{"shuffle",false},{"spacing",3}});chosen(-1);chosen(-1);
    // Reset while paused clears live geometry and rewinds order, preserving cadence.
    event(p,A75_PAUSE,0);event(p,A75_INPUT_RESET,0);require(p.handles.empty()&&p.manager->GetTotalInstanceCount()==0&&p.program.cursor==0&&p.program.keys==2,"Reset must stop all, rewind and retain cadence");chosen(-1);event(p,A75_RESUME,0);chosen(1);chosen(-1);chosen(-1);chosen(4);
    patch({{"spacing",1},{"flower",1},{"size",2},{"speed",2},{"brightness",.25}});chosen(0);
    require(p.program.scale==2&&p.program.speed==2&&p.program.brightness==.25f,"Original appearance binding values");
    auto m=p.manager->GetMatrix(p.handles.back());require(std::abs(m.Value[0][0]-2)<.001f,"Size did not reach real Effekseer root transform");
    // Brightness changes existing particles without advancing simulation.
    surface.draw(p,0);event(p,A75_PAUSE,0);auto dim=surface.draw(p,0);patch({{"brightness",1}});auto bright=surface.draw(p,0);
    const auto energy=[](const Image& image){std::uint64_t sum=0;for(unsigned i=0;i<image.size();++i)if(i%4!=3)sum+=image[i];return sum;};
    require(energy(bright)>energy(dim),"Brightness did not affect existing real particles");event(p,A75_RESUME,0);
    patch({{"size",.5},{"speed",.5}});m=p.manager->GetMatrix(p.handles.back());require(std::abs(m.Value[0][0]-.5f)<.001f&&p.program.speed==.5f,"Live size/speed binding update");
    patch({{"spacing",1}});for(unsigned i=0;i<72;++i)event(p,A75_KEY_DOWN,0);require(p.handles.size()==64&&p.manager->GetTotalInstanceCount()<=8192,"Legacy effect capacity");
    return {{"filteredOrderAndLoop",true},{"emptySet",true},{"fixedOverridesMask",true},{"fixedRetainsOrderCursor",true},{"shuffleOnlySelected",true},{"shuffleVisitsSelected",true},
        {"pointerBypassesCadence",true},{"settingsPreserveCadenceExceptInterval",true},{"pausedResetClearsAndRewinds",true},{"resetRetainsCadence",true},
        {"resumeFirstKeyUsesRewoundOrder",true},{"liveScaleTransform",true},{"liveBrightnessPixels",true},{"speedBinding",true},{"capacity64Effects8192Particles",true}};
}
json dynamicChecks(const std::filesystem::path& pack,Surface12& surface){
    author_particles::Instance p(pack);initialize(p,surface,pack,"{}");event(p,A75_KEY_DOWN,0);require(p.manager->GetDynamicInput(p.handles.back(),0)==.5f,"Original bearing default");
    for(int value=-1;value<=1;++value){const auto patch=json({{"bearing",value}}).dump();event(p,A75_SETTINGS,0,patch.c_str());require(p.manager->GetDynamicInput(p.handles.back(),0)==float(value),"Bearing failed to update live original effect");}
    auto count=p.handles.size();event(p,A75_POINTER_DOWN,0);require(p.handles.size()==count,"Non-playback pack must not capture pointer");
    return {{"defaultDynamicInput",.5},{"liveDynamicInputMinusOneZeroOne",true},{"pointerCaptureDisabled",true}};
}
json dllChecks(const std::filesystem::path& package,Surface12& surface,const std::filesystem::path& out){
    const auto absolute=std::filesystem::absolute(package);HMODULE module=LoadLibraryExW((absolute/L"wallpaper.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);require(module!=nullptr,"Final DLL LoadLibrary");
    const auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));require(get&&get(1)==nullptr,"ABI negotiation");const auto* api=get(2);require(api&&api->size==sizeof(A75Wallpaper12)&&api->abi==2,"Final DLL public ABI2 table");
    A75Create12 create{sizeof(create),W,H,surface.device.Get(),absolute.c_str(),"{}",nullptr,nullptr};void* instance=api->create(&create);require(instance!=nullptr,"Final DLL create");
    const auto send=[&](unsigned type,double seconds,const char* settings=nullptr){A75Event e{};e.size=sizeof(e);e.type=type;e.seconds=seconds;e.x=e.y=.5f;e.width=W;e.height=H;e.settings_json=settings;api->on_event(instance,&e);};
    send(A75_KEY_DOWN,0);Image before;for(unsigned f=0;f<=26;++f)before=surface.draw(api,instance,double(f)/60);require(difference(before,before)["supportPixels"].get<unsigned>()>50,"Final DLL produced no original particle pixels");png(out/(package.filename().u8string()+"-loaded-dll.png"),before);
    send(A75_PAUSE,26./60);auto frozen=surface.draw(api,instance,10);require(difference(before,frozen)["maximumChannelError"]==0,"Final DLL pause changed pixels");send(A75_RESUME,10);auto resumed=surface.draw(api,instance,10);require(difference(before,resumed)["maximumChannelError"]==0,"Final DLL resume jumped time");
    send(A75_PAUSE,10);send(A75_INPUT_RESET,10);int scheduling=0;auto cleared=surface.draw(api,instance,10,&scheduling);require(difference(cleared,cleared)["supportPixels"]==0&&scheduling==-1,"Final DLL reset did not clear and idle");send(A75_RESUME,10);send(A75_KEY_DOWN,10);auto restarted=surface.draw(api,instance,10);require(difference(restarted,restarted)["supportPixels"].get<unsigned>()>0,"Final DLL reset/resume lost next key");
    send(A75_INPUT_RESET,10);send(A75_SETTINGS,10,"{\"backgroundStyle\":1,\"backgroundColor\":[100,160,220],\"backgroundHue\":80,\"backgroundSaturation\":1.4,\"backgroundBrightness\":1.7}");auto background=surface.draw(api,instance,10,&scheduling);require(scheduling==-1&&difference(background,background)["supportPixels"]==W*H,"Final DLL background binding");
    api->destroy(instance);FreeLibrary(module);return {{"loadLibrary",true},{"publicAbi2",true},{"realParticlePixels",true},{"pauseResumeExact",true},{"pausedResetClears",true},{"nextKeyAfterReset",true},{"settingsBackground",true}};
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=4,"Usage: verify_particles candidate-root frozen-baseline-root evidence-output [warp]");CoInitializeEx(nullptr,COINIT_MULTITHREADED);const std::filesystem::path candidate=argv[1],baseline=argv[2],out=argv[3];std::filesystem::create_directories(out);
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> chosen;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
    if(argc>4)check(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)),"WARP adapter");else{SIZE_T best=0;for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&(!chosen||d.DedicatedVideoMemory>best)){chosen=a;best=d.DedicatedVideoMemory;}}}
    require(bool(chosen),"Graphics adapter");DXGI_ADAPTER_DESC1 adapter{};chosen->GetDesc1(&adapter);Surface12 surface(chosen.Get());json report={{"adapter",std::filesystem::path(adapter.Description).u8string()},{"width",W},{"height",H},{"seed",12345},{"reference","real Effekseer 1.80.8 DX11 renderer and frozen original resources"},{"candidate","same CPU runtime and original .efk; actual package Instance on borrowed DX12 command list"}};
    auto run=[&](const std::string& id,const std::string& name,unsigned slot,json settings,unsigned end=190){auto pack=candidate/id;auto patch=settings.dump();author_particles::Instance runtime(pack);initialize(runtime,surface,pack,patch);Reference11 reference(chosen.Get(),pack,baseline/id);reference.begin(slot,patch);runtime.trigger(.5f,.5f,0,true);require(runtime.handles.size()==1,"Runtime original effect selection");runtime.manager->SetRandomSeed(runtime.handles.front(),12345);json item={{"id",id},{"case",name},{"settings",settings}};const std::vector<unsigned> samples{0,6,26,50,90,150,190};
        for(unsigned f=0;f<=end;++f){auto a=reference.draw(f),b=surface.draw(runtime,double(f)/60);require(runtime.manager->GetTotalInstanceCount()==reference.manager->GetTotalInstanceCount(),"CPU instance count differs");if(std::find(samples.begin(),samples.end(),f)!=samples.end()){auto metric=difference(a,b);metric["frame"]=f;metric["cpuInstances"]=runtime.manager->GetTotalInstanceCount();auto stem=name+"-"+std::to_string(f);png(out/(stem+"-dx11.png"),a);png(out/(stem+"-dx12.png"),b);item["samples"].push_back(metric);require(metric["supportMeanAbsoluteRgbError"].get<double>()<5&&metric["supportIoU"].get<double>()>.90,"DX12 nonblack support pixel difference exceeds tolerance");}}
        report["cases"].push_back(item);std::ofstream(out/L"verification.json")<<report.dump(2);std::cout<<name<<" passed\n";
    };
    for(unsigned i=0;i<8;++i)run("lantern-almanac","flower-"+std::to_string(i+1),i,{{"flower",i+1}});
    run("lantern-almanac","small",0,{{"flower",1},{"size",.5}},50);run("lantern-almanac","large",0,{{"flower",1},{"size",2}},50);run("lantern-almanac","dim",0,{{"flower",1},{"brightness",.25}},50);run("lantern-almanac","slow",0,{{"flower",1},{"speed",.5}},90);run("lantern-almanac","fast",0,{{"flower",1},{"speed",2}},90);
    for(int b=-1;b<=1;++b)run("tidemark-bearing","bearing-"+std::to_string(b),0,{{"bearing",b}},150);
    auto pack=candidate/L"lantern-almanac";author_particles::Instance runtime(pack);initialize(runtime,surface,pack,"{\"flower\":1}");runtime.trigger(.5f,.5f,0,true);Image before;for(unsigned f=0;f<=26;++f)before=surface.draw(runtime,double(f)/60);event(runtime,A75_PAUSE,26./60);auto frozen=surface.draw(runtime,10);require(difference(before,frozen)["maximumChannelError"]==0,"Pause changed pixels");event(runtime,A75_RESUME,10);auto resumed=surface.draw(runtime,10);require(difference(before,resumed)["maximumChannelError"]==0,"Resume included paused time");int sleep=0;for(unsigned f=1;f<=600;++f)surface.draw(runtime,10+double(f)/60,&sleep);require(runtime.handles.empty()&&sleep==-1,"Expiry failed to idle");
    event(runtime,A75_SETTINGS,20,"{\"backgroundStyle\":1,\"backgroundColor\":[100,160,220],\"backgroundHue\":80,\"backgroundSaturation\":1.4,\"backgroundBrightness\":1.7}");auto bg=surface.draw(runtime,20,&sleep);png(out/L"solid-background-dx12.png",bg);require(sleep==-1,"Static background failed to idle");report["lifecycle"]={{"pausePixelsExact",true},{"resumeExcludesPausedTime",true},{"expirySleeps",true},{"staticBackgroundSleeps",true}};
    report["playback"]=playbackChecks(pack,surface);report["dynamicInput"]=dynamicChecks(candidate/L"tidemark-bearing",surface);
    for(const auto id:{"lantern-almanac","tidemark-bearing"})report["loadedDll"][id]=dllChecks(candidate/id,surface,out);
    double supportMaximum=0,canvasMaximum=0,minIoU=1;unsigned channelMaximum=0;
    for(const auto& c:report["cases"])for(const auto& s:c["samples"]){supportMaximum=std::max(supportMaximum,s["supportMeanAbsoluteRgbError"].get<double>());canvasMaximum=std::max(canvasMaximum,s["meanAbsoluteRgbError"].get<double>());minIoU=std::min(minIoU,s["supportIoU"].get<double>());channelMaximum=std::max(channelMaximum,s["maximumChannelError"].get<unsigned>());}
    report["pixelSummary"]={{"maximumNonblackSupportMAE",supportMaximum},{"maximumCanvasMAE",canvasMaximum},{"minimumSupportIoU",minIoU},{"maximumChannelError",channelMaximum},{"thresholdSupportMAE",5},{"thresholdSupportIoU",.90}};
    report["resetAbiLimitation"]="A75_INPUT_RESET has no reason payload. Package clears all particles and rewinds order, retaining cadence, matching legacy stopEffects. Legacy pointer-capture cancellation did not clear Effekseer; its identical native reset event cannot be distinguished through the public ABI.";
    report["passed"]=true;std::ofstream(out/L"verification.json")<<report.dump(2);std::cout<<"All real CPU/DX11/DX12, actual DLL, playback, binding and lifecycle checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
