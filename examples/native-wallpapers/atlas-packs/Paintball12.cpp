#include "Wallpaper12.h"
#include "PaintballImages12.h"
#include "PaintballOpacity12.h"

#include <Effekseer.h>
#include <Effekseer/Effekseer.EffectImplemented.h>
#include <Effekseer/Effekseer.EffectNode.h>
#include <Effekseer/Effekseer.ManagerImplemented.h>
#include <Effekseer/Material/Effekseer.MaterialFile.h>
#include <EffekseerRendererDX12.h>
#include <EffekseerRendererCommon/EffekseerRenderer.Renderer_Impl.h>
#include <EffekseerRendererLLGI/EffekseerRendererLLGI.Renderer.h>
#include <EffekseerRendererLLGI/GraphicsDevice.h>
#include <DX12/LLGI.GraphicsDX12.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>

namespace atlas_packs {
namespace {
using Vec4 = std::array<float, 4>;
constexpr float framesPerSecond = 60.f, maxDormantSeconds = 10.f;
constexpr unsigned maxHits = 64;

Effekseer::MaterialFile readMaterial(const fs::path& file) {
  constexpr std::streamoff maxBytes = 4 * 1024 * 1024;
  std::ifstream input(file, std::ios::binary | std::ios::ate);
  const std::streamoff bytes = input.tellg();
  if (bytes <= 0 || bytes > maxBytes)
    throw std::runtime_error("Invalid Effekseer material size");
  input.seekg(0);
  std::vector<uint8_t> data(static_cast<std::size_t>(bytes));
  input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  Effekseer::MaterialFile material;
  if (!input || !material.Load(data.data(), static_cast<int32_t>(data.size())))
    throw std::runtime_error("Could not read the Paintball material uniforms");
  return material;
}

// The ripple material collapses its quad when erosion times opacity is
// below 0.5. Derive its contribution to the visible lifetime from its curves.
int rippleLastVisibleFrame(const Effekseer::EffectNodeImplemented& node,
                           const Effekseer::EffectInstanceTerm& term) {
  const auto& first = node.RendererCommon.CustomData1;
  const auto& second = node.RendererCommon.CustomData2;
  const auto& life = node.CommonValues.life;
  if (first.Type != Effekseer::ParameterCustomDataType::FCurve2D ||
      second.Type != Effekseer::ParameterCustomDataType::FCurve2D ||
      life.min != life.max || life.max < 1 || life.max > maxDormantSeconds * framesPerSecond)
    return term.LastInstanceEndMax;

  class OffsetEndpoint final : public Effekseer::IRandObject {
  public:
    explicit OffsetEndpoint(bool upper) : upper_(upper) {}
    int32_t GetRandInt() override { return 0; }
    float GetRand() override { return upper_ ? 1.f : 0.f; }
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
    const float mask = erosion.GetValues(time, duration).GetX() * opacity.GetValues(time, duration).GetY();
    if (!std::isfinite(mask) || mask >= .5f)
      return std::min(term.LastInstanceEndMax, term.LastInstanceStartMax + frame);
  }
  return term.LastInstanceStartMax;
}

struct PigmentBindings {
  Vec4* tint;
  Vec4* grade;
  Vec4* palette;
  const Vec4* timing;
};

struct Effect {
  Effekseer::EffectRef resource;
  std::vector<PigmentBindings> pigments;
  std::unordered_map<std::string, std::vector<Vec4*>> uniforms;
  int overlayEnd = 0;
  bool tracksVisibility = false;
};

struct ParticleInstance {
  Effekseer::Handle handle;
  size_t effect;
  float baseScale;
};

class Engine {
public:
  Engine(ID3D12Device* device, ID3D12CommandQueue* uploads, unsigned capacity,
         PaintballImageUploads* images = nullptr, const fs::path& cacheDirectory = {})
      : graphics_(EffekseerRendererDX12::CreateGraphicsDevice(device, uploads, 1)),
        manager_(Effekseer::Manager::Create(capacity)) {
    DXGI_FORMAT targetFormat = format;
    renderer_ = EffekseerRendererDX12::Create(graphics_, &targetFormat, 1, DXGI_FORMAT_UNKNOWN,
                                               false, capacity);
    if (!renderer_ || !manager_)
      throw std::runtime_error("Initialize the Effekseer DX12 renderer and particle pool");
    renderer_->GetImpl()->IsPremultipliedAlphaEnabled = true;
    // ABI 2 fences each frame before another callback. One frame of constants
    // is sufficient, and the CPU sprite effects use no GPU particle constants.
    auto backend = graphics_.DownCast<EffekseerRendererLLGI::Backend::GraphicsDevice>();
    const auto memory = LLGI::CreateSharedPtr(backend->GetGraphics()->CreateSingleFrameMemoryPool(2 * 1024 * 1024, 128));
    pool_ = Effekseer::MakeRefPtr<EffekseerRendererLLGI::SingleFrameMemoryPool>(memory.get());
    commands_ = EffekseerRenderer::CreateCommandList(graphics_, pool_);
    if (!pool_ || !commands_)
      throw std::runtime_error("Allocate Effekseer DX12 recording resources");
    manager_->SetCoordinateSystem(Effekseer::CoordinateSystem::RH);
    manager_->SetSpriteRenderer(renderer_->CreateSpriteRenderer());
    manager_->SetRibbonRenderer(renderer_->CreateRibbonRenderer());
    manager_->SetRingRenderer(renderer_->CreateRingRenderer());
    manager_->SetTrackRenderer(renderer_->CreateTrackRenderer());
    manager_->SetModelRenderer(renderer_->CreateModelRenderer());
    auto loader = renderer_->CreateTextureLoader();
    if (images && !cacheDirectory.empty())
      loader = paintballTextureLoader(std::move(loader), graphics_, *images, cacheDirectory);
    manager_->SetTextureLoader(loader);
    manager_->SetModelLoader(renderer_->CreateModelLoader());
    manager_->SetMaterialLoader(renderer_->CreateMaterialLoader());
    manager_->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>());
    instances_.reserve(maxHits);

    const Effekseer::Vector3D eye(0, 0, 1000);
    Effekseer::Matrix44 camera;
    camera.LookAtRH(eye, Effekseer::Vector3D(0, 0, 0), Effekseer::Vector3D(0, 1, 0));
    renderer_->SetCameraMatrix(camera);
    renderer_->SetCameraParameter(Effekseer::Vector3D(0, 0, -1), eye);
    Effekseer::Manager::LayerParameter layer;
    layer.ViewerPosition = eye;
    manager_->SetLayerParameter(0, layer);
  }

  void resize(unsigned width, unsigned height) {
    if (width_ == width && height_ == height)
      return;
    width_ = width;
    height_ = height;
    Effekseer::Matrix44 projection;
    projection.OrthographicRH(float(width_), float(height_), .1f, 2000.f);
    renderer_->SetProjectionMatrix(projection);
  }

  size_t load(const fs::path& path, bool runtimeControls) {
    const auto filename = path.u16string();
    Effect effect;
    effect.resource = Effekseer::Effect::Create(manager_, filename.c_str());
    if (!effect.resource)
      throw std::runtime_error("Could not load the authored Effekseer effect");
    auto* resource = effect.resource.Get();
    auto complete = [&](int count, auto get) {
      for (int i = 0; i < count; ++i)
        if (!(resource->*get)(i))
          return false;
      return true;
    };
    if (!complete(resource->GetColorImageCount(), &Effekseer::Effect::GetColorImage) ||
        !complete(resource->GetNormalImageCount(), &Effekseer::Effect::GetNormalImage) ||
        !complete(resource->GetDistortionImageCount(), &Effekseer::Effect::GetDistortionImage) ||
        !complete(resource->GetMaterialCount(), &Effekseer::Effect::GetMaterial) ||
        !complete(resource->GetModelCount(), &Effekseer::Effect::GetModel) ||
        !complete(resource->GetCurveCount(), &Effekseer::Effect::GetCurve))
      throw std::runtime_error("An authored Paintball resource failed to load");
    if (runtimeControls)
      bindMaterials(effect, path.parent_path());
    effects_.push_back(std::move(effect));
    return effects_.size() - 1;
  }

  void setBackground(ID3D12Resource* canvas, D3D12_RESOURCE_STATES state) {
    renderer_->SetBackground(paintballTexture(graphics_, canvas, state));
  }

  ID3D12CommandQueue* directQueue() {
    auto backend = graphics_.DownCast<EffekseerRendererLLGI::Backend::GraphicsDevice>();
    return static_cast<LLGI::GraphicsDX12*>(backend->GetGraphics())->GetCommandQueue();
  }
  bool samplesBackground() const { return samplesBackground_; }

  void configure(const Appearance& values, double now) {
    catchUp(now);
    const auto previousInputs = appearance_.dynamicInputs;
    appearance_ = values;
    appearance_.scale = std::clamp(values.scale, .5f, 3.f);
    appearance_.brightness = std::clamp(values.brightness, 0.f, 4.f);
    appearance_.intensity = std::clamp(values.intensity, 0.f, 3.f);
    appearance_.speed = std::clamp(values.speed, .25f, 3.f);
    const float radians = std::clamp(values.hue, -180.f, 180.f) * .01745329251994329577f;
    const float saturation = std::clamp(values.saturation, 0.f, 2.5f);

    std::set<std::pair<std::string, unsigned>> seen;
    for (const auto& uniform : values.uniforms) {
      if (uniform.component >= 4 || uniform.name.rfind("A75", 0) == 0 ||
          !seen.emplace(uniform.name, uniform.component).second)
        throw std::runtime_error("Invalid or reserved paint material uniform");
    }
    auto toLinear = [](uint8_t byte) {
      const float v = byte / 255.f;
      return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
    };
    for (size_t i = 0; i < effects_.size(); ++i) {
      auto& effect = effects_[i];
      Vec4 palette{1, 1, 1, 0};
      if (!values.palette.empty()) {
        const auto& rgb = values.palette[i % values.palette.size()];
        palette = {toLinear(rgb[0]), toLinear(rgb[1]), toLinear(rgb[2]), 1};
      }
      for (const auto& binding : effect.pigments) {
        *binding.tint = {values.color[0] / 255.f, values.color[1] / 255.f,
                         values.color[2] / 255.f, appearance_.brightness};
        *binding.grade = {appearance_.intensity, saturation, radians, appearance_.speed};
        if (binding.palette)
          *binding.palette = palette;
      }
      for (const auto& uniform : values.uniforms) {
        const auto targets = effect.uniforms.find(uniform.name);
        if (targets == effect.uniforms.end())
          throw std::runtime_error("Paint material does not expose " + uniform.name);
        for (auto* target : targets->second)
          (*target)[uniform.component] = uniform.value;
      }
    }
    prune();
    for (const auto& instance : instances_) {
      setScale(instance);
      const auto defaults = effects_[instance.effect].resource->GetDefaultDynamicInputs();
      for (unsigned i = 0; i < values.dynamicInputs.size(); ++i)
        if (values.dynamicInputs[i] != previousInputs[i])
          manager_->SetDynamicInput(instance.handle, int(i), values.dynamicInputs[i].value_or(defaults[i]));
    }
  }

  bool play(size_t effect, float x, float y, float scale) {
    prune();
    if (instances_.size() >= maxHits) {
      manager_->StopEffect(instances_.front().handle);
      instances_.erase(instances_.begin());
      flushStops();
    }
    const auto handle = manager_->Play(effects_.at(effect).resource,
                                       x * width_ - width_ * .5f, height_ * .5f - y * height_, 0);
    if (handle < 0)
      return false;
    instances_.push_back({handle, effect, scale});
    setScale(instances_.back());
    for (unsigned i = 0; i < appearance_.dynamicInputs.size(); ++i)
      if (auto value = appearance_.dynamicInputs[i])
        manager_->SetDynamicInput(handle, int(i), *value);
    idleSince_.reset();
    return true;
  }

  void clear() {
    manager_->StopAllEffects();
    flushStops();
    instances_.clear();
    idleSince_.reset();
  }

  void update(float seconds) {
    seconds = std::min(seconds, .25f);
    animationTime_ += seconds;
    Effekseer::Manager::UpdateParameter update;
    update.DeltaFrame = seconds * framesPerSecond;
    update.UpdateInterval = 1.f;
    update.SyncUpdate = true;
    manager_->Update(update);
    prune();
  }

  bool catchUp(double now) {
    if (!idleSince_)
      return false;
    float remaining = std::clamp(float(now - *idleSince_) * appearance_.speed, 0.f, maxDormantSeconds);
    idleSince_ = now;
    while (remaining > 0 && active()) {
      const float step = std::min(remaining, .25f);
      update(step);
      remaining -= step;
    }
    return true;
  }

  void updateFrame(double now, double elapsed, bool drawForeground) {
    if (!catchUp(now))
      update(float(std::clamp(elapsed, 0., .1)) * appearance_.speed);
    if (!active() || (drawForeground && visible()))
      idleSince_.reset();
    else if (!idleSince_)
      idleSince_ = now;
  }

  void draw(const A75Frame12& frame) {
    if (!active())
      return;
    resize(frame.width, frame.height);
    bindTarget(frame);
    pool_->NewFrame();
    EffekseerRendererDX12::BeginCommandList(commands_, frame.commands);
    renderer_->SetCommandList(commands_);
    struct EndRecording {
      EffekseerRenderer::RendererRef& renderer;
      Effekseer::RefPtr<EffekseerRenderer::CommandList>& commands;
      ~EndRecording() {
        renderer->SetCommandList(nullptr);
        EffekseerRendererDX12::EndCommandList(commands);
      }
    } recording{renderer_, commands_};
    renderer_->SetTime(float(animationTime_));
    if (!renderer_->BeginRendering())
      throw std::runtime_error("Start recording Effekseer DX12 sprites");
    Effekseer::Manager::DrawParameter parameters;
    parameters.ZNear = 0;
    parameters.ZFar = 1;
    parameters.CameraPosition = Effekseer::Vector3D(0, 0, 1000);
    parameters.CameraFrontDirection = Effekseer::Vector3D(0, 0, -1);
    parameters.ViewProjectionMatrix = renderer_->GetCameraProjectionMatrix();
    manager_->Draw(parameters);
    if (!renderer_->EndRendering())
      throw std::runtime_error("Finish recording Effekseer DX12 sprites");
  }

  bool active() const { return !instances_.empty(); }

  bool visible() const {
    const auto& playing =
        static_cast<const Effekseer::ManagerImplemented*>(manager_.Get())->GetPlayingDrawSets();
    for (const auto& instance : instances_) {
      const auto& effect = effects_[instance.effect];
      const auto drawSet = playing.find(instance.handle);
      if (!effect.tracksVisibility || drawSet == playing.end())
        return true;
      const float frame = drawSet->second.GlobalPointer->GetUpdatedFrame();
      if (frame <= effect.overlayEnd)
        return true;
      const float age = (frame / framesPerSecond - .75f) / appearance_.speed;
      for (const auto& binding : effect.pigments) {
        if (!binding.timing)
          continue;
        const float hold = (*binding.timing)[2] - 1.f;
        const float duration = (*binding.timing)[3] > 0.f ? (*binding.timing)[3] : .25f;
        if (hold < 0 || age <= hold + duration + .00001f)
          return true;
      }
    }
    return false;
  }

private:
  void bindMaterials(Effect& effect, const fs::path& directory) {
    const auto* source = static_cast<const Effekseer::EffectImplemented*>(effect.resource.Get());
    const int end = effect.resource->CalculateTerm().TermMax;
    effect.tracksVisibility = end > 0 && end + 2.f < maxDormantSeconds * framesPerSecond &&
                              source->GetDynamicEquation().empty();
    bool hasPigment = false;
    std::function<void(Effekseer::EffectNode*, Effekseer::EffectInstanceTerm, unsigned)> visit;
    visit = [&](Effekseer::EffectNode* node, Effekseer::EffectInstanceTerm term, unsigned depth) {
      auto* native = static_cast<Effekseer::EffectNodeImplemented*>(node);
      const auto basic = node->GetBasicRenderParameter();
      if (native->IsRendered && basic.Distortion)
        samplesBackground_ = true;
      const auto type = node->GetType();
      if (depth > 2)
        effect.tracksVisibility = false;
      if (basic.MaterialType == Effekseer::RendererMaterialType::File && basic.MaterialIndex >= 0) {
        if (basic.MaterialIndex >= effect.resource->GetMaterialCount())
          throw std::runtime_error("Invalid authored Paintball material index");
        if (native->IsRendered && effect.resource->GetMaterial(basic.MaterialIndex)->IsRefractionRequired)
          samplesBackground_ = true;
        const auto* filename = effect.resource->GetMaterialPath(basic.MaterialIndex);
        if (!filename)
          throw std::runtime_error("Paint material path missing");
        const auto path = (directory / fs::path(filename)).lexically_normal();
        auto names = uniformNames_.find(path);
        if (names == uniformNames_.end()) {
          auto material = readMaterial(path);
          std::vector<std::string> keys;
          for (int i = 0; i < material.GetUniformCount(); ++i)
            keys.emplace_back(material.GetUniformName(i));
          names = uniformNames_.emplace(path, std::move(keys)).first;
        }
        auto& uniforms = native->RendererCommon.MaterialData.MaterialUniforms;
        if (names->second.size() != uniforms.size())
          throw std::runtime_error("Paint material uniform layout does not match its node");
        const auto file = path.filename().u16string();
        const bool splat = file.rfind(u"PaintSplat_", 0) == 0;
        const bool ripple = file.rfind(u"PaintSplash_", 0) == 0;
        if (!splat && !ripple && file.rfind(u"PaintFlash_", 0) != 0 &&
            file.rfind(u"PaintDrop_", 0) != 0)
          throw std::runtime_error("Unexpected controlled Paintball material");
        const size_t original = splat ? 2u : 1u;
        if (uniforms.size() != original + 2 && uniforms.size() != original + 3)
          throw std::runtime_error("Expected appended paint tint and intensity uniforms");
        const Vec4* timing = nullptr;
        for (size_t i = 0; i < uniforms.size(); ++i) {
          const auto& name = names->second[i];
          effect.uniforms[name].push_back(&uniforms[i]);
          if (splat && i < original && name == "RuntimePipeline")
            timing = &uniforms[i];
        }
        hasPigment = hasPigment || timing;
        effect.tracksVisibility = effect.tracksVisibility && type == Effekseer::EffectNodeType::Sprite &&
                                  (!splat || timing);
        if (!splat && effect.tracksVisibility) {
          const int last = ripple ? rippleLastVisibleFrame(*native, term) : term.LastInstanceEndMax;
          effect.overlayEnd = std::max(effect.overlayEnd, last + 2);
        }
        effect.pigments.push_back({&uniforms[original], &uniforms[original + 1],
                                    uniforms.size() == original + 3 ? &uniforms[original + 2] : nullptr,
                                    timing});
      } else if (native->IsRendered && type != Effekseer::EffectNodeType::Root &&
                 type != Effekseer::EffectNodeType::NoneType) {
        effect.tracksVisibility = false;
      }
      for (int child = 0; child < node->GetChildrenCount(); ++child) {
        auto* next = node->GetChild(child);
        visit(next, next->CalculateInstanceTerm(term), depth + 1);
      }
    };
    visit(effect.resource->GetRoot(), {}, 0);
    if (effect.pigments.empty())
      throw std::runtime_error("No paint material takes runtime controls");
    effect.tracksVisibility = effect.tracksVisibility && hasPigment;
  }

  void setScale(const ParticleInstance& instance) {
    const float scale = instance.baseScale * appearance_.scale;
    manager_->SetScale(instance.handle, scale, scale, scale);
  }

  void prune() {
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(), [&](const auto& instance) {
      return !manager_->Exists(instance.handle);
    }), instances_.end());
  }

  void flushStops() {
    Effekseer::Manager::UpdateParameter flush;
    flush.DeltaFrame = 0;
    flush.SyncUpdate = true;
    manager_->Update(flush);
    prune();
  }

  Effekseer::Backend::GraphicsDeviceRef graphics_;
  EffekseerRenderer::RendererRef renderer_;
  Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> pool_;
  Effekseer::RefPtr<EffekseerRenderer::CommandList> commands_;
  Effekseer::ManagerRef manager_;
  std::vector<Effect> effects_;
  std::vector<ParticleInstance> instances_;
  std::unordered_map<fs::path, std::vector<std::string>> uniformNames_;
  Appearance appearance_;
  unsigned width_ = 0, height_ = 0;
  double animationTime_ = 0;
  std::optional<double> idleSince_;
  bool samplesBackground_ = false;
};

ComPtr<ID3D12CommandQueue> uploadQueue(ID3D12Device* device) {
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COPY};
  if (SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
    return queue;
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "Create Paintball upload queue");
  return queue;
}

ComPtr<ID3D12Resource> bakeCanvas(ID3D12Device* device, ID3D12CommandQueue* queue, const fs::path& path) {
  constexpr unsigned width = 1920, height = 720;
  Engine baker(device, queue, 256);
  baker.resize(width, height);
  const auto canvas = baker.load(path, false);
  if (!baker.play(canvas, .5f, .5f, 1))
    throw std::runtime_error("Start the noninteractive canvas");
  baker.update(3.f / framesPerSecond);
  if (!baker.active())
    throw std::runtime_error("Canvas did not survive its 3-frame snapshot");

  ComPtr<ID3D12Resource> texture;
  auto info = textureDescription(width, height);
  info.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CLEAR_VALUE black{format, {0, 0, 0, 1}};
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &info,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, &black,
                                        IID_PPV_ARGS(&texture)), "Allocate the authored canvas");
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC viewDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  check(device->CreateDescriptorHeap(&viewDesc, IID_PPV_ARGS(&views)), "Allocate the canvas RTV");
  const auto rtv = views->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;
  ComPtr<ID3D12Fence> fence;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "Allocate canvas commands");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&commands)), "Record canvas commands");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create canvas fence");
  commands->ClearRenderTargetView(rtv, black.Color, 0, nullptr);
  baker.draw({sizeof(A75Frame12), width, height, 0, commands.Get(), texture.Get(), rtv});
  transition(commands.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_GENERIC_READ);
  check(commands->Close(), "Close private canvas commands");
  ID3D12CommandList* submission[]{commands.Get()};
  queue->ExecuteCommandLists(1, submission);
  check(queue->Signal(fence.Get(), 1), "Signal canvas completion");
  check(fence->SetEventOnCompletion(1, nullptr), "Wait for the immutable canvas");
  check(device->GetDeviceRemovedReason(), "GPU renders the authored canvas");
  return texture;
}
} // namespace

class Paintball {
public:
  static constexpr bool canvas = true;
  Paintball(const A75Create12& create, const json& recipe)
      : device_(create.device), uploads_(uploadQueue(create.device)), images_(device_, uploads_.Get()),
        canvasEffect_(assetPath(create.pack_dir, recipe.at("canvasEffect"))),
        canvasImage_(optionalAsset(create.pack_dir, recipe, "canvasImage")),
        engine_(device_, uploads_.Get(), 2048, &images_,
                optionalAsset(create.pack_dir, recipe, "bakedTextureDirectory")),
        scale_(recipe.value("scale", 1.f)) {
    engine_.resize(create.width, create.height);
    for (const auto& name : recipe.at("effects"))
      engine_.load(assetPath(create.pack_dir, name), true);
    pigments_ = recipe.at("effects").size();
    if (!pigments_ || !std::isfinite(scale_) || scale_ <= 0)
      throw std::runtime_error("Invalid Paintball pigment recipe");
    images_.finish();
  }

  void configure(const Appearance& appearance, double now, Backdrop& backdrop) {
    engine_.configure(appearance, now);
    foregroundOpacity_ = appearance.foregroundOpacity;
    if (foregroundOpacity_ == 0 || foregroundOpacity_ == 1)
      opacityPass_.reset();
    const bool visibleCanvas = !appearance.solidBackground && appearance.backgroundBrightness > 0 &&
                               appearance.backgroundColor != Rgb{0, 0, 0};
    const bool sampled = engine_.samplesBackground();
    if (((visibleCanvas && appearance.backgroundOpacity > 0) || sampled) && !canvas_) {
      canvas_ = canvasImage_.empty() ? bakeCanvas(device_, engine_.directQueue(), canvasEffect_)
                                    : images_.loadDds(canvasImage_);
      images_.finish();
      backdrop.setCanvas(device_, canvas_);
      if (sampled)
        engine_.setBackground(canvas_.Get(), canvasImage_.empty() ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                 : images_.sampleState());
    } else if (!visibleCanvas && !sampled && canvas_) {
      backdrop.setCanvas(device_, {});
      canvas_.Reset();
    }
  }
  void clear() { engine_.clear(); }

  void key(const A75Event& event) {
    if (!std::isfinite(event.seconds) || !std::isfinite(event.x) || !std::isfinite(event.y) ||
        event.x < 0 || event.x > 1 || event.y < 0 || event.y > 1)
      return;
    if (event.width && event.height)
      engine_.resize(event.width, event.height);
    if (engine_.catchUp(event.seconds) || !engine_.active())
      lastFrame_ = event.seconds;
    randomState_ ^= randomState_ >> 12;
    randomState_ ^= randomState_ << 25;
    randomState_ ^= randomState_ >> 27;
    const size_t pigment = (randomState_ * 2685821657736338717ull) % pigments_;
    engine_.play(pigment, event.x, event.y, scale_);
  }

  int draw(const A75Frame12& frame, bool paused) {
    const bool drawForeground = foregroundOpacity_ > 0;
    if (!paused && engine_.active()) {
      engine_.updateFrame(frame.seconds, frame.seconds - lastFrame_, drawForeground);
      if (foregroundOpacity_ == 1) {
        engine_.draw(frame);
      } else if (drawForeground && engine_.active()) {
        if (!opacityPass_ || !opacityPass_->matches(frame))
          opacityPass_.emplace(device_, frame.width, frame.height);
        engine_.draw(opacityPass_->begin(frame));
        opacityPass_->blend(frame, foregroundOpacity_);
      }
    }
    lastFrame_ = frame.seconds;
    return !paused && drawForeground && engine_.visible() ? 0 : -1;
  }

private:
  static fs::path optionalAsset(const fs::path& pack, const json& recipe, const char* name) {
    const auto item = recipe.find(name);
    return item == recipe.end() ? fs::path{} : assetPath(pack, *item);
  }

  ID3D12Device* device_;
  ComPtr<ID3D12CommandQueue> uploads_;
  PaintballImageUploads images_;
  fs::path canvasEffect_, canvasImage_;
  ComPtr<ID3D12Resource> canvas_;
  Engine engine_;
  std::optional<PaintballOpacityPass> opacityPass_;
  float foregroundOpacity_ = 1;
  const float scale_;
  size_t pigments_ = 0;
  double lastFrame_ = 0;
  uint64_t randomState_ = (GetTickCount64() << 16) ^ GetCurrentProcessId() ^ 0x9e3779b97f4a7c15ull;
};
} // namespace atlas_packs

extern "C" const A75Wallpaper12* __cdecl Action75GetWallpaper12(uint32_t abi) {
  return atlas_packs::api12<atlas_packs::Paintball>(abi);
}
