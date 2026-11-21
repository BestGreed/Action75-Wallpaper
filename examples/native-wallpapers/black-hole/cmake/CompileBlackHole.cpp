#include "BlackHoleSimulation.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <wrl/client.h>

int wmain(int argc,wchar_t** argv) {
  if (argc!=4) return 2;
  const std::wstring api = argv[3];
  if (api != L"dx11" && api != L"dx12") return 2;
  const bool dx12 = api == L"dx12";
  const auto count=std::to_string(action75::BlackHoleSimulation::MaxStars);
  const D3D_SHADER_MACRO macros[]={{"BLACK_HOLE_MAX_STARS",count.c_str()},{nullptr,nullptr}};
  const char* stages[] = {"ScreenVs", "StarVs", "ScenePs", "StarPs", "LightSourcePs",
    "BloomHorizontalPs", "BloomVerticalPs", "CompositePs", "BakeArtworkPs", "BakeStarsPs", "SkyMipPs",
    "TranslucentScenePs", "TranslucentStarPs", "TranslucentCompositePs"};
  const size_t stageCount = dx12 ? std::size(stages) : 10;
  std::ifstream in(std::filesystem::path(argv[1]), std::ios::binary);
  if (!in) return 1;
  std::string source{std::istreambuf_iterator<char>(in),{}};
  // Compile everything before publishing the generated header.
  Microsoft::WRL::ComPtr<ID3DBlob> codes[std::size(stages)];
  for (size_t i=0;i<stageCount;++i) {
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostic;
    const char* profile = i < 2 ? "vs_5_0" : "ps_5_0";
    if (FAILED(D3DCompile(source.data(),source.size(),"BlackHole.hlsl",macros,nullptr,
        stages[i],profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&codes[i],&diagnostic))) {
      if (diagnostic) std::cerr<<static_cast<const char*>(diagnostic->GetBufferPointer());
      return 1;
    }
  }
  std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);
  out<<"#pragma once\nnamespace "<<(dx12 ? "black_hole_dx12" : "black_hole_dx11")<<" {\n";
  for (size_t i=0;i<stageCount;++i) {
    auto bytes=static_cast<const unsigned char*>(codes[i]->GetBufferPointer());
    out<<"constexpr unsigned char "<<stages[i]<<"[]={\n";
    for (size_t j=0;j<codes[i]->GetBufferSize();++j) {
      out<<unsigned(bytes[j])<<',';
      if(j%24==23) out<<'\n';
    }
    out<<"\n};\n";
  }
  out<<"}\n";
  return out ? 0 : 1;
}
