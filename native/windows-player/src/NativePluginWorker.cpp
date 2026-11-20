#include "NativePluginWire.h"
#include "Action75Wallpaper.h"
#include <shellapi.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

using namespace action75::worker;
namespace fs = std::filesystem;

namespace {
struct ComRuntime {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComRuntime() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};
HANDLE argumentHandle(const wchar_t *text) {
  return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(text, nullptr, 10)));
}
bool isConfined() {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
    return false;
  DWORD value = 0, bytes = 0;
  return GetTokenInformation(token.value, TokenIsAppContainer, &value, sizeof(value), &bytes) &&
         value;
}
struct Instance {
  HMODULE library = nullptr;
  const A75Wallpaper *api = nullptr;
  void *state = nullptr;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11RenderTargetView> view;
  ComPtr<IDXGIKeyedMutex> mutex;
  unsigned width = 0, height = 0;
  Mailbox *eventMailbox = nullptr;
  static void __cdecl commit(void *host, const char *patch) {
    auto msg = static_cast<Instance *>(host)->eventMailbox;
    if (!msg || !patch || strnlen_s(patch, sizeof(msg->patch)) >= sizeof(msg->patch))
      return;
    try {
      auto values = nlohmann::json::parse(patch);
      if (!values.is_object())
        return;
      auto prior = msg->patch[0] ? nlohmann::json::parse(msg->patch) : nlohmann::json::object();
      prior.update(values);
      auto merged = prior.dump();
      if (merged.size() < sizeof(msg->patch))
        memcpy(msg->patch, merged.c_str(), merged.size() + 1);
    } catch (...) {
    }
  }
  HRESULT start(const fs::path &dll, const fs::path &pack, HANDLE texture, Mailbox &msg) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
      return hr;
    ComPtr<IDXGIAdapter> adapter;
    hr = factory->EnumAdapterByLuid(msg.adapter, IID_PPV_ARGS(&adapter));
    if (FAILED(hr))
      return hr;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device,
                           nullptr, &context);
    if (FAILED(hr))
      return hr;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11Texture2D> surface;
    if (FAILED(hr = device.As(&device1)) ||
        FAILED(hr = device1->OpenSharedResource1(texture, IID_PPV_ARGS(&surface))) ||
        FAILED(hr = surface.As(&mutex)) ||
        FAILED(hr = device->CreateRenderTargetView(surface.Get(), nullptr, &view)))
      return hr;
    width = msg.width;
    height = msg.height;
    library = LoadLibraryExW(dll.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
      return HRESULT_FROM_WIN32(GetLastError());
    auto get = reinterpret_cast<A75GetWallpaper>(GetProcAddress(library, "Action75GetWallpaper"));
    if (!get)
      return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    api = get(ACTION75_WALLPAPER_ABI);
    if (!api || api->size < offsetof(A75Wallpaper, hit_test) ||
        api->abi != ACTION75_WALLPAPER_ABI || !api->create || !api->on_event || !api->render ||
        !api->destroy)
      return E_NOINTERFACE;
    // A negative initialization result in this field opts out of hit-test IPC.
    msg.hit = api->size >= sizeof(A75Wallpaper) && api->hit_test ? 1 : -1;
    A75Create create{sizeof(create), width,        height, device.Get(), context.Get(),
                     pack.c_str(),   msg.settings, this,   commit};
    state = api->create(&create);
    return state ? S_OK : E_FAIL;
  }
  HRESULT render(Mailbox &msg) {
    HRESULT hr = mutex->AcquireSync(0, 200);
    if (hr != S_OK)
      return E_FAIL;
    A75Frame frame{sizeof(frame), width, height, msg.seconds, view.Get()};
    msg.delay = api->render(state, &frame);
    context->Flush();
    hr = mutex->ReleaseSync(1);
    return msg.delay < -1 ? E_FAIL : hr;
  }
  ~Instance() {
    if (state)
      api->destroy(state);
    if (library)
      FreeLibrary(library);
  }
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (!isConfined() || !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
    return 2;
  int count = 0;
  auto args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!args || count != 7)
    return 3;
  Handle mapping(argumentHandle(args[1])), request(argumentHandle(args[2])),
      response(argumentHandle(args[3]));
  Handle texture(argumentHandle(args[4]));
  const fs::path pack(args[5]), module(args[6]);
  LocalFree(args);
  auto msg = static_cast<Mailbox *>(
      MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Mailbox)));
  if (!msg)
    return 4;
  ComRuntime com;
  if (FAILED(com.result))
    return 7;
  Instance plugin;
  while (WaitForSingleObject(request.value, INFINITE) == WAIT_OBJECT_0) {
    HRESULT hr = E_INVALIDARG;
    switch (msg->operation) {
    case Operation::Initialize:
      hr = plugin.start(module, pack, texture.value, *msg);
      break;
    case Operation::Event: {
      A75Event event{sizeof(event), msg->type, msg->width, msg->height,  msg->seconds,
                     msg->x,        msg->y,    msg->code,  msg->settings};
      msg->patch[0] = 0;
      plugin.eventMailbox = msg;
      plugin.api->on_event(plugin.state, &event);
      plugin.eventMailbox = nullptr;
      hr = S_OK;
      break;
    }
    case Operation::Render:
      hr = plugin.render(*msg);
      break;
    case Operation::HitTest:
      msg->hit = plugin.api->size >= sizeof(A75Wallpaper) && plugin.api->hit_test &&
                 plugin.api->hit_test(plugin.state, msg->x, msg->y, plugin.width, plugin.height);
      hr = S_OK;
      break;
    case Operation::Exit:
      return 0;
    }
    msg->status = hr;
    if (FAILED(hr))
      std::snprintf(msg->diagnostic, sizeof(msg->diagnostic),
                    "Wallpaper worker operation %u failed (0x%08lX)",
                    static_cast<unsigned>(msg->operation), static_cast<unsigned long>(hr));
    SetEvent(response.value);
    if (FAILED(hr))
      return 5;
  }
  return 6;
}
