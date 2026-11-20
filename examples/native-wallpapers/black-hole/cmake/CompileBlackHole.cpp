#include "BlackHoleScene.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <wrl/client.h>

int wmain(int argc,wchar_t** argv) {
  if (argc!=3) return 2;
  const auto count=std::to_string(action75::BlackHoleScene::MaxStars);
  const D3D_SHADER_MACRO macros[]={{"BLACK_HOLE_MAX_STARS",count.c_str()},{nullptr,nullptr}};
  const struct Stage {const char* name; const char* profile;} stages[]={
    {"ScreenVs","vs_5_0"},{"StarVs","vs_5_0"},{"ScenePs","ps_5_0"},{"StarPs","ps_5_0"},
    {"LightSourcePs","ps_5_0"},{"BloomHorizontalPs","ps_5_0"},{"BloomVerticalPs","ps_5_0"},
    {"CompositePs","ps_5_0"},{"BakeArtworkPs","ps_5_0"},{"BakeStarsPs","ps_5_0"}};
  std::ifstream in(std::filesystem::path(argv[1]), std::ios::binary);
  if (!in) return 1;
  std::string source{std::istreambuf_iterator<char>(in),{}};
  // Compile everything before publishing the generated header.
  Microsoft::WRL::ComPtr<ID3DBlob> codes[std::size(stages)];
  for (size_t i=0;i<std::size(stages);++i) {
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostic;
    if (FAILED(D3DCompile(source.data(),source.size(),"BlackHole.hlsl",macros,nullptr,
        stages[i].name,stages[i].profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&codes[i],&diagnostic))) {
      if (diagnostic) std::cerr<<static_cast<const char*>(diagnostic->GetBufferPointer());
      return 1;
    }
  }
  std::ofstream out(std::filesystem::path(argv[2]),std::ios::binary);
  out<<"#pragma once\nnamespace black_hole_dx11 {\n";
  for (size_t i=0;i<std::size(stages);++i) {
    auto bytes=static_cast<const unsigned char*>(codes[i]->GetBufferPointer());
    out<<"constexpr unsigned char "<<stages[i].name<<"[]={\n";
    for (size_t j=0;j<codes[i]->GetBufferSize();++j) {
      out<<unsigned(bytes[j])<<',';
      if(j%24==23) out<<'\n';
    }
    out<<"\n};\n";
  }
  out<<"}\n";
  return out ? 0 : 1;
}
