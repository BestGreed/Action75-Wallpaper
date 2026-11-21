#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t**argv) {
  if(argc!=3) return 2;
  struct Shader {const char*entry,*profile,*symbol;const wchar_t*file;};
  const Shader list[]={{"sparkVS","vs_5_0","FireworksSparkVS",L"spark-vs.cso"},
    {"screenVS","vs_5_0","FireworksScreenVS",L"screen-vs.cso"},
    {"sparkPS","ps_5_0","FireworksSparkPS",L"spark-ps.cso"},
    {"blurPS","ps_5_0","FireworksBlurPS",L"blur-ps.cso"},
    {"compositePS","ps_5_0","FireworksCompositePS",L"composite-ps.cso"},
    {"idlePS","ps_5_0","FireworksIdlePS",L"idle-ps.cso"}};
  const std::filesystem::path dir(argv[2]);std::filesystem::create_directories(dir);
  std::ofstream header(dir/L"FireworksShaders.h",std::ios::binary|std::ios::trunc);
  header<<"#pragma once\n";
  for(const auto&s:list) {
    ComPtr<ID3DBlob> code,error;
    const auto hr=D3DCompileFromFile(argv[1],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,s.entry,s.profile,
      D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());
    if(FAILED(hr))return 1;
    std::ofstream out(dir/s.file,std::ios::binary|std::ios::trunc);
    out.write(static_cast<const char*>(code->GetBufferPointer()),code->GetBufferSize());
    if(!out)return 1;
    header<<"constexpr unsigned char "<<s.symbol<<"[]={\n";
    const auto*p=static_cast<const unsigned char*>(code->GetBufferPointer());
    for(size_t i=0;i<code->GetBufferSize();++i){header<<unsigned(p[i])<<',';if(i%20==19)header<<'\n';}
    header<<"\n};\n";
  }
  return header?0:1;
}
