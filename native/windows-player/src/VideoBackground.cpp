#include "VideoBackground.h"

#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <new>
#include <sstream>
#include <vector>

// This implementation follows the documented Media Engine frame-server API:
// https://learn.microsoft.com/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengine-onvideostreamtick
// https://learn.microsoft.com/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengine-transfervideoframe
// Link mfplat, mfuuid, mf, oleaut32, shlwapi (in addition to D3D11/COM).

using Microsoft::WRL::ComPtr;

namespace action75 {
namespace {

struct PlaybackEvents {
  std::atomic<bool> closed{false};
  std::atomic<bool> metadata{false};
  std::atomic<bool> ready{false};
  std::atomic<bool> failed{false};
};

// No owner pointers or D3D calls are used on Media Foundation worker threads.
// Shared atomic state can safely outlive VideoBackground during shutdown.
class MediaNotify final : public IMFMediaEngineNotify {
public:
  explicit MediaNotify(std::shared_ptr<PlaybackEvents> events)
      : events_(std::move(events)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify)) {
      *result = static_cast<IMFMediaEngineNotify *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR parameter1,
                                        DWORD) override {
    if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
      // Not normally emitted (WAITFORSTABLE_STATE is not requested).
      SetEvent(reinterpret_cast<HANDLE>(parameter1));
      return S_OK;
    }
    if (events_->closed.load(std::memory_order_acquire)) return S_OK;
    if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA ||
        event == MF_MEDIA_ENGINE_EVENT_FORMATCHANGE)
      events_->metadata.store(true, std::memory_order_release);
    if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY ||
        event == MF_MEDIA_ENGINE_EVENT_LOADEDDATA)
      events_->ready.store(true, std::memory_order_release);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
      events_->failed.store(true, std::memory_order_release);
    return S_OK;
  }

private:
  std::atomic<ULONG> references_{1};
  std::shared_ptr<PlaybackEvents> events_;
};

std::string failure(const char *operation, HRESULT result) {
  std::ostringstream output;
  output << operation << " failed (0x" << std::hex
         << static_cast<unsigned long>(result) << ')';
  return output.str();
}

} // namespace

struct VideoBackground::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IMFDXGIDeviceManager> manager;
  ComPtr<IMFMediaEngine> engine;
  ComPtr<IMFMediaEngineNotify> notify;
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  std::shared_ptr<PlaybackEvents> events;
  std::string error;
  unsigned width = 1920, height = 720;
  bool foundationStarted = false;
  bool paused = false, ready = false, failed = false, hasFrame = false;
  std::chrono::steady_clock::time_point loadingStarted{};

  bool check(HRESULT result, const char *operation) {
    if (SUCCEEDED(result)) return true;
    error = failure(operation, result);
    failed = true;
    return false;
  }

  void close() noexcept {
    if (events) events->closed.store(true, std::memory_order_release);
    if (engine) engine->Shutdown();
    engine.Reset();
    notify.Reset();
    manager.Reset();
    events.reset();
    view.Reset();
    texture.Reset();
    ready = false;
    hasFrame = false;
    if (foundationStarted) {
      MFShutdown();
      foundationStarted = false;
    }
  }

  bool target(unsigned requestedWidth, unsigned requestedHeight) {
    if (!requestedWidth || !requestedHeight || requestedWidth > 16384 ||
        requestedHeight > 16384) {
      error = "Invalid video output size";
      return false;
    }
    if (!engine) { width = requestedWidth; height = requestedHeight; return true; }
    if (texture && width == requestedWidth && height == requestedHeight) return true;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = requestedWidth;
    description.Height = requestedHeight;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> candidateTexture;
    ComPtr<ID3D11ShaderResourceView> candidateView;
    if (!check(device->CreateTexture2D(&description, nullptr, &candidateTexture),
               "Create video output texture") ||
        !check(device->CreateShaderResourceView(candidateTexture.Get(), nullptr,
                                               &candidateView),
               "Create video texture view")) return false;
    texture = std::move(candidateTexture);
    view = std::move(candidateView);
    width = requestedWidth;
    height = requestedHeight;
    hasFrame = false;
    return true;
  }
};

VideoBackground::VideoBackground() : impl_(std::make_unique<Impl>()) {}
VideoBackground::~VideoBackground() { Close(); }

bool VideoBackground::Initialize(ID3D11Device *device,
                                 ID3D11DeviceContext *context) {
  Close();
  impl_->error.clear();
  impl_->failed = false;
  impl_->device.Reset();
  impl_->context.Reset();
  if (!device || !context) { impl_->error = "Video requires a D3D11 device and context"; return false; }
  ComPtr<ID3D11Device> contextDevice;
  context->GetDevice(&contextDevice);
  if (contextDevice.Get() != device) { impl_->error = "Video device and context do not match"; return false; }
  impl_->device = device;
  impl_->context = context;
  return true;
}

bool VideoBackground::Open(const std::filesystem::path &file) {
  Close();
  auto &state = *impl_;
  state.error.clear();
  state.failed = false;
  if (!state.device || !state.context) { state.error = "Video has not been initialized"; return false; }
  try {
    if (!std::filesystem::is_regular_file(file)) { state.error = "Video file is missing"; return false; }
    if (!(state.device->GetCreationFlags() & D3D11_CREATE_DEVICE_VIDEO_SUPPORT)) {
      state.error = "D3D11 device was created without video support";
      return false;
    }
    if (!state.check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Start Media Foundation")) return false;
    state.foundationStarted = true;
    // MF can use the same immediate context from its internal decode threads.
    // Leave protection enabled for the shared device: another video instance
    // can remain open when a previous wallpaper's instance is destroyed.
    ComPtr<ID3D10Multithread> multithread;
    if (!state.check(state.context.As(&multithread), "Enable video context protection")) { Close(); return false; }
    multithread->SetMultithreadProtected(TRUE);
    UINT token = 0;
    if (!state.check(MFCreateDXGIDeviceManager(&token, &state.manager), "Create video device manager") ||
        !state.check(state.manager->ResetDevice(state.device.Get(), token), "Bind video D3D11 device")) { Close(); return false; }
    state.events = std::make_shared<PlaybackEvents>();
    state.notify.Attach(new (std::nothrow) MediaNotify(state.events));
    if (!state.notify) { state.error = "Cannot allocate video callback"; Close(); return false; }
    ComPtr<IMFAttributes> attributes;
    if (!state.check(MFCreateAttributes(&attributes, 3), "Create video attributes") ||
        !state.check(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, state.manager.Get()), "Set video device manager") ||
        !state.check(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, state.notify.Get()), "Set video callback") ||
        !state.check(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM), "Set video pixel format")) { Close(); return false; }
    ComPtr<IMFMediaEngineClassFactory> factory;
    if (!state.check(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Create Media Engine factory") ||
        !state.check(factory->CreateInstance(MF_MEDIA_ENGINE_FORCEMUTE | MF_MEDIA_ENGINE_REAL_TIME_MODE,
                                              attributes.Get(), &state.engine), "Create Media Engine")) { Close(); return false; }
    if (!state.check(state.engine->SetMuted(TRUE), "Mute video") ||
        !state.check(state.engine->SetAutoPlay(FALSE), "Configure video autoplay") ||
        !state.check(state.engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC), "Configure video loading") ||
        !state.check(state.engine->SetLoop(TRUE), "Loop video") ||
        !state.target(state.width, state.height)) { Close(); return false; }
    const std::wstring absolute = std::filesystem::absolute(file).wstring();
    DWORD urlSize = 32768;
    std::vector<wchar_t> url(urlSize);
    HRESULT result = UrlCreateFromPathW(absolute.c_str(), url.data(), &urlSize, 0);
    if (result == E_POINTER) {
      url.resize(static_cast<size_t>(urlSize) + 1);
      DWORD capacity = static_cast<DWORD>(url.size());
      result = UrlCreateFromPathW(absolute.c_str(), url.data(), &capacity, 0);
    }
    if (!state.check(result, "Convert video file URL")) { Close(); return false; }
    BSTR source = SysAllocString(url.data());
    if (!source) { state.error = "Cannot allocate video file URL"; Close(); return false; }
    result = state.engine->SetSource(source);
    SysFreeString(source);
    if (!state.check(result, "Open video source") || !state.check(state.engine->Load(), "Load video source")) { Close(); return false; }
    state.loadingStarted = std::chrono::steady_clock::now();
    return true;
  } catch (const std::exception &exception) {
    state.error = exception.what();
    Close();
    return false;
  }
}

bool VideoBackground::Resize(unsigned width, unsigned height) {
  return impl_->target(width, height);
}

bool VideoBackground::UpdateFrame() {
  auto &state = *impl_;
  if (!state.engine || !state.events || state.failed) return false;
  if (state.events->failed.exchange(false, std::memory_order_acq_rel)) {
    ComPtr<IMFMediaError> error;
    HRESULT result = state.engine->GetError(&error);
    if (SUCCEEDED(result) && error) {
      std::ostringstream message;
      message << "Video playback error " << error->GetErrorCode() << " (0x"
              << std::hex << static_cast<unsigned long>(error->GetExtendedErrorCode()) << ')';
      state.error = message.str();
    } else state.error = failure("Read video playback error", result);
    state.failed = true;
    state.engine->Pause();
    return false;
  }
  if (state.events->metadata.exchange(false, std::memory_order_acq_rel) && !state.engine->HasVideo()) {
    state.error = "The selected file does not contain a video stream";
    state.failed = true;
    state.engine->Pause();
    return false;
  }
  if (state.events->ready.exchange(false, std::memory_order_acq_rel)) {
    state.ready = true;
    if (!state.paused && !state.check(state.engine->Play(), "Start video playback")) return false;
  }
  if (!state.ready && std::chrono::steady_clock::now() - state.loadingStarted >
                          std::chrono::seconds(15)) {
    state.error = "Video did not become ready within 15 seconds";
    state.failed = true;
    state.engine->Pause();
    return false;
  }
  if (state.paused || !state.ready || !state.texture) return false;
  LONGLONG presentationTime = 0;
  const HRESULT tick = state.engine->OnVideoStreamTick(&presentationTime);
  if (tick == S_FALSE) return false;
  if (!state.check(tick, "Check video frame")) return false;
  // The previous frame may still be bound as a pixel-shader input. Clear those
  // references before Media Engine writes into the same texture.
  ID3D11ShaderResourceView *empty[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
  state.context->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, empty);
  const MFVideoNormalizedRect source{0.0f, 0.0f, 1.0f, 1.0f};
  const RECT destination{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
  const MFARGB black{0, 0, 0, 255};
  if (!state.check(state.engine->TransferVideoFrame(state.texture.Get(), &source,
                                                   &destination, &black), "Transfer video frame")) return false;
  state.hasFrame = true;
  return true;
}

ID3D11ShaderResourceView *VideoBackground::TextureView() const noexcept {
  return impl_->hasFrame ? impl_->view.Get() : nullptr;
}

bool VideoBackground::IsAnimated() const noexcept {
  return impl_->engine && !impl_->failed && !impl_->paused;
}

void VideoBackground::SetPaused(bool paused) {
  auto &state = *impl_;
  if (state.paused == paused) return;
  state.paused = paused;
  // Preloading continues while paused; autoplay is already disabled. Calling
  // Pause before ready is unnecessary and may be rejected by some sources.
  if (!state.engine || state.failed || !state.ready) return;
  if (paused) state.check(state.engine->Pause(), "Pause video");
  else if (state.ready) state.check(state.engine->Play(), "Resume video");
}

void VideoBackground::Close() noexcept { if (impl_) impl_->close(); }
const std::string &VideoBackground::LastError() const noexcept { return impl_->error; }

} // namespace action75
