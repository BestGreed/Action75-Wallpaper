#include "NativePluginSession.h"
#include <wrl/client.h>
#include <iostream>
using action75::NativePluginSession;
using Microsoft::WRL::ComPtr;
int wmain(int argc, wchar_t **argv) {
  if (argc < 2)
    return 1;
  // The manager's import trial already puts the host in a two-process job.
  HANDLE trialJob = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION trial{};
  trial.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
  trial.BasicLimitInformation.ActiveProcessLimit = 2;
  if (!trialJob ||
      !SetInformationJobObject(trialJob, JobObjectExtendedLimitInformation, &trial,
                               sizeof(trial)) ||
      !AssignProcessToJobObject(trialJob, GetCurrentProcess()))
    return 15;
  auto driver = (argc == 3) ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                               nullptr, &ctx)))
    return 2;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 32;
  desc.Height = 16;
  desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> out, read;
  ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &out)) ||
      FAILED(device->CreateRenderTargetView(out.Get(), nullptr, &rtv)))
    return 3;
  ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &read)))
    return 4;
  std::filesystem::path dll = std::filesystem::absolute(argv[1]);
  NativePluginSession session;
  std::string error;
  auto fail = [&](int n) {
    std::cerr << n << " " << error << std::endl;
    return n;
  };
  if (!session.start(device.Get(), dll.parent_path(), dll.filename(), 32, 16, "{}", error))
    return fail(5);
  bool hit = false;
  if (!session.hitTest(0.2f, 0, hit, error) || !hit)
    return fail(6);
  A75Event key{};
  key.type = A75_KEY_DOWN;
  key.size = sizeof(key);
  key.code = "KeyA";
  if (!session.event(key, error) || session.takeSettingsPatch() != "{\"seen\":true}")
    return fail(7);
  int32_t delay = 0;
  if (!session.render(out.Get(), 1, delay, error) || delay != 70)
    return fail(8);
  ctx->CopyResource(read.Get(), out.Get());
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(ctx->Map(read.Get(), 0, D3D11_MAP_READ, 0, &map)))
    return fail(9);
  auto rgb = static_cast<unsigned char *>(map.pData);
  bool valid = rgb[0] == 255 && (rgb[1] == 127 || rgb[1] == 128) && rgb[2] == 0 && rgb[3] == 255;
  if (!valid)
    std::cerr << int(rgb[0]) << "," << int(rgb[1]) << "," << int(rgb[2]) << "," << int(rgb[3])
              << std::endl;
  ctx->Unmap(read.Get(), 0);
  if (!valid)
    return fail(10);
  session.stop();
  if (!session.start(device.Get(), dll.parent_path(), dll.filename(), 32, 16, "{\"hang\":true}",
                     error))
    return fail(11);
  if (session.render(out.Get(), 2, delay, error) || session.active() ||
      error.find("timed out") == std::string::npos)
    return fail(12);
  if (!session.start(device.Get(), dll.parent_path(), dll.filename(), 32, 16, "{\"exit\":true}",
                     error))
    return fail(13);
  if (session.render(out.Get(), 3, delay, error) || session.active() ||
      error.find("exited (83)") == std::string::npos)
    return fail(14);

  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() /
                    (L"Action75WorkerNestedTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  if (!fs::create_directory(root))
    return fail(16);
  struct TestDirectory {
    fs::path path;
    ~TestDirectory() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } fixture{fs::path(LR"(\\?\)" + root.native())};
  const auto nested = fs::path(std::wstring(91, L'a')) / std::wstring(91, L'b') /
                      std::wstring(60, L'c') / L"wallpaper.dll";
  const auto module = fixture.path / nested;
  fs::create_directories(module.parent_path());
  fs::copy_file(dll, module);
  if (!session.start(device.Get(), root, nested, 32, 16, "{}", error) ||
      !session.render(out.Get(), 4, delay, error) || delay != 70)
    return fail(17);
  session.stop();
  std::cout
      << "isolated render, events, write denial, hit test, patch, timeout, early exit and long entry passed\n";
  return 0;
}
