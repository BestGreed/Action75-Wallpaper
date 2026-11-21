#include "Action75Wallpaper12.h"
#include "ParticleProgram.h"
#include <Effekseer.h>
#include <EffekseerRendererDX12.h>
#include <wrl/client.h>
#include <memory>
#include <limits>

using Microsoft::WRL::ComPtr;
namespace author_particles {
struct Instance {
    Program program;
    // The private queue is used only by the SDK's synchronous resource uploader.
    // All frame rendering appends to the ABI2 command list owned by the worker.
    ComPtr<ID3D12CommandQueue> uploads;
    Effekseer::Backend::GraphicsDeviceRef graphics;
    EffekseerRenderer::RendererRef renderer;
    Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> pool;
    Effekseer::RefPtr<EffekseerRenderer::CommandList> commands;
    Effekseer::ManagerRef manager;
    std::vector<Effekseer::EffectRef> effects;
    std::vector<Effekseer::Handle> handles;
    unsigned width=0,height=0;
    double last=-1,elapsed=0;
    bool paused=false;

    explicit Instance(const std::filesystem::path& directory):program(directory) {
        // Production shuffle changes between launches; tests can seed this
        // package-side state directly without adding a public debug control.
        program.random^=(GetTickCount64()<<16)^GetCurrentProcessId();
    }
    ~Instance() {
        if(manager) manager->StopAllEffects();
        effects.clear();manager.Reset();
        if(renderer) renderer->SetCommandList(nullptr);
        commands.Reset();pool.Reset();renderer.Reset();graphics.Reset();
    }
    bool initialize(const A75Create12& create) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        if(FAILED(create.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&uploads)))) throw std::runtime_error("Effekseer DX12 resource-upload queue creation failed");
        graphics=EffekseerRendererDX12::CreateGraphicsDevice(create.device,uploads.Get(),1);
        if(!graphics) throw std::runtime_error("Effekseer DX12 graphics device creation failed");
        DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM;
        renderer=EffekseerRendererDX12::Create(graphics,&format,1,DXGI_FORMAT_UNKNOWN,false,8192);
        if(!renderer) throw std::runtime_error("Effekseer DX12 renderer creation failed");
        pool=EffekseerRenderer::CreateSingleFrameMemoryPool(graphics);
        commands=EffekseerRenderer::CreateCommandList(graphics,pool);
        if(!pool || !commands) throw std::runtime_error("Effekseer DX12 frame command wrapper creation failed");
        manager=Effekseer::Manager::Create(8192);
        manager->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);
        manager->SetSpriteRenderer(renderer->CreateSpriteRenderer());
        manager->SetRibbonRenderer(renderer->CreateRibbonRenderer());
        manager->SetRingRenderer(renderer->CreateRingRenderer());
        manager->SetTrackRenderer(renderer->CreateTrackRenderer());
        manager->SetModelRenderer(renderer->CreateModelRenderer());
        manager->SetTextureLoader(renderer->CreateTextureLoader());
        manager->SetModelLoader(renderer->CreateModelLoader());
        manager->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>());
        for(const auto& path:program.resources) {
            const auto name=path.u16string();
            auto effect=Effekseer::Effect::Create(manager,name.c_str());
            if(!effect) throw std::runtime_error("Cannot load original CPU particle effect");
            for(int i=0;i<effect->GetColorImageCount();++i) if(!effect->GetColorImage(i)) throw std::runtime_error("Missing particle texture");
            effects.push_back(effect);
        }
        resize(create.width,create.height);
        program.configure(create.settings_json);
        return true;
    }
    void resize(unsigned w,unsigned h) {
        width=std::max(1u,w);height=std::max(1u,h);
        Effekseer::Matrix44 projection,camera;
        projection.OrthographicRH(float(width),float(height),.1f,2000.f);
        const Effekseer::Vector3D eye(0,0,1000);
        camera.LookAtRH(eye,Effekseer::Vector3D(0,0,0),Effekseer::Vector3D(0,1,0));
        renderer->SetProjectionMatrix(projection);renderer->SetCameraMatrix(camera);
        renderer->SetCameraParameter(Effekseer::Vector3D(0,0,-1),eye);
        Effekseer::Manager::LayerParameter layer;layer.ViewerPosition=eye;manager->SetLayerParameter(0,layer);
    }
    void prune() {
        handles.erase(std::remove_if(handles.begin(),handles.end(),[&](auto handle){return !manager->Exists(handle);}),handles.end());
    }
    void appearance(Effekseer::Handle handle) {
        const float scale=program.baseScale*program.scale;
        manager->SetScale(handle,scale,scale,scale);
        const auto gain=static_cast<std::uint8_t>(std::lround(255*std::clamp(program.brightness,0.f,1.f)));
        manager->SetAllColor(handle,Effekseer::Color(gain,gain,gain,255));
        for(unsigned i=0;i<4;++i) if(program.dynamicBound[i]) manager->SetDynamicInput(handle,i,program.dynamic[i]);
    }
    void trigger(float x,float y,double seconds,bool keyboard) {
        if(paused) return;
        const int selected=program.choose(keyboard);
        if(selected<0 || static_cast<unsigned>(selected)>=effects.size()) return;
        prune();
        if(handles.empty()) last=seconds;
        if(handles.size()>=64) {
            manager->StopEffect(handles.front());handles.erase(handles.begin());
            Effekseer::Manager::UpdateParameter flush;flush.DeltaFrame=0;flush.SyncUpdate=true;manager->Update(flush);
        }
        const auto handle=manager->Play(effects[selected],std::clamp(x,0.f,1.f)*width-width*.5f,height*.5f-std::clamp(y,0.f,1.f)*height,0);
        if(handle>=0) {handles.push_back(handle);appearance(handle);}
    }
    void event(const A75Event& e) {
        switch(e.type) {
        case A75_KEY_DOWN: trigger(e.x,e.y,e.seconds,true);break;
        case A75_POINTER_DOWN: if(!program.selection.empty()) trigger(e.x,e.y,e.seconds,false);break;
        case A75_SETTINGS:
            program.configure(e.settings_json);for(auto handle:handles) appearance(handle);break;
        case A75_RESIZE: resize(e.width,e.height);break;
        case A75_PAUSE: paused=true;last=e.seconds;break;
        case A75_RESUME: paused=false;last=e.seconds;break;
        case A75_INPUT_RESET: {
            manager->StopAllEffects();
            Effekseer::Manager::UpdateParameter flush;flush.DeltaFrame=0;flush.SyncUpdate=true;manager->Update(flush);
            handles.clear();program.cursor=0;last=e.seconds;
            // Legacy stopEffects rewinds order but retains keyboard cadence.
            break;
        }
        default: break;
        }
    }
    int draw(const A75Frame12& frame) {
        if(frame.width!=width || frame.height!=height) resize(frame.width,frame.height);
        const auto clear=program.background();frame.commands->ClearRenderTargetView(frame.rtv,clear.data(),0,nullptr);
        const double dt=last<0?0:std::clamp(frame.seconds-last,0.,.1);
        last=frame.seconds;
        if(!paused) {
            const float step=static_cast<float>(dt)*program.speed;
            elapsed+=step;
            Effekseer::Manager::UpdateParameter update;update.DeltaFrame=step*60.f;update.UpdateInterval=1.f;update.SyncUpdate=true;
            manager->Update(update);prune();
        }
        if(handles.empty()) return -1;
        pool->NewFrame();
        EffekseerRendererDX12::BeginCommandList(commands,frame.commands);
        renderer->SetCommandList(commands);
        renderer->SetTime(static_cast<float>(elapsed));
        if(!renderer->BeginRendering()) throw std::runtime_error("Effekseer DX12 begin rendering failed");
        Effekseer::Manager::DrawParameter draw;
        draw.ZNear=0;draw.ZFar=1;draw.CameraPosition=Effekseer::Vector3D(0,0,1000);draw.CameraFrontDirection=Effekseer::Vector3D(0,0,-1);
        draw.ViewProjectionMatrix=renderer->GetCameraProjectionMatrix();manager->Draw(draw);
        if(!renderer->EndRendering()) throw std::runtime_error("Effekseer DX12 end rendering failed");
        renderer->SetCommandList(nullptr);EffekseerRendererDX12::EndCommandList(commands);
        return paused?-1:0;
    }
};
}

namespace {
void* __cdecl create(const A75Create12* c) noexcept {
    try {
        if(!c || c->size<sizeof(*c) || !c->device || !c->pack_dir || !c->width || !c->height) return nullptr;
        auto instance=std::make_unique<author_particles::Instance>(c->pack_dir);
        return instance->initialize(*c)?instance.release():nullptr;
    } catch(const std::exception& e) {OutputDebugStringA(e.what());return nullptr;}
    catch(...) {OutputDebugStringA("Unknown particle create failure");return nullptr;}
}
void __cdecl event(void* instance,const A75Event* e) noexcept {
    try {if(instance && e && e->size>=sizeof(*e)) static_cast<author_particles::Instance*>(instance)->event(*e);}
    catch(const std::exception& e) {OutputDebugStringA(e.what());}
    catch(...) {OutputDebugStringA("Unknown particle event failure");}
}
int32_t __cdecl render(void* instance,const A75Frame12* f) noexcept {
    try {return instance && f && f->size>=sizeof(*f) && f->commands?static_cast<author_particles::Instance*>(instance)->draw(*f):-2;}
    catch(const std::exception& e) {OutputDebugStringA(e.what());return -2;}
    catch(...) {OutputDebugStringA("Unknown particle render failure");return -2;}
}
void __cdecl destroy(void* instance) noexcept {delete static_cast<author_particles::Instance*>(instance);}
int32_t __cdecl hit(void* instance,float,float,uint32_t,uint32_t) noexcept {
    const auto* p=static_cast<author_particles::Instance*>(instance);
    return p && !p->paused && !p->program.selection.empty();
}
}
extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
    static const A75Wallpaper12 table{sizeof(table),ACTION75_WALLPAPER_ABI12,create,event,render,destroy,hit};
    return abi==ACTION75_WALLPAPER_ABI12?&table:nullptr;
}
