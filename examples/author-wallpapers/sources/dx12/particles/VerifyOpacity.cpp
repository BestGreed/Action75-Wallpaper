// Reuses the genuine DX12 readback harness. No substitute particle renderer.
#define wmain legacy_particle_comparison_main
#include "Verify.cpp"
#undef wmain

namespace {
struct Loaded {
    HMODULE module=nullptr;const A75Wallpaper12* api=nullptr;void* instance=nullptr;
    Loaded(const std::filesystem::path& pack,Surface12& surface,const char* settings="{}") {
        const auto absolute=std::filesystem::absolute(pack);
        module=LoadLibraryExW((absolute/L"wallpaper.dll").c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);require(bool(module),"Load actual opacity DLL");
        auto get=reinterpret_cast<A75GetWallpaper12>(GetProcAddress(module,"Action75GetWallpaper12"));require(get && !get(1),"Public ABI negotiation");api=get(2);require(api&&api->abi==2,"Public ABI2");
        A75Create12 c{sizeof(c),W,H,surface.device.Get(),absolute.c_str(),settings,nullptr,nullptr};instance=api->create(&c);require(bool(instance),"Actual DLL create");
    }
    ~Loaded(){if(instance)api->destroy(instance);if(module)FreeLibrary(module);}
    void send(unsigned type,double seconds=0,const char* settings=nullptr) {A75Event e{};e.size=sizeof(e);e.type=type;e.seconds=seconds;e.x=e.y=.5f;e.width=W;e.height=H;e.settings_json=settings;api->on_event(instance,&e);}
    void settings(const json& j,double seconds=0){auto s=j.dump();send(A75_SETTINGS,seconds,s.c_str());}
    Image draw(Surface12& s,double seconds,int* scheduling=nullptr){return s.draw(api,instance,seconds,scheduling);}
};
json rgbaDifference(const Image& a,const Image& b) {
    std::uint64_t total=0;unsigned maximum=0,changed=0;
    for(unsigned i=0;i<a.size();++i){unsigned d=unsigned(std::abs(int(a[i])-int(b[i])));total+=d;maximum=std::max(maximum,d);changed+=d!=0;}
    return {{"maximumRGBAError",maximum},{"meanAbsoluteRGBAError",double(total)/a.size()},{"changedChannels",changed}};
}
json premul(const Image& a) {
    unsigned violations=0,transparent=0,nonzero=0;unsigned alphaMax=0;
    for(unsigned i=0;i<a.size();i+=4){alphaMax=std::max(alphaMax,unsigned(a[i+3]));if(!a[i+3])++transparent;if(a[i]||a[i+1]||a[i+2]||a[i+3])++nonzero;
        for(unsigned c=0;c<3;++c)violations+=a[i+c]>a[i+3];}
    require(!violations,"RGBA is not bounded premultiplied");return {{"rgbAboveAlphaChannels",violations},{"zeroAlphaPixels",transparent},{"nonzeroPixels",nonzero},{"maximumAlpha",alphaMax}};
}
Image over(const Image& a,const std::array<unsigned,3>& desktop) {
    auto result=a;for(unsigned i=0;i<a.size();i+=4){for(unsigned c=0;c<3;++c)result[i+c]=std::uint8_t(std::clamp(std::lround(a[i+c]+desktop[c]*(1-a[i+3]/255.)),0l,255l));result[i+3]=255;}return result;
}
json baselineCase(const std::filesystem::path& root,const std::filesystem::path& baseline,const std::string& id,const json& settings,Surface12& surface,const std::filesystem::path& out,const std::string& name) {
    const auto text=settings.dump();Loaded old(baseline/id,surface,text.c_str()),candidate(root/id,surface,text.c_str());old.send(A75_KEY_DOWN);candidate.send(A75_KEY_DOWN);
    json report={{"settings",settings}};unsigned maximum=0;
    for(unsigned f=0;f<=50;++f){auto a=old.draw(surface,f/60.),b=candidate.draw(surface,f/60.);if(f==0||f==6||f==26||f==50){auto d=rgbaDifference(a,b);maximum=std::max(maximum,d["maximumRGBAError"].get<unsigned>());d["frame"]=f;report["samples"].push_back(d);if(f==26){png(out/(name+"-baseline.png"),a);png(out/(name+"-candidate.png"),b);}}}
    require(maximum==0,"100/100 actual baseline DLL comparison changed pixels");report["maximumRGBAError"]=maximum;return report;
}
json opacityCase(const std::filesystem::path& pack,Surface12& surface,const std::filesystem::path& out) {
    Loaded dll(pack,surface);dll.settings({{"backgroundOpacity",0}});int scheduling=0;auto empty=dll.draw(surface,0,&scheduling);auto blank=premul(empty);require(blank["nonzeroPixels"]==0&&scheduling==-1,"Background zero idle must be transparent 0000");
    dll.settings({{"backgroundStyle",1},{"backgroundColor",{96,144,208}},{"backgroundBrightness",1},{"backgroundHue",0},{"backgroundSaturation",1},{"flower",1}});
    dll.send(A75_KEY_DOWN);Image alive;for(unsigned f=0;f<=26;++f)alive=dll.draw(surface,f/60.);dll.send(A75_PAUSE,26./60.);require(premul(alive)["nonzeroPixels"].get<unsigned>()>50,"Background zero hid key particles");
    json report={{"backgroundZeroEmptyRGBA0000",true},{"backgroundZeroKeyStillEmits",true}};Image full;
    for(unsigned background:{0u,50u,100u})for(unsigned foreground:{0u,50u,100u}) {
        dll.settings({{"backgroundOpacity",background},{"foregroundOpacity",foreground}},26./60.);auto image=dll.draw(surface,10,&scheduling);require(scheduling==-1,"Paused opacity draw busy-looped");
        auto p=premul(image);p["backgroundOpacity"]=background;p["foregroundOpacity"]=foreground;
        if(!background&&!foreground)require(p["nonzeroPixels"]==0,"Zero foreground/background is not all zero");
        if(!foreground){const auto alpha=unsigned(std::lround(255*background/100.));unsigned maximum=0;
            for(unsigned i=0;i<image.size();i+=4){maximum=std::max(maximum,unsigned(std::abs(int(image[i+3])-int(alpha))));for(unsigned c=0;c<3;++c){const unsigned rgb[]={96,144,208};maximum=std::max(maximum,unsigned(std::abs(int(image[i+c])-int(std::lround(rgb[c]*background/100.)))));}}
            require(maximum<=1,"Foreground zero altered background");p["backgroundOnlyMaximumRGBAError"]=maximum;}
        if(!background&&foreground==100)full=image;
        const auto name=pack.filename().u8string()+"-bg"+std::to_string(background)+"-fg"+std::to_string(foreground);png(out/(name+"-premul.png"),image);
        if(background==0&&foreground){png(out/(name+"-over-dark.png"),over(image,{18,22,30}));png(out/(name+"-over-light.png"),over(image,{245,245,238}));}
        report["combinations"].push_back(p);
    }
    // Same frozen true GPU draw, scaled as one object rather than per node.
    dll.settings({{"backgroundOpacity",0},{"foregroundOpacity",50}});auto half=dll.draw(surface,10);unsigned scalarError=0;
    for(unsigned i=0;i<half.size();++i)scalarError=std::max(scalarError,unsigned(std::abs(int(half[i])-int(std::lround(full[i]*.5)))));
    require(scalarError<=1,"Whole foreground opacity is not a scalar premul fade");report["foregroundHalfMaximumRGBAError"]=scalarError;
    dll.settings({{"backgroundStyle",1},{"backgroundColor",{255,255,255}},{"backgroundOpacity",100},{"foregroundOpacity",100}});dll.draw(surface,10);
    dll.settings({{"backgroundOpacity",0}});auto fromWhite=dll.draw(surface,10);require(rgbaDifference(fromWhite,full)["maximumRGBAError"]==0,"White background contaminated transparent particles");report["whiteBackgroundToZeroPreservesForegroundExact"]=true;
    dll.send(A75_RESUME,10);dll.settings({{"foregroundOpacity",0},{"backgroundOpacity",0}},10);dll.draw(surface,10,&scheduling);require(scheduling==100,"Invisible active particles must use a delayed wake");report["hiddenActiveDelayMs"]=scheduling;
    dll.settings({{"foregroundOpacity",100}},10);auto restored=dll.draw(surface,10);require(rgbaDifference(restored,full)["maximumRGBAError"]==0,"Restoring opacity at same time changed particle state");report["restoreAtSameTimeExact"]=true;
    dll.settings({{"foregroundOpacity",0}},10);for(unsigned f=1;f<=600;++f)dll.draw(surface,10+f*.1,&scheduling);require(scheduling==-1,"Invisible effects failed to expire and idle");report["invisibleExpirySleeps"]=true;
    dll.settings({{"foregroundOpacity",100}},70);auto expired=dll.draw(surface,70,&scheduling);require(premul(expired)["nonzeroPixels"]==0&&scheduling==-1,"Restoring opacity revived expired geometry");report["restoreAfterLifetimeHasNoOldGeometry"]=true;
    dll.send(A75_INPUT_RESET,70);dll.settings({{"backgroundOpacity",0},{"foregroundOpacity",100}},70);dll.send(A75_KEY_DOWN,70);auto restarted=dll.draw(surface,70);require(premul(restarted)["nonzeroPixels"].get<unsigned>()>0,"Reset/opacity lost next key");report["nextKeyAfterReset"]=true;
    return report;
}
json hiddenAge(const std::filesystem::path& pack,Surface12& surface) {
    author_particles::Instance visible(pack),hidden(pack);initialize(visible,surface,pack,"{\"backgroundOpacity\":0,\"flower\":1}");initialize(hidden,surface,pack,"{\"backgroundOpacity\":0,\"foregroundOpacity\":0,\"flower\":1}");
    visible.trigger(.5f,.5f,0,true);hidden.trigger(.5f,.5f,0,true);visible.manager->SetRandomSeed(visible.handles.front(),12345);hidden.manager->SetRandomSeed(hidden.handles.front(),12345);
    for(unsigned f=0;f<=4;++f){surface.draw(visible,f*.1);surface.draw(hidden,f*.1);require(visible.manager->GetTotalInstanceCount()==hidden.manager->GetTotalInstanceCount(),"Hidden simulation changed CPU particle count");}
    event(hidden,A75_SETTINGS,.4,"{\"foregroundOpacity\":100}");auto a=surface.draw(visible,.4),b=surface.draw(hidden,.4);auto metric=rgbaDifference(a,b);require(metric["maximumRGBAError"]==0,"Short hidden interval changed simulation age");
    auto* target=hidden.opacityLayer.target.Get();event(hidden,A75_SETTINGS,.4,"{\"foregroundOpacity\":50}");surface.draw(hidden,.4);require(hidden.opacityLayer.target.Get()==target,"Opacity settings recreated persistent target");
    metric["fixedInternalSeed"]=12345;metric["shortHiddenSeconds"]=.4;metric["cpuParticleCount"]=visible.manager->GetTotalInstanceCount();metric["persistentTargetReused"]=true;return metric;
}
}
int wmain(int argc,wchar_t** argv){try {
    require(argc>=4,"verify_particle_opacity candidate-root baseline-root output [warp]");CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::filesystem::path candidate=argv[1],baseline=argv[2],out=argv[3];std::filesystem::create_directories(out);
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> chosen;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
    if(argc>4)check(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)),"WARP");else{SIZE_T best=0;for(unsigned i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&(!chosen||d.DedicatedVideoMemory>best)){chosen=a;best=d.DedicatedVideoMemory;}}}
    require(bool(chosen),"Adapter");Surface12 surface(chosen.Get());DXGI_ADAPTER_DESC1 description{};chosen->GetDesc1(&description);json report={{"adapter",std::filesystem::path(description.Description).u8string()},{"width",W},{"height",H},{"reference","actual frozen baseline DX12 DLL and actual final candidate DLL"},{"premulModel","complete stock Effekseer draw over transparent black; coverage=max(stock alpha,R,G,B); scale whole layer; premultiplied over independently premultiplied background"},{"baselineSeed","Each comparison loads both DLLs afresh with their independent static CRT default rand states; fixed flower mode. No public/private debug control and no altered production random behavior."}};
    for(unsigned flower=1;flower<=8;++flower){auto name="flower-"+std::to_string(flower);report["baseline"][name]=baselineCase(candidate,baseline,"lantern-almanac",{{"flower",flower}},surface,out,name);}
    for(int bearing=-1;bearing<=1;++bearing){auto name="bearing-"+std::to_string(bearing);report["baseline"][name]=baselineCase(candidate,baseline,"tidemark-bearing",{{"bearing",bearing}},surface,out,name);}
    report["baseline"]["old-profile-custom"]=baselineCase(candidate,baseline,"lantern-almanac",{{"flower",3},{"size",1.4},{"speed",.5},{"brightness",.6},{"backgroundStyle",1},{"backgroundColor",{70,130,180}},{"backgroundHue",53},{"backgroundSaturation",.7},{"backgroundBrightness",1.2}},surface,out,"old-profile-custom");
    for(const auto id:{"lantern-almanac","tidemark-bearing"}){report["opacity"][id]=opacityCase(candidate/id,surface,out);report["hiddenAge"][id]=hiddenAge(candidate/id,surface);}
    report["playback"]=playbackChecks(candidate/L"lantern-almanac",surface);report["dynamicInput"]=dynamicChecks(candidate/L"tidemark-bearing",surface);
    report["passed"]=true;std::ofstream(out/L"opacity-verification.json")<<report.dump(2);std::cout<<"Actual DLL baseline, opacity, premultiplied edges, playback and lifecycle passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
