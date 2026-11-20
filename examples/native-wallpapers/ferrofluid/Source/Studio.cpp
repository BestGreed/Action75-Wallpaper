#define ACTION75_WALLPAPER_EXPORTS
#define NOMINMAX
#include "Action75Wallpaper.h"
#include "Bath.hpp"
#include "Appearance.hpp"
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <nlohmann/json.hpp>
using Microsoft::WRL::ComPtr;
using json=nlohmann::json;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11");}
struct Studio {
    Bath bath;
    std::vector<std::pair<std::string,Vec>> magnets;
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11VertexShader> floorVS,fluidVS;
    ComPtr<ID3D11PixelShader> floorPS,fluidPS;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11DepthStencilState> floorDepth;
    UINT depthWidth=0,depthHeight=0;
    void ensureDepth(UINT width,UINT height){
        if(width==depthWidth && height==depthHeight)return;
        ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC desc={width,height,1,1,DXGI_FORMAT_D16_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL};
        ComPtr<ID3D11Texture2D> image;
        check(dev->CreateTexture2D(&desc,nullptr,&image));
        check(dev->CreateDepthStencilView(image.Get(),nullptr,depth.ReleaseAndGetAddressOf()));
        depthWidth=width;depthHeight=height;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Buffer> indices,tiles,material;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    float strength=1,shine=1,tone=0,fluidStyle=0;
    SurfaceLook fluidLook,dishLook;
    ComPtr<ID3D11Buffer> appearance;
    void updateLook(){
        struct Constants {float floor[2][4];float fluid[3][4];};
        static_assert(sizeof(Constants)==80);
        Constants constants{};
        static constexpr SurfaceLook::RGB palette[][2]={
            {{.37f,.50f,.52f},{.74f,.82f,.79f}},
            {{.57f,.46f,.40f},{.90f,.83f,.73f}},
            {{.12f,.15f,.19f},{.34f,.40f,.44f}}
        };
        int style=tone>1.5f?2:tone>.5f?1:0;
        for(int i=0;i<2;++i){
            auto rgb=dishLook.transform(palette[style][i]);
            std::copy(rgb.begin(),rgb.end(),constants.floor[i]);
        }
        static constexpr SurfaceLook::RGB metals[]={{1,1,1},{1.65f,.84f,.48f},{.55f,.90f,1.85f}};
        for(int i=0;i<3;++i){
            SurfaceLook::RGB axis{};
            axis[i]=metals[(int)fluidStyle][i];
            auto rgb=fluidLook.transform(axis);
            for(int j=0;j<3;++j)constants.fluid[j][i]=rgb[j];
        }
        constants.fluid[0][3]=fluidStyle!=0 || !fluidLook.neutral();
        ctx->UpdateSubresource(appearance.Get(),0,nullptr,&constants,0,0);
    }
    double last=-1;
    bool paused=false;
    void settings(const char* s){
        if(!s)return;
        auto j=json::parse(s,nullptr,false);
        if(!j.is_object())return;
        auto read=[&](const char* id,float& value,float low,float high){
            auto it=j.find(id);
            if(it!=j.end() && it->is_number())value=std::clamp(it->get<float>(),low,high);
        };
        read("radius",bath.radius,0.085f,0.23f);
        read("viscosity",bath.viscosity,.35f,1.8f);
        read("tension",bath.tension,.65f,1.65f);
        read("edge",bath.edge,0,1.7f);
        read("holdMotion",bath.holdMotion,0,1.5f);
        read("holdHz",bath.holdHz,.15f,1.6f);
        read("field",strength,0.65f,1.45f);
        read("priority",bath.priority,2.0f,10.0f);
        read("follow",bath.follow,4.0f,16.0f);
        read("returnRate",bath.returnRate,3.0f,12.0f);
        read("stretch",bath.stretch,0.0f,3.2f);
        read("spikes",bath.spikes,0.0f,1.6f);
        read("spacing",bath.spacing,0.034f,0.078f);
        read("shine",shine,0.55f,1.35f);
        read("tone",tone,0,2);
        read("fluidStyle",fluidStyle,0,2);
        fluidLook.read(j,"fluid");dishLook.read(j,"dish");
        if(appearance)updateLook();
    }
    explicit Studio(const A75Create& c):ctx(c.context){
        settings(c.settings_json);
        auto dev=c.device;
        auto blob=[&](const wchar_t* name){
            std::ifstream f(std::filesystem::path(c.pack_dir)/name,std::ios::binary);
            if(!f)throw std::runtime_error("shader");
            return std::vector<char>(std::istreambuf_iterator<char>(f),{});
        };
        auto a=blob(L"floorVS.cso"),b=blob(L"fluidVS.cso"),d=blob(L"floorPS.cso"),e=blob(L"fluidPS.cso");
        check(dev->CreateVertexShader(a.data(),a.size(),nullptr,&floorVS));
        check(dev->CreateVertexShader(b.data(),b.size(),nullptr,&fluidVS));
        check(dev->CreatePixelShader(d.data(),d.size(),nullptr,&floorPS));
        check(dev->CreatePixelShader(e.data(),e.size(),nullptr,&fluidPS));
        D3D11_INPUT_ELEMENT_DESC input={"ORIGIN",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1};
        check(dev->CreateInputLayout(&input,1,b.data(),b.size(),&layout));
        D3D11_TEXTURE2D_DESC td={Bath::W,Bath::H,1,1,DXGI_FORMAT_R16G16_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE};
        check(dev->CreateTexture2D(&td,nullptr,&tex));check(dev->CreateShaderResourceView(tex.Get(),nullptr,&srv));
        D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        check(dev->CreateSamplerState(&sd,&sampler));
        D3D11_DEPTH_STENCIL_DESC dd={};
        dd.DepthEnable=FALSE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        check(dev->CreateDepthStencilState(&dd,&floorDepth));
        D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        check(dev->CreateRasterizerState(&rd,&raster));
        std::vector<uint16_t> ids;
        for(int j=0;j<12;j++)for(int i=0;i<12;i++){
            uint16_t n=(uint16_t)(j*13+i);
            for(int offset:{0,1,13,13,1,14})ids.push_back(n+offset);
        }
        D3D11_BUFFER_DESC bd={};bd.ByteWidth=(UINT)ids.size()*2;bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init={ids.data()};check(dev->CreateBuffer(&bd,&init,&indices));
        constexpr UINT columns=(Bath::W-1)/12,rows=(Bath::H-1)/12;
        bd.ByteWidth=(columns*rows+2*(columns+rows))*2*sizeof(uint32_t);bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        check(dev->CreateBuffer(&bd,nullptr,&tiles));
        bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=0;
        check(dev->CreateBuffer(&bd,nullptr,&material));
        bd.ByteWidth=80;
        check(dev->CreateBuffer(&bd,nullptr,&appearance));
        updateLook();
    }
    void event(const A75Event& e){
        if(e.type==A75_SETTINGS)settings(e.settings_json);
        if(e.type==A75_PAUSE||e.type==A75_INPUT_RESET){magnets.clear();paused=e.type==A75_PAUSE;}
        if(e.type==A75_RESUME){paused=false;last=-1;}
        if((e.type==A75_KEY_DOWN||e.type==A75_KEY_UP)&&e.code){
            auto it=std::find_if(magnets.begin(),magnets.end(),[&](auto& k){return k.first==e.code;});
            if(e.type==A75_KEY_UP){if(it!=magnets.end())magnets.erase(it);}
            else if(it==magnets.end()&&magnets.size()<128){
                Vec pos{e.x*Bath::aspect,e.y+.027f};
                magnets.push_back({e.code,pos});
                // A very short tap still imparts momentum between two frame callbacks.
                bath.tap(pos,strength);
            }
        }
    }
    int draw(const A75Frame& f){
        if(paused)return -1;
        std::array<Vec,128> keys{};
        for(size_t i=0;i<magnets.size();i++)keys[i]=magnets[i].second;
        float dt=last<0?1.f/60:(float)std::clamp(f.seconds-last,.001,.06);last=f.seconds;
        bool moving=bath.step(keys.data(),(int)magnets.size(),strength,dt);
        bath.raster();
        auto resource=srv.Get(); ID3D11ShaderResourceView* none=nullptr;
        ctx->VSSetShaderResources(0,1,&none);ctx->PSSetShaderResources(0,1,&none);
        const UINT x=std::min(bath.previousX0,bath.x0),y=std::min(bath.previousY0,bath.y0);
        D3D11_BOX dirty{x,y,0,UINT(std::max(bath.previousX1,bath.x1)+1),
                        UINT(std::max(bath.previousY1,bath.y1)+1),1};
        ctx->UpdateSubresource(tex.Get(),0,&dirty,bath.surface.data()+2*(y*Bath::W+x),Bath::W*4,0);
        float constants[16]={shine,tone,0,0,bath.x0/960.f-.02f,bath.y0/360.f-.04f,bath.x1/960.f+.02f,bath.y1/360.f+.04f};
        for(int i=0;i<std::min(bath.active,2);i++){
            const auto& pole=bath.poles[bath.active-1-i];
            constants[8+4*i]=pole.pos.x;constants[9+4*i]=pole.pos.y;
            constants[10+4*i]=pole.stress;constants[11+4*i]=bath.radius;
        }
        ctx->UpdateSubresource(material.Get(),0,nullptr,constants,0,0);
        auto cb=material.Get();ID3D11Buffer* pixelConstants[]={cb,appearance.Get()};ctx->PSSetConstantBuffers(0,2,pixelConstants);ctx->VSSetConstantBuffers(0,1,&cb);
        auto sm=sampler.Get();ctx->PSSetSamplers(0,1,&sm);
        ctx->VSSetShaderResources(0,1,&resource);ctx->PSSetShaderResources(0,1,&resource);
        ensureDepth(f.width,f.height);
        ctx->OMSetDepthStencilState(floorDepth.Get(),0);
        ctx->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH,1,0);
        ctx->OMSetRenderTargets(1,&f.target,depth.Get());ctx->OMSetBlendState(nullptr,nullptr,~0u);
        ctx->RSSetState(raster.Get());D3D11_VIEWPORT vp={0,0,(float)f.width,(float)f.height,0,1};ctx->RSSetViewports(1,&vp);
        ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(floorVS.Get(),nullptr,0);ctx->PSSetShader(floorPS.Get(),nullptr,0);ctx->Draw(3,0);
        D3D11_MAPPED_SUBRESOURCE map{};check(ctx->Map(tiles.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));
        auto origins=(uint32_t*)map.pData;UINT count=0;
        for(int y=bath.y0/12*12;y<=bath.y1 && y<Bath::H-1;y+=12)
            for(int x=bath.x0/12*12;x<=bath.x1 && x<Bath::W-1;x+=12){
                if(!bath.wetTiles[(y/12)*Bath::tilesAcross+x/12])continue;
                *origins++=x; *origins++=y; ++count;
            }
        // Boundary instances reuse the 12-segment mesh strip. The shader maps its
        // second coordinate to height, sharing the same upper vertices as the surface.
        auto closeEdge=[&](UINT side,int first,int last,int limit){
            for(int along=first/12*12;along<=last && along<limit;along+=12){
                *origins++=Bath::W+side; *origins++=along; ++count;
            }
        };
        if(bath.x0==0)closeEdge(0,bath.y0,bath.y1,Bath::H-1);
        if(bath.x1==Bath::W-1)closeEdge(1,bath.y0,bath.y1,Bath::H-1);
        if(bath.y0==0)closeEdge(2,bath.x0,bath.x1,Bath::W-1);
        if(bath.y1==Bath::H-1)closeEdge(3,bath.x0,bath.x1,Bath::W-1);
        ctx->Unmap(tiles.Get(),0);ctx->IASetInputLayout(layout.Get());UINT stride=8,offset=0;
        auto vb=tiles.Get();ctx->IASetVertexBuffers(0,1,&vb,&stride,&offset);
        ctx->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,0);
        ctx->VSSetShader(fluidVS.Get(),nullptr,0);ctx->PSSetShader(fluidPS.Get(),nullptr,0);
        ctx->OMSetDepthStencilState(nullptr,0);
        ctx->DrawIndexedInstanced(12*12*6,count,0,0,0);
        if(!moving)last=-1;
        return moving?0:-1;
    }
};
static void* __cdecl create(const A75Create* c){try{return new Studio(*c);}catch(...){return nullptr;}}
static void __cdecl event(void* s,const A75Event* e){try{static_cast<Studio*>(s)->event(*e);}catch(...){}}
static int32_t __cdecl render(void* s,const A75Frame* f){try{return static_cast<Studio*>(s)->draw(*f);}catch(...){return -2;}}
static void __cdecl destroy(void* s){delete static_cast<Studio*>(s);}
extern "C" const A75Wallpaper* __cdecl Action75GetWallpaper(uint32_t abi){
    static const A75Wallpaper api={sizeof(A75Wallpaper),1,create,event,render,destroy,nullptr};
    return abi==1?&api:nullptr;
}
