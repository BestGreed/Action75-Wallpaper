#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <string>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    std::filesystem::create_directories(argv[2]);
    for(const char* entry:{"backgroundVS","backgroundPS","echoVS","echoPS"}) {
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        const std::string name(entry);
        const auto hr=D3DCompileFromFile(argv[1],nullptr,nullptr,entry,name.substr(name.size()-2)=="VS"?"vs_5_0":"ps_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        if(errors) fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);
        if(FAILED(hr)) return 1;
        const auto file=std::filesystem::path(argv[2])/(name+".cso");
        if(FAILED(D3DWriteBlobToFile(blob.Get(),file.c_str(),TRUE))) return 1;
    }
    return 0;
}
