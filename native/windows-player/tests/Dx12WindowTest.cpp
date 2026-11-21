#include "Dx12Output.h"
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;
using action75::Dx12Output;

static bool frame(Dx12Output& output, std::string& error) {
  ID3D12Device* device = output.device();
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex.Width = output.width;
  tex.Height = output.height;
  tex.DepthOrArraySize = tex.MipLevels = tex.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> source;
  ComPtr<ID3D12Fence> fence;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source))) ||
      FAILED(device->CreateFence(1, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    error = "Create test source or fence";
    return false;
  }
  if (!output.copyFrame(source.Get(), fence.Get(), 1, error))
    return false;
  const HRESULT hr = output.present();
  std::printf("%ux%u Present HRESULT 0x%08lx\n", output.width, output.height, static_cast<unsigned long>(hr));
  if (hr != S_OK && hr != DXGI_STATUS_OCCLUDED) {
    error = "Unable to present on private desktop";
    return false;
  }
  HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const bool returned = completed && SUCCEEDED(fence->SetEventOnCompletion(2, completed)) &&
                        WaitForSingleObject(completed, 2000) == WAIT_OBJECT_0;
  if (completed) CloseHandle(completed);
  if (!returned) error = "Output did not return ownership of the source texture";
  return returned;
}

int main(int argc, char**) {
  // The test never activates this desktop or routes the user's input to it.
  const auto name = L"Action75Dx12WindowTest-" + std::to_wstring(GetCurrentProcessId());
  HDESK original = GetThreadDesktop(GetCurrentThreadId());
  HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) return 1;
  HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Action75 DX12 output test",
                                 WS_POPUP | WS_VISIBLE, 0, 0, 640, 360, nullptr, nullptr, nullptr, nullptr);
  bool ok = false;
  std::string error;
  if (window) {
    Dx12Output output;
    ok = output.initialize(window, 640, 360, argc == 1, action75::GpuPreference::Integrated, error)
      && output.frameReady && frame(output, error)
      && output.resize(800, 450, error) && frame(output, error);
  }
  if (window) DestroyWindow(window);
  SetThreadDesktop(original);
  CloseDesktop(desktop);
  if (!ok) std::fprintf(stderr, "DX12 window test: %s\n", error.c_str());
  return ok ? 0 : 1;
}
