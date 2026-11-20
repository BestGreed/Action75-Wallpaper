#include "EffekseerBackend.h"
#include "EffectPalette.h"

#include <d3d11.h>
#include <Effekseer.h>
#include <EffekseerRendererDX11.h>
// The adapter is pinned to 1.80.8. SDK draw time and node material access stay here.
#include <Effekseer/Effekseer.EffectNode.h>
#include <Effekseer/Effekseer.EffectImplemented.h>
#include <Effekseer/Effekseer.ManagerImplemented.h>
#include <Effekseer/Material/Effekseer.MaterialFile.h>
#include <Effekseer/IO/Effekseer.EfkEfcFactory.h>
#include <Effekseer/Model/Effekseer.ProceduralModelGenerator.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>
#include <functional>
#include <fstream>
#include <set>

namespace action75 {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr float sdkFramesPerSecond = 60.0f;
constexpr float maxDormantSeconds = 10.0f;

// The ripple shader collapses its quad when customData1.x * customData2.y < .5.
// Scan the authored, fixed-step curves at load time to bound that visible term.
int rippleLastVisibleFrame(const Effekseer::EffectNodeImplemented& node,
                           const Effekseer::EffectInstanceTerm& term) {
    const auto& first = node.RendererCommon.CustomData1;
    const auto& second = node.RendererCommon.CustomData2;
    const auto& life = node.CommonValues.life;
    if (first.Type != Effekseer::ParameterCustomDataType::FCurve2D ||
        second.Type != Effekseer::ParameterCustomDataType::FCurve2D ||
        life.min != life.max || life.max < 1 || life.max > maxDormantSeconds * sdkFramesPerSecond)
        return term.LastInstanceEndMax;

    class OffsetEndpoint final : public Effekseer::IRandObject {
    public:
        explicit OffsetEndpoint(bool upper) : upper_(upper) {}
        int32_t GetRandInt() override { return 0; }
        float GetRand() override { return upper_ ? 1.0f : 0.0f; }
        float GetRand(float lower, float upper) override { return upper_ ? upper : lower; }
    private:
        bool upper_;
    };
    const auto& erosion = *first.FCurve.Values;
    const auto& opacity = *second.FCurve.Values;
    OffsetEndpoint minimum(false), maximum(true);
    if (erosion.GetOffsets(minimum).GetX() != 0 || erosion.GetOffsets(maximum).GetX() != 0 ||
        opacity.GetOffsets(minimum).GetY() != 0 || opacity.GetOffsets(maximum).GetY() != 0)
        return term.LastInstanceEndMax;

    const float duration = static_cast<float>(life.max);
    for (int frame = life.max; frame >= 0; --frame) {
        const float time = static_cast<float>(frame);
        const float mask = erosion.GetValues(time, duration).GetX() *
                           opacity.GetValues(time, duration).GetY();
        if (!std::isfinite(mask) || mask >= .5f)
            return (std::min)(term.LastInstanceEndMax, term.LastInstanceStartMax + frame);
    }
    return term.LastInstanceStartMax;
}

template <class Factory> class InspectEffect : public Factory {
    bool GetIsResourcesLoadedAutomatically() const override { return false; }
};

class DetectGpuParticles : public Effekseer::GpuParticleFactory {
public:
    bool found = false;
    Effekseer::GpuParticles::ResourceRef CreateResource(const Effekseer::GpuParticles::ParamSet&,
                                                       const Effekseer::Effect*) override {
        found = true;
        return nullptr;
    }
};

// Inspect the runtime effect's actual dependency table before SDK resource loaders run.
void validateImportDependencies(const fs::path& path, const fs::path& package) {
    const auto base = fs::canonical(package);
    const auto effectPath = fs::canonical(path).lexically_relative(base);
    if (effectPath.empty() || *effectPath.begin() == L"..")
        throw std::runtime_error("粒子效果文件超出包目录");
    auto setting = Effekseer::Setting::Create();
    setting->ClearEffectFactory();
    setting->AddEffectFactory(Effekseer::MakeRefPtr<InspectEffect<Effekseer::EffectFactory>>());
    setting->AddEffectFactory(Effekseer::MakeRefPtr<InspectEffect<Effekseer::EfkEfcFactory>>());
    setting->SetProceduralMeshGenerator(nullptr);
    auto gpu = Effekseer::MakeRefPtr<DetectGpuParticles>();
    setting->SetGpuParticleFactory(gpu);
    const auto bytes = fs::file_size(path);
    if (bytes == 0 || bytes > 16 * 1024 * 1024)
        throw std::runtime_error("粒子效果文件大小超出范围");
    std::vector<char> data(static_cast<std::size_t>(bytes));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("无法读取粒子效果文件");
    auto effect = Effekseer::Effect::Create(setting, data.data(), static_cast<int32_t>(data.size()));
    if (!effect)
        throw std::runtime_error("无法解析粒子效果，请使用兼容 Effekseer 1.80.8 的效果文件");
    if (gpu->found || effect->GetWaveCount() || effect->GetMaterialCount() ||
        effect->GetProceduralModelCount())
        throw std::runtime_error("粒子包支持 CPU 粒子和内置材质。GPU 粒子、音频、自定义材质和程序模型暂不可导入");

    const auto validate = [&](int count, auto getPath, const wchar_t* extension) {
        if (count > 256)
            throw std::runtime_error("粒子效果引用的资源过多");
        for (int i = 0; i < count; ++i) {
            const auto* raw = (effect.Get()->*getPath)(i);
            const fs::path name(raw ? raw : u"");
            const auto label = name.u8string();
            const auto units = name.u16string();
            if (name.empty() || name.has_root_path() || units.size() > 240 ||
                path.parent_path().native().size() + units.size() + 2 >= 512 ||
                units.find(u':') != std::u16string::npos)
                throw std::runtime_error("粒子资源必须使用包内相对路径：" + label);
            auto actual = fs::weakly_canonical(path.parent_path() / name);
            const auto relative = actual.lexically_relative(base);
            if (relative.empty() || *relative.begin() == L"..")
                throw std::runtime_error("粒子资源路径超出包目录：" + label);
            if (!fs::is_regular_file(actual))
                throw std::runtime_error("缺少粒子资源：" + label);
            auto suffix = actual.extension().wstring();
            std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                           [](wchar_t c) { return std::towlower(c); });
            if (suffix != extension)
                throw std::runtime_error("粒子资源格式暂不可导入：" + label);
        }
    };
    validate(effect->GetColorImageCount(), &Effekseer::Effect::GetColorImagePath, L".png");
    validate(effect->GetNormalImageCount(), &Effekseer::Effect::GetNormalImagePath, L".png");
    validate(effect->GetDistortionImageCount(), &Effekseer::Effect::GetDistortionImagePath, L".png");
    validate(effect->GetModelCount(), &Effekseer::Effect::GetModelPath, L".efkmodel");
    validate(effect->GetCurveCount(), &Effekseer::Effect::GetCurvePath, L".efkcurve");
}

bool loadedImportResources(const Effekseer::EffectRef& effect) {
    const auto allLoaded = [&](int count, auto get) {
        for (int i = 0; i < count; ++i)
            if (!(effect.Get()->*get)(i))
                return false;
        return true;
    };
    return allLoaded(effect->GetColorImageCount(), &Effekseer::Effect::GetColorImage) &&
           allLoaded(effect->GetNormalImageCount(), &Effekseer::Effect::GetNormalImage) &&
           allLoaded(effect->GetDistortionImageCount(), &Effekseer::Effect::GetDistortionImage) &&
           allLoaded(effect->GetModelCount(), &Effekseer::Effect::GetModel) &&
           allLoaded(effect->GetCurveCount(), &Effekseer::Effect::GetCurve);
}

} // namespace

struct EffekseerBackend::Impl {
    // References are intentionally released in reverse order: effects, manager,
    // renderer, graphics device. The caller must keep its D3D11 context alive.
    Effekseer::Backend::GraphicsDeviceRef graphicsDevice;
    EffekseerRendererDX11::RendererRef renderer;
    Effekseer::ManagerRef manager;
    Effekseer::Backend::TextureRef background;
    ID3D11ShaderResourceView* backgroundView = nullptr; // borrowed; background retains it
    std::unordered_map<int, Effekseer::EffectRef> effects;
    std::unordered_map<std::wstring, int> loadedPaths;
    struct Instance {
        Effekseer::Handle handle;
        int effectId;
        float baseScale;
    };
    struct Controls {
        std::array<float, 4>* tint;
        std::array<float, 4>* intensity;
        std::array<float, 4>* palette;
        const std::array<float, 4>* pigmentTiming;
    };
    struct EffectControls {
        std::vector<Controls> bindings;
        std::size_t paletteIndex=0;
        int overlayEnd = 0;
        bool tracksVisibility = false;
    };
    std::unordered_map<int, EffectControls> controlledEffects;
    struct UniformTarget {std::array<float,4>* value; std::array<float,4> original;};
    std::unordered_map<int,std::unordered_map<std::string,std::vector<UniformTarget>>> namedUniforms;
    std::unordered_map<int,std::vector<std::pair<std::string,unsigned>>> overriddenUniforms;
    std::unordered_map<std::wstring,std::vector<std::string>> uniformMetadata;
    std::vector<Instance> instances;
    std::string error;
    unsigned width = 1;
    unsigned height = 1;
    int nextEffectId = 1;
    double elapsedSeconds = 0.0;
    std::optional<Clock::time_point> idleSince;
    float appearanceScale = 1, brightness = 1, intensity = 1, playbackSpeed = 1;
    std::array<std::uint8_t, 3> color{255, 255, 255};
    std::array<std::optional<float>, 4> dynamicInputs{};
    float saturation=1, hueRadians=0;
    bool usePalette=false;
    std::vector<std::array<float, 3>> paletteColors;

    void ApplyUniforms() {
        for (auto& entry : controlledEffects) {
            std::array<float, 4> selected{1,1,1,0};
            if (usePalette && !paletteColors.empty()) {
                const auto& rgb=paletteColors[entry.second.paletteIndex%paletteColors.size()];
                selected={rgb[0],rgb[1],rgb[2],1};
            }
            for (auto& controls : entry.second.bindings) {
                *controls.tint = {color[0]/255.0f, color[1]/255.0f, color[2]/255.0f, brightness};
                *controls.intensity = {intensity, saturation, hueRadians, playbackSpeed};
                if (controls.palette) *controls.palette=selected;
            }
        }
    }
    void ApplyInstance(const Instance& instance) {
        if (!manager) return;
        const float scale = instance.baseScale * appearanceScale;
        manager->SetScale(instance.handle, scale, scale, scale);
        if (controlledEffects.find(instance.effectId) == controlledEffects.end()) {
            // Generic effects have only the SDK's byte tint. Our float-uniform
            // contract supplies independent brightness/color depth for Paintball.
            const float gain = (std::min)(brightness, 1.0f);
            manager->SetAllColor(instance.handle, Effekseer::Color(
                static_cast<std::uint8_t>(std::lround(color[0]*gain)),
                static_cast<std::uint8_t>(std::lround(color[1]*gain)),
                static_cast<std::uint8_t>(std::lround(color[2]*gain)),
                static_cast<std::uint8_t>(std::lround(255*(std::min)(intensity, 1.0f)))));
        }
    }
    bool RegisterControls(int id, const Effekseer::EffectRef& effect, std::size_t paletteIndex) {
        std::vector<Controls> bindings;
        const int end = effect->CalculateTerm().TermMax;
        const auto* nativeEffect = static_cast<const Effekseer::EffectImplemented*>(effect.Get());
        bool tracksVisibility = end > 0 && end + 2.0f < maxDormantSeconds * sdkFramesPerSecond &&
                                nativeEffect->GetDynamicEquation().empty();
        bool hasPigment = false;
        int overlayEnd = 0;
        const auto& names = namedUniforms.at(id);
        const auto timing = names.find("RuntimePipeline");
        std::function<bool(Effekseer::EffectNode*, Effekseer::EffectInstanceTerm, unsigned)> visit =
          [&](Effekseer::EffectNode* node, Effekseer::EffectInstanceTerm term, unsigned depth) {
            const auto basic = node->GetBasicRenderParameter();
            const auto* nativeNode = static_cast<Effekseer::EffectNodeImplemented*>(node);
            const auto type = node->GetType();
            if (depth > 2)
                tracksVisibility = false;
            if (basic.MaterialType == Effekseer::RendererMaterialType::File && basic.MaterialIndex >= 0) {
                if (basic.MaterialIndex >= effect->GetMaterialCount()) return false;
                const auto path = effect->GetMaterialPath(basic.MaterialIndex);
                if (!path) return false;
                const auto name = std::filesystem::path(path).filename().u16string();
                const bool splat = name.rfind(u"PaintSplat_", 0) == 0;
                const bool ripple = name.rfind(u"PaintSplash_", 0) == 0;
                if (!splat && name.rfind(u"PaintFlash_", 0) != 0 &&
                    name.rfind(u"PaintDrop_", 0) != 0 && !ripple)
                    return false;
                auto& uniforms = static_cast<Effekseer::EffectNodeImplemented*>(node)
                                     ->RendererCommon.MaterialData.MaterialUniforms;
                const std::size_t original = splat ? 2u : 1u;
                if (uniforms.size()!=original+2 && uniforms.size()!=original+3) return false;
                const std::array<float, 4>* pigmentTiming = nullptr;
                if (splat && timing != names.end()) {
                    for (const auto& target : timing->second)
                        if (target.value == &uniforms[0] || target.value == &uniforms[1])
                            pigmentTiming = target.value;
                }
                hasPigment = hasPigment || pigmentTiming;
                tracksVisibility = tracksVisibility && type == Effekseer::EffectNodeType::Sprite &&
                                   (!splat || pigmentTiming);
                if (!splat && tracksVisibility) {
                    const int last = ripple ? rippleLastVisibleFrame(*nativeNode, term)
                                            : term.LastInstanceEndMax;
                    // A child can spawn on the next simulation step.
                    overlayEnd = (std::max)(overlayEnd, last + 2);
                }
                bindings.push_back({&uniforms[original], &uniforms[original+1],
                                    uniforms.size()==original+3 ? &uniforms[original+2] : nullptr,
                                    pigmentTiming});
            } else if (nativeNode->IsRendered && type != Effekseer::EffectNodeType::Root &&
                       type != Effekseer::EffectNodeType::NoneType) {
                tracksVisibility = false;
            }
            for (int child=0; child<node->GetChildrenCount(); ++child) {
                auto* next = node->GetChild(child);
                if (!visit(next, next->CalculateInstanceTerm(term), depth + 1)) return false;
            }
            return true;
        };
        if (!visit(effect->GetRoot(), {}, 0) || bindings.empty()) {
            error = "Controlled Paintball material contract mismatch (expected appended tint/brightness and intensity uniforms).";
            return false;
        }
        controlledEffects[id] = {std::move(bindings), paletteIndex, overlayEnd,
                                 tracksVisibility && hasPigment};
        ApplyUniforms();
        return true;
    }
    void RegisterNamedUniforms(int id,const Effekseer::EffectRef& effect,
                               const std::filesystem::path& effectPath) {
        auto& named=namedUniforms[id];
        std::function<void(Effekseer::EffectNode*)> visit=[&](Effekseer::EffectNode* node) {
            const auto basic=node->GetBasicRenderParameter();
            if(basic.MaterialType==Effekseer::RendererMaterialType::File && basic.MaterialIndex>=0 &&
                basic.MaterialIndex<effect->GetMaterialCount()) {
                const auto* relative=effect->GetMaterialPath(basic.MaterialIndex);
                if(relative) {
                    const auto file=(effectPath.parent_path()/std::filesystem::path(relative)).lexically_normal();
                    const auto key=file.wstring();
                    auto metadata=uniformMetadata.find(key);
                    if(metadata==uniformMetadata.end()) {
                        std::vector<std::string> names;
                        std::ifstream input(file,std::ios::binary);
                        if(input) {
                            input.seekg(0,std::ios::end);const auto bytes=input.tellg();input.seekg(0);
                            if(bytes>0 && bytes<=4*1024*1024) {
                                std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));
                                input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
                                Effekseer::MaterialFile material;
                                if(input && material.Load(data.data(),static_cast<int32_t>(data.size())) &&
                                    material.GetUniformCount()>=0 && material.GetUniformCount()<=256)
                                    for(int index=0;index<material.GetUniformCount();++index)
                                        names.emplace_back(material.GetUniformName(index));
                            }
                        }
                        metadata=uniformMetadata.emplace(key,std::move(names)).first;
                    }
                    auto& uniforms=static_cast<Effekseer::EffectNodeImplemented*>(node)->RendererCommon.MaterialData.MaterialUniforms;
                    if(metadata->second.size()==uniforms.size())
                        for(std::size_t index=0;index<uniforms.size();++index)
                            named[metadata->second[index]].push_back({&uniforms[index],uniforms[index]});
                }
            }
            for(int child=0;child<node->GetChildrenCount();++child) visit(node->GetChild(child));
        };
        visit(effect->GetRoot());
    }

    void PruneInstances() {
        if (!manager) {
            instances.clear();
            return;
        }
        instances.erase(std::remove_if(instances.begin(), instances.end(),
            [this](const Instance& instance) { return !manager->Exists(instance.handle); }),
            instances.end());
    }
};

EffekseerBackend::EffekseerBackend() : impl_(std::make_unique<Impl>()) {}
EffekseerBackend::~EffekseerBackend() { Shutdown(); }

bool EffekseerBackend::Initialize(ID3D11Device* device,
                                 ID3D11DeviceContext* context,
                                 int maxParticles) {
    Shutdown();
    impl_->error.clear();
    if (!device || !context || maxParticles < 1 || maxParticles > 65536) {
        impl_->error = "Invalid D3D11 device/context or particle capacity (1..65536).";
        return false;
    }
    impl_->graphicsDevice = EffekseerRendererDX11::CreateGraphicsDevice(device, context);
    if (!impl_->graphicsDevice) {
        impl_->error = "Effekseer could not wrap the supplied D3D11 device/context.";
        return false;
    }
    impl_->renderer = EffekseerRendererDX11::Renderer::Create(
        impl_->graphicsDevice, maxParticles);
    impl_->manager = Effekseer::Manager::Create(maxParticles);
    if (!impl_->renderer || !impl_->manager) {
        impl_->error = "Effekseer renderer or particle pool initialization failed.";
        Shutdown();
        return false;
    }

    auto& manager = impl_->manager;
    auto& renderer = impl_->renderer;
    manager->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);
    manager->SetSpriteRenderer(renderer->CreateSpriteRenderer());
    manager->SetRibbonRenderer(renderer->CreateRibbonRenderer());
    manager->SetRingRenderer(renderer->CreateRingRenderer());
    manager->SetTrackRenderer(renderer->CreateTrackRenderer());
    manager->SetModelRenderer(renderer->CreateModelRenderer());
    manager->SetTextureLoader(renderer->CreateTextureLoader());
    manager->SetModelLoader(renderer->CreateModelLoader());
    manager->SetMaterialLoader(renderer->CreateMaterialLoader());
    manager->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>());
    impl_->instances.reserve(static_cast<std::size_t>(maxParticles));
    // Sound and GPU-particle systems are deliberately not initialized. This
    // first backend targets the original Paintball's CPU sprite emitters.
    renderer->SetRestorationOfStatesFlag(true);
    Resize(impl_->width, impl_->height);
    return true;
}

void EffekseerBackend::Shutdown() {
    if (!impl_) return;
    StopAll();
    impl_->effects.clear();
    impl_->controlledEffects.clear();
    impl_->namedUniforms.clear();impl_->overriddenUniforms.clear();impl_->uniformMetadata.clear();
    impl_->loadedPaths.clear();
    impl_->background.Reset();
    impl_->backgroundView = nullptr;
    impl_->manager.Reset();
    impl_->renderer.Reset();
    impl_->graphicsDevice.Reset();
    impl_->nextEffectId = 1;
    impl_->elapsedSeconds = 0.0;
}

void EffekseerBackend::Resize(unsigned width, unsigned height) {
    impl_->width = (std::max)(width, 1u);
    impl_->height = (std::max)(height, 1u);
    if (!impl_->renderer || !impl_->manager) return;
    Effekseer::Matrix44 projection;
    projection.OrthographicRH(static_cast<float>(impl_->width),
                             static_cast<float>(impl_->height), 0.1f, 2000.0f);
    Effekseer::Matrix44 camera;
    const Effekseer::Vector3D eye(0.0f, 0.0f, 1000.0f);
    camera.LookAtRH(eye, Effekseer::Vector3D(0.0f, 0.0f, 0.0f),
                    Effekseer::Vector3D(0.0f, 1.0f, 0.0f));
    impl_->renderer->SetProjectionMatrix(projection);
    impl_->renderer->SetCameraMatrix(camera);
    impl_->renderer->SetCameraParameter(Effekseer::Vector3D(0.0f, 0.0f, -1.0f), eye);
    Effekseer::Manager::LayerParameter layer;
    layer.ViewerPosition = eye;
    impl_->manager->SetLayerParameter(0, layer);
}

int EffekseerBackend::LoadEffect(const std::filesystem::path& path, bool runtimeControls,
                                 std::size_t paletteIndex, const std::filesystem::path& package) {
    impl_->error.clear();
    if (!impl_->manager) {
        impl_->error = "Effekseer is not initialized.";
        return -1;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec || !std::filesystem::is_regular_file(absolute, ec)) {
        impl_->error = "Effect file does not exist or is inaccessible.";
        return -1;
    }
    const auto key = absolute.wstring();
    if (!package.empty()) {
        try {
            validateImportDependencies(absolute, package);
        } catch (const std::exception& failure) {
            impl_->error = failure.what();
            return -1;
        }
    }
    const auto existing = impl_->loadedPaths.find(key);
    if (existing != impl_->loadedPaths.end()) {
        if (!package.empty() && !loadedImportResources(impl_->effects.at(existing->second))) {
            impl_->error = "粒子资源存在但无法解码，请检查纹理、模型或曲线";
            return -1;
        }
        if (runtimeControls && impl_->controlledEffects.find(existing->second) == impl_->controlledEffects.end() &&
            !impl_->RegisterControls(existing->second, impl_->effects.at(existing->second), paletteIndex)) return -1;
        if (runtimeControls) {
            impl_->controlledEffects.at(existing->second).paletteIndex=paletteIndex;
            impl_->ApplyUniforms();
        }
        return existing->second;
    }
    const auto utf16Path = absolute.u16string();
    auto effect = Effekseer::Effect::Create(impl_->manager, utf16Path.c_str());
    if (!effect) {
        impl_->error = "Cannot load Effekseer effect. Unreal .pak files are not supported.";
        return -1;
    }
    if (!package.empty() && !loadedImportResources(effect)) {
        impl_->error = "粒子资源存在但无法解码，请检查纹理、模型或曲线";
        return -1;
    }
    if (impl_->nextEffectId == (std::numeric_limits<int>::max)()) {
        impl_->error = "Effect ID capacity exceeded.";
        return -1;
    }
    const int id = impl_->nextEffectId++;
    impl_->RegisterNamedUniforms(id,effect,absolute);
    if (runtimeControls && !impl_->RegisterControls(id, effect, paletteIndex)) {
        impl_->namedUniforms.erase(id);
        return -1;
    }
    impl_->effects.emplace(id, std::move(effect));
    impl_->loadedPaths.emplace(key, id);
    return id;
}

bool EffekseerBackend::UnloadEffect(int effectId) {
    const auto effect = impl_->effects.find(effectId);
    if (effect == impl_->effects.end()) return false;
    if (impl_->manager) {
        for (const auto& instance : impl_->instances)
            if (instance.effectId == effectId) impl_->manager->StopEffect(instance.handle);
        // Release stopped particles without advancing any other live effect.
        Effekseer::Manager::UpdateParameter flush;
        flush.DeltaFrame = 0.0f;
        flush.SyncUpdate = true;
        impl_->manager->Update(flush);
    }
    impl_->instances.erase(std::remove_if(impl_->instances.begin(), impl_->instances.end(),
        [effectId](const Impl::Instance& instance) { return instance.effectId == effectId; }),
        impl_->instances.end());
    for (auto path = impl_->loadedPaths.begin(); path != impl_->loadedPaths.end();) {
        if (path->second == effectId) path = impl_->loadedPaths.erase(path);
        else ++path;
    }
    impl_->effects.erase(effect);
    impl_->controlledEffects.erase(effectId);
    impl_->namedUniforms.erase(effectId);impl_->overriddenUniforms.erase(effectId);
    impl_->PruneInstances();
    return true;
}

void EffekseerBackend::UnloadEffects() {
    StopAll();
    impl_->effects.clear();
    impl_->controlledEffects.clear();
    impl_->namedUniforms.clear();impl_->overriddenUniforms.clear();impl_->uniformMetadata.clear();
    impl_->loadedPaths.clear();
}

int EffekseerBackend::PlayAt(int effectId, float pixelX, float pixelY,
                            float effectUnitsToPixels) {
    impl_->error.clear();
    const auto effect = impl_->effects.find(effectId);
    if (!impl_->manager || effect == impl_->effects.end()) {
        impl_->error = "Unknown effect ID or uninitialized Effekseer backend.";
        return -1;
    }
    if (!std::isfinite(pixelX) || !std::isfinite(pixelY) ||
        !std::isfinite(effectUnitsToPixels) || effectUnitsToPixels <= 0.0f) {
        impl_->error = "Effect position and scale must be finite, with a positive scale.";
        return -1;
    }
    impl_->PruneInstances();
    const auto handle = impl_->manager->Play(effect->second,
        pixelX - static_cast<float>(impl_->width) * 0.5f,
        static_cast<float>(impl_->height) * 0.5f - pixelY, 0.0f);
    if (handle < 0) {
        impl_->error = "Effekseer particle pool is full; this trigger was dropped.";
        return -1;
    }
    impl_->instances.push_back({handle, effectId, effectUnitsToPixels});
    for (std::size_t index = 0; index < impl_->dynamicInputs.size(); ++index)
        if (const auto value = impl_->dynamicInputs[index])
            impl_->manager->SetDynamicInput(handle, static_cast<int32_t>(index), *value);
    impl_->idleSince.reset();
    impl_->ApplyInstance(impl_->instances.back());
    return handle;
}

void EffekseerBackend::Stop(int instanceHandle) {
    if (impl_->manager) impl_->manager->StopEffect(instanceHandle);
    impl_->instances.erase(std::remove_if(impl_->instances.begin(), impl_->instances.end(),
        [instanceHandle](const Impl::Instance& instance) { return instance.handle == instanceHandle; }),
        impl_->instances.end());
}

bool EffekseerBackend::StopOldest() {
    impl_->PruneInstances();
    if (!impl_->manager || impl_->instances.empty()) return false;
    const auto handle = impl_->instances.front().handle;
    impl_->manager->StopEffect(handle);
    impl_->instances.erase(impl_->instances.begin());
    // Effekseer defers StopEffect. A zero-time flush immediately recycles its
    // particles without advancing or resetting the remaining visible marks.
    Effekseer::Manager::UpdateParameter flush;
    flush.DeltaFrame = 0.0f;
    flush.SyncUpdate = true;
    impl_->manager->Update(flush);
    impl_->PruneInstances();
    return true;
}

void EffekseerBackend::StopAll() {
    if (impl_->manager) {
        impl_->manager->StopAllEffects();
        // Stop requests are deferred by Effekseer. Flush them before an idle
        // wait or pack unload so a new trigger can reuse the entire pool.
        Effekseer::Manager::UpdateParameter flush;
        flush.DeltaFrame = 0.0f;
        flush.SyncUpdate = true;
        impl_->manager->Update(flush);
    }
    impl_->instances.clear();
    impl_->idleSince.reset();
}

void EffekseerBackend::SetDynamicInput(int instanceHandle, unsigned index, float value) {
    if (impl_->manager && index < 4 && std::isfinite(value))
        impl_->manager->SetDynamicInput(instanceHandle, static_cast<int>(index), value);
}

void EffekseerBackend::SetColor(int instanceHandle, std::uint8_t red,
                               std::uint8_t green, std::uint8_t blue,
                               std::uint8_t alpha) {
    if (impl_->manager)
        impl_->manager->SetAllColor(instanceHandle, Effekseer::Color(red, green, blue, alpha));
}

void EffekseerBackend::SetDynamicInputs(const std::array<std::optional<float>, 4>& inputs) {
    if (inputs == impl_->dynamicInputs) return;
    CatchUpIdle();
    const auto previous = impl_->dynamicInputs;
    impl_->dynamicInputs = inputs;
    if (!impl_->manager) return;
    impl_->PruneInstances();
    for (const auto& instance : impl_->instances) {
        const auto defaults = impl_->effects.at(instance.effectId)->GetDefaultDynamicInputs();
        for (std::size_t index = 0; index < inputs.size(); ++index)
            if (inputs[index] != previous[index])
                impl_->manager->SetDynamicInput(instance.handle, static_cast<int32_t>(index),
                                                inputs[index].value_or(defaults[index]));
    }
}

std::array<float, 4> EffekseerBackend::GetDynamicInputs(int instanceHandle) const {
    std::array<float, 4> inputs{};
    if (impl_->manager)
        for (std::size_t index = 0; index < inputs.size(); ++index)
            inputs[index] = impl_->manager->GetDynamicInput(instanceHandle, static_cast<int32_t>(index));
    return inputs;
}

void EffekseerBackend::SetAppearance(float scale, float brightness, float intensity,
                                    const std::array<std::uint8_t, 3>& color, float speed) {
    // Keep the old speed and transforms in effect during the deferred steps.
    CatchUpIdle();
    impl_->appearanceScale = std::isfinite(scale) ? std::clamp(scale, .5f, 3.0f) : 1.0f;
    impl_->brightness = std::isfinite(brightness) ? std::clamp(brightness, 0.0f, 4.0f) : 1.0f;
    impl_->intensity = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 3.0f) : 1.0f;
    impl_->playbackSpeed = std::isfinite(speed) ? std::clamp(speed, .25f, 3.0f) : 1.0f;
    impl_->color = color;
    impl_->ApplyUniforms();
    impl_->PruneInstances();
    for (const auto& instance : impl_->instances) impl_->ApplyInstance(instance);
}

void EffekseerBackend::SetColorGrade(float saturation, float hueDegrees,
                                    const std::vector<std::array<std::uint8_t, 3>>& palette,
                                    bool usePalette) {
    impl_->saturation=std::isfinite(saturation) ? std::clamp(saturation,0.0f,2.5f) : 1.0f;
    const float hue=std::isfinite(hueDegrees) ? std::clamp(hueDegrees,-180.0f,180.0f) : 0.0f;
    impl_->hueRadians=hue*0.01745329251994329577f;
    impl_->usePalette=usePalette && !palette.empty();
    impl_->paletteColors.clear();
    for (std::size_t i=0;i<(std::min)(palette.size(),std::size_t{9});++i)
        impl_->paletteColors.push_back(linearPaletteColor(palette[i]));
    impl_->ApplyUniforms();
}

bool EffekseerBackend::ValidateMaterialUniforms(int effectId,const std::vector<UniformValue>& values,
                                               std::string& error) const {
    if(impl_->effects.find(effectId)==impl_->effects.end()) {error="Unknown effect for material settings";return false;}
    const auto names=impl_->namedUniforms.find(effectId);
    std::set<std::pair<std::string,unsigned>> seen;
    for(const auto& value:values) {
        if(value.name.empty() || value.name.size()>128 || value.name.rfind("A75",0)==0 || value.component>=4 ||
            !std::isfinite(value.value) || std::abs(value.value)>1000000 || !seen.insert({value.name,value.component}).second) {
            error="Invalid or reserved material uniform setting";return false;
        }
        if(names==impl_->namedUniforms.end() || names->second.find(value.name)==names->second.end()) {
            error="Material uniform is not exposed by this wallpaper: "+value.name;return false;
        }
    }
    return true;
}
bool EffekseerBackend::SetMaterialUniforms(int effectId,const std::vector<UniformValue>& values,
                                          std::string& error) {
    if(!ValidateMaterialUniforms(effectId,values,error)) return false;
    auto& names=impl_->namedUniforms[effectId];
    for(const auto& previous:impl_->overriddenUniforms[effectId])
        for(auto& target:names[previous.first]) (*target.value)[previous.second]=target.original[previous.second];
    auto& overrides=impl_->overriddenUniforms[effectId];overrides.clear();
    for(const auto& value:values) {
        for(auto& target:names.at(value.name)) (*target.value)[value.component]=value.value;
        overrides.emplace_back(value.name,value.component);
    }
    return true;
}

void EffekseerBackend::SetBackground(ID3D11ShaderResourceView* background) {
    if (!impl_->renderer || impl_->backgroundView == background) return;
    impl_->background = background
        ? EffekseerRendererDX11::CreateTexture(impl_->graphicsDevice, background, nullptr, nullptr)
        : Effekseer::Backend::TextureRef{};
    impl_->backgroundView = background;
    static_cast<EffekseerRenderer::Renderer*>(impl_->renderer.Get())
        ->SetBackground(impl_->background);
}

void EffekseerBackend::Update(float elapsedSeconds) {
    if (!impl_->manager || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0f) return;
    // Bound a single update after resume/debug stalls without an unbounded
    // catch-up loop. Normal 60/120 Hz frames retain their exact elapsed time.
    const float seconds = (std::min)(elapsedSeconds, 0.25f);
    impl_->elapsedSeconds += seconds;
    Effekseer::Manager::UpdateParameter update;
    update.DeltaFrame = seconds * sdkFramesPerSecond;
    update.UpdateInterval = 1.0f;
    update.SyncUpdate = true;
    impl_->manager->Update(update);
    impl_->PruneInstances();
}

bool EffekseerBackend::CatchUpIdle() {
    if (!impl_->idleSince) return false;
    const auto now = Clock::now();
    float remaining = (std::min)(maxDormantSeconds,
        std::chrono::duration<float>(now - *impl_->idleSince).count() * impl_->playbackSpeed);
    impl_->idleSince = now;
    // Only bounded Paintball effects can enter this state. Run their usual
    // fixed steps when needed, so changing retention can reveal a live mark.
    while (remaining > 0 && HasActiveEffects()) {
        const float step = (std::min)(remaining, .25f);
        Update(step);
        remaining -= step;
    }
    return true;
}

void EffekseerBackend::UpdateForFrame(float elapsedSeconds) {
    if (!CatchUpIdle())
        Update(elapsedSeconds);
    if (!HasActiveEffects() || HasVisibleEffects())
        impl_->idleSince.reset();
    else if (!impl_->idleSince)
        impl_->idleSince = Clock::now();
}

bool EffekseerBackend::Draw() {
    if (!impl_->renderer || !impl_->manager) return false;
    if (impl_->instances.empty()) return true;
    impl_->renderer->SetTime(static_cast<float>(impl_->elapsedSeconds));
    if (!impl_->renderer->BeginRendering()) {
        impl_->error = "Effekseer BeginRendering failed.";
        return false;
    }
    Effekseer::Manager::DrawParameter draw;
    draw.ZNear = 0.0f;
    draw.ZFar = 1.0f;
    draw.CameraPosition = Effekseer::Vector3D(0.0f, 0.0f, 1000.0f);
    draw.CameraFrontDirection = Effekseer::Vector3D(0.0f, 0.0f, -1.0f);
    draw.ViewProjectionMatrix = impl_->renderer->GetCameraProjectionMatrix();
    impl_->manager->Draw(draw);
    impl_->renderer->EndRendering();
    return true;
}

bool EffekseerBackend::HasActiveEffects() const { return !impl_->instances.empty(); }
bool EffekseerBackend::HasVisibleEffects() const {
    if (impl_->instances.empty()) return false;
    const auto& playing =
        static_cast<const Effekseer::ManagerImplemented*>(impl_->manager.Get())->GetPlayingDrawSets();
    for (const auto& instance : impl_->instances) {
        const auto controlled = impl_->controlledEffects.find(instance.effectId);
        if (controlled == impl_->controlledEffects.end() || !controlled->second.tracksVisibility)
            return true;
        const auto drawSet = playing.find(instance.handle);
        if (drawSet == playing.end()) return true;
        // Use the same integer-step local clock as the sprite material.
        const float frame = drawSet->second.GlobalPointer->GetUpdatedFrame();
        if (frame <= controlled->second.overlayEnd) return true;
        const float pigmentAge = (frame / sdkFramesPerSecond - .75f) / impl_->playbackSpeed;
        for (const auto& controls : controlled->second.bindings) {
            if (!controls.pigmentTiming) continue;
            const auto& uniform = *controls.pigmentTiming;
            const float hold = uniform[2] - 1.0f;
            const float fade = uniform[3] > 0.0f ? uniform[3] : .25f;
            // The small margin covers shader/CPU floating-point rounding.
            if (hold < 0.0f || pigmentAge <= hold + fade + .00001f)
                return true;
        }
    }
    return false;
}
std::size_t EffekseerBackend::ActiveEffectCount() const { return impl_->instances.size(); }
int EffekseerBackend::ActiveParticleCount() const {
    return impl_->manager ? impl_->manager->GetTotalInstanceCount() : 0;
}
const std::string& EffekseerBackend::LastError() const { return impl_->error; }

} // namespace action75
