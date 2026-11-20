#define NOMINMAX
#include "Action75Wallpaper.h"
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <nlohmann/json.hpp>
using Microsoft::WRL::ComPtr;
void checked(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 capture failed");}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc!=4 && argc!=5)return 1;
        std::filesystem::path out=argv[3];
        constexpr int W=1280,H=480;
        std::filesystem::path pack=std::filesystem::absolute(argv[1]);
        std::ifstream calibration{std::filesystem::path(argv[2])};
        auto points=nlohmann::json::parse(calibration).at("points");
        auto lib=LoadLibraryW((pack/L"wallpaper.dll").c_str());
        if(!lib)throw std::runtime_error("Missing DLL");
        auto get=(A75GetWallpaper)GetProcAddress(lib,"Action75GetWallpaper");
        if(!get)throw std::runtime_error("Missing entry");
        auto api=get(1);
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
        checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
        D3D11_TEXTURE2D_DESC desc={W,H,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET};
        ComPtr<ID3D11Texture2D> color,staging;ComPtr<ID3D11RenderTargetView> target;
        checked(device->CreateTexture2D(&desc,nullptr,&color));
        checked(device->CreateRenderTargetView(color.Get(),nullptr,&target));
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&desc,nullptr,&staging));
        A75Create create={sizeof(A75Create),W,H,device.Get(),ctx.Get(),pack.c_str(),"{}"};
        auto scene=api->create(&create);
        if(!scene)throw std::runtime_error("Plugin create failed");
        if(argc==5){
            std::ifstream settings{std::filesystem::path(argv[4])};
            auto json=nlohmann::json::parse(settings).dump();
            A75Event change{sizeof(A75Event),A75_SETTINGS,W,H,0,0,0,nullptr,json.c_str()};
            api->on_event(scene,&change);
        }
        struct Beat{int frame;const char* key;uint32_t type;};
        const Beat beats[]={{28,"KeyA",1},{105,"KeyL",1},{124,"KeyE",1},{201,"KeyE",2},
                            {254,"KeyL",2},{300,"KeyA",2}};
        

        bool awake=true;
        for(int i=0;i<(argc==5?1:500);i++){
            for(int j=0;j<6;j++){
                auto beat=beats[j];if(i!=beat.frame)continue;
                auto p=points.at(beat.key);
                A75Event e={sizeof(A75Event),beat.type,W,H,i/60.,p["x"],p["y"],beat.key};
                api->on_event(scene,&e);awake=true;
            }
            if(awake){
                A75Frame f={sizeof(A75Frame),W,H,i/60.,target.Get()};
                int next=api->render(scene,&f);
                if(next < -1)throw std::runtime_error("Render failed");
                awake=next>=0;

            }
            if(i%15!=0 && i!=20 && i!=100 && i!=155 && i!=230 && i!=499)continue;
            ctx->CopyResource(staging.Get(),color.Get());
            std::ofstream raw(out/(std::to_string(i)+".rgba"),std::ios::binary);
            D3D11_MAPPED_SUBRESOURCE data{};
            checked(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&data));
            for(unsigned y=0;y<H;y++)
                raw.write((char*)data.pData+y*data.RowPitch,W*4);
            ctx->Unmap(staging.Get(),0);
        }
        api->destroy(scene);
    }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}
    return 0;
}
