#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <wrl/client.h>

int wmain(int argc, wchar_t** argv) {
  if (argc != 4)
    return 2;
  std::ofstream output{std::filesystem::path(argv[2])};
  if (!output)
    return 2;
  const std::string prefix = std::filesystem::path(argv[3]).string();
  output << "#pragma once\n";
  for (const bool vertex : {true, false}) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT result = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        vertex ? "vs" : "ps", vertex ? "vs_5_0" : "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &errors);
    if (errors && errors->GetBufferSize())
      std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize() - 1);
    if (FAILED(result))
      return 1;
    output << "inline constexpr unsigned char k" << prefix << (vertex ? "VS" : "PS") << "[] = {";
    const auto* data = static_cast<const unsigned char*>(code->GetBufferPointer());
    for (size_t i = 0; i < code->GetBufferSize(); ++i) {
      if (i % 24 == 0)
        output << "\n  ";
      output << static_cast<unsigned>(data[i]) << ',';
    }
    output << "\n};\n";
  }
  return !output;
}
