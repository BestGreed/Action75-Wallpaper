#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) return 1;
    const std::wstring entry(argv[2]), profile(argv[3]);
    const auto ascii = [](const std::wstring& text) {
        std::string result(text.size(), '\0');
        std::transform(text.begin(), text.end(), result.begin(),
                       [](wchar_t c) { return static_cast<char>(c); });
        return result;
    };
    const auto entryName = ascii(entry);
    const auto target = ascii(profile);
    Microsoft::WRL::ComPtr<ID3DBlob> code, diagnostics;
    const HRESULT result = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryName.c_str(), target.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics);
    if (diagnostics)
        std::fwrite(diagnostics->GetBufferPointer(), 1, diagnostics->GetBufferSize(), stderr);
    if (FAILED(result)) {
        std::fprintf(stderr, "Shader compilation failed (0x%08lx)\n", static_cast<unsigned long>(result));
        return 2;
    }
    std::ofstream output(std::filesystem::path(argv[4]), std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
    output.close();
    return output ? 0 : 3;
}
