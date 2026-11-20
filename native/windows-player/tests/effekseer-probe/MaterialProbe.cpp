#include <d3d11.h>
#include <wrl/client.h>
#include <Effekseer.h>
#include <EffekseerRendererDX11.h>
#include <cstdio>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context))) return 3;
    auto graphics=EffekseerRendererDX11::CreateGraphicsDevice(device.Get(),context.Get());
    auto loader=EffekseerRendererDX11::CreateMaterialLoader(graphics);
    int failures=0;
    for (int i=1;i<argc;++i) {
        const auto path=std::filesystem::absolute(argv[i]).u16string();
        auto material=loader->Load(path.c_str());
        if (!material) { std::fwprintf(stderr,L"MATERIAL COMPILE FAILED: %ls\n",argv[i]); ++failures; }
        else std::wprintf(L"MATERIAL COMPILE OK: %ls textures=%d uniforms=%d\n",argv[i],material->TextureCount,material->UniformCount);
    }
    return failures ? 1 : 0;
}
