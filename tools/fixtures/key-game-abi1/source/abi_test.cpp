#include "Action75Wallpaper.h"
#include <cstdio>
#include <cstring>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#expr); return 1; } } while(false)
int check_format(const wchar_t *path, DXGI_FORMAT format) {
    HMODULE module=LoadLibraryW(path); CHECK(module);
    auto get=reinterpret_cast<A75GetWallpaper>(GetProcAddress(module,"Action75GetWallpaper"));
    CHECK(get && !get(0));
    auto api=get(ACTION75_WALLPAPER_ABI);
    CHECK(api && api->size>=sizeof(A75Wallpaper));
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)));
    constexpr unsigned width=960,height=360;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=format; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,readback;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)));
    ComPtr<ID3D11RenderTargetView> view;
    CHECK(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&view)));
    desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)));
    auto pixel=[&](int x,int y){
        ctx->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map))) return 0u;
        const auto *bytes=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch+x*4;
        const unsigned red=bytes[format==DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 2];
        const unsigned blue=bytes[format==DXGI_FORMAT_R8G8B8A8_UNORM ? 2 : 0];
        const unsigned result=(unsigned(bytes[3])<<24) | (red<<16) | (unsigned(bytes[1])<<8) | blue;
        ctx->Unmap(readback.Get(),0); return result;
    };
    A75Create cfg={sizeof(cfg),width,height,device.Get(),ctx.Get(),L".","{}"};
    void *game=api->create(&cfg); CHECK(game);
    A75Frame frame={sizeof(frame),width,height,0,view.Get()};
    auto render=[&](){ frame.seconds+=1./60; return api->render(game,&frame); };
    auto emit=[&](unsigned type,const char *code=nullptr,const char *json=nullptr){
        A75Event e={sizeof(e),type,width,height,frame.seconds,0,0,code,json};
        api->on_event(game,&e);
    };
    CHECK(render()==-1 && pixel(93,246)==0xff86efce);
    struct Case {const char* update;const char* reset;bool foreground;};
    const Case appearance[]={
        {R"({"runnerStyle":2})",R"({"runnerStyle":0})",true},
        {R"({"runnerBrightness":0.5})",R"({"runnerBrightness":1})",true},
        {R"({"runnerHue":120})",R"({"runnerHue":0})",true},
        {R"({"runnerSaturation":0})",R"({"runnerSaturation":1})",true},
        {R"({"runnerColor":[255,68,36]})",R"({"runnerColor":[255,255,255]})",true},
        {R"({"stationStyle":2})",R"({"stationStyle":0})",false},
        {R"({"stationBrightness":0.5})",R"({"stationBrightness":1})",false},
        {R"({"stationHue":120})",R"({"stationHue":0})",false},
        {R"({"stationSaturation":0})",R"({"stationSaturation":1})",false},
        {R"({"stationColor":[255,68,36]})",R"({"stationColor":[255,255,255]})",false}
    };
    for(const Case& test:appearance){
        emit(A75_SETTINGS,nullptr,test.update); CHECK(render()==-1);
        CHECK((pixel(93,246)!=0xff86efce)==test.foreground);
        CHECK((pixel(14,10)!=0xff101b2b)!=test.foreground);
        emit(A75_SETTINGS,nullptr,test.reset); CHECK(render()==-1);
        CHECK(pixel(93,246)==0xff86efce && pixel(14,10)==0xff101b2b);
    }
    emit(A75_KEY_DOWN,"ArrowRight");
    for (int i=0;i<32;++i) CHECK(render()==0);
    emit(A75_KEY_UP,"ArrowRight"); CHECK(render()==-1);
    CHECK(pixel(93,246)!=0xff86efce && pixel(101,24)==0xffdcefeb && pixel(92,26)!=0xffdcefeb);
    emit(A75_SETTINGS,nullptr,"{ \"amber_lights\" : true }"); CHECK(render()==-1 && pixel(50,270)==0xffffbb59);
    emit(A75_KEY_DOWN,"Space"); CHECK(render()==0); emit(A75_INPUT_RESET);
    emit(A75_PAUSE); CHECK(render()==-1 && pixel(456,138)==0xffffbb59);
    emit(A75_RESUME); CHECK(render()==0 && pixel(456,138)!=0xffffbb59);
    emit(A75_KEY_DOWN,"Enter"); CHECK(render()==-1);
    emit(A75_KEY_DOWN,"Enter"); CHECK(render()==-1);
    emit(A75_KEY_UP,"Enter"); emit(A75_KEY_DOWN,"Enter"); CHECK(render()==0);
    for (int i=0;i<120;++i) render();
    CHECK(render()==-1);
    emit(A75_KEY_DOWN,"ArrowRight");
    for (int i=0;i<155;++i) render();
    CHECK(render()==-1 && pixel(93,246)==0xffffbb59 && pixel(196,26)!=0xffdcefeb);
    api->destroy(game); FreeLibrary(module);
    return 0;
}

int wmain(int argc,wchar_t **argv) {
    CHECK(argc==2);
    CHECK(check_format(argv[1], DXGI_FORMAT_R8G8B8A8_UNORM)==0);
    CHECK(check_format(argv[1], DXGI_FORMAT_B8G8R8A8_UNORM)==0);
    std::puts("RGBA8 and BGRA8 WARP game readback passed, including amber RGB(255,187,89)");
    return 0;
}
