#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>
#include <wrl/client.h>
#include "../src/SpriteBurstShader.h"

int wmain(int argc, wchar_t **argv) {
  const bool glass = argc == 4 && std::wstring_view(argv[3]) == L"glass";
  const bool water = argc == 4 && std::wstring_view(argv[3]) == L"water";
  const bool atlas = argc == 4 && std::wstring_view(argv[3]) == L"atlas";
  const bool background = argc == 4 && std::wstring_view(argv[3]) == L"background";
  if (!background && argc != 3 && !glass && !water && !atlas)
    return 2;
  struct Shader {
    const char *entry, *profile, *symbol;
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
  };
  std::vector<Shader> shaders;
  if (background) {
    shaders = {{"vs", "vs_5_0", "BackgroundVS", {}}, {"ps", "ps_5_0", "BackgroundPS", {}}};
  } else if (atlas) {
    shaders = {{"vs", "vs_5_0", "AtlasVS", {}}, {"ps", "ps_5_0", "AtlasPS", {}}};
  } else if (water) {
    shaders = {
        {"ScreenVertex", "vs_5_0", "WaterScreenVertex", {}},
        {"ImpactVertex", "vs_5_0", "WaterImpactVertex", {}},
        {"WaterPixel", "ps_5_0", "WaterWaterPixel", {}},
        {"ImpactPixel", "ps_5_0", "WaterImpactPixel", {}},
        {"StepWave", "cs_5_0", "WaterStepWave", {}},
        {"BuildSlope", "cs_5_0", "WaterBuildSlope", {}},
        {"BakeOptics", "cs_5_0", "WaterBakeOptics", {}},
    };
  } else if (glass) {
    shaders = {
        {"backdropVS", "vs_5_0", "GlassBackdropVS", {}},
        {"backdropPS", "ps_5_0", "GlassBackdropPS", {}},
        {"crackVS", "vs_5_0", "GlassCrackVS", {}},
        {"crackPS", "ps_5_0", "GlassCrackPS", {}},
        {"paneVS", "vs_5_0", "GlassPaneVS", {}},
        {"panePS", "ps_5_0", "GlassPanePS", {}},
    };
  } else {
    shaders = {
        {"sparkVS", "vs_5_0", "FireworksSparkVS", {}},
        {"screenVS", "vs_5_0", "FireworksScreenVS", {}},
        {"sparkPS", "ps_5_0", "FireworksSparkPS", {}},
        {"blurPS", "ps_5_0", "FireworksBlurPS", {}},
        {"compositePS", "ps_5_0", "FireworksCompositePS", {}},
        {"idlePS", "ps_5_0", "FireworksIdlePS", {}},
    };
  }
  for (auto &shader : shaders) {
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostic;
    const D3D_SHADER_MACRO atlas2D[]={{"SPRITE_ATLAS_2D","1"},{nullptr,nullptr}};
    const auto result = atlas ? D3DCompile(SpriteBurstShader, sizeof(SpriteBurstShader)-1,
        "SpriteBurst", atlas2D, nullptr, shader.entry, shader.profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader.bytecode, &diagnostic)
        : D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, shader.entry, shader.profile,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader.bytecode, &diagnostic);
    if (diagnostic)
      std::cerr << static_cast<const char *>(diagnostic->GetBufferPointer());
    if (FAILED(result)) {
      std::cerr << "Could not compile " << shader.entry << '\n';
      return 1;
    }
  }
  std::ofstream out(std::filesystem::path(argv[2]), std::ios::binary | std::ios::trunc);
  out << "#pragma once\n";
  for (const auto &shader : shaders) {
    out << "constexpr unsigned char " << shader.symbol << "[] = {\n";
    const auto *bytes = static_cast<const unsigned char *>(shader.bytecode->GetBufferPointer());
    for (size_t i = 0; i < shader.bytecode->GetBufferSize(); ++i) {
      out << static_cast<unsigned>(bytes[i]) << ',';
      if ((i + 1) % 20 == 0)
        out << '\n';
    }
    out << "\n};\n";
  }
  return out ? 0 : 1;
}
