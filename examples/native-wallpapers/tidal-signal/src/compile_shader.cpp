#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>

int wmain(int argc, wchar_t **argv) {
    if (argc!=4) return 2;
    const bool vertex=wcscmp(argv[2],L"vs")==0;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const HRESULT result=D3DCompileFromFile(argv[1],nullptr,nullptr,vertex?"vs":"ps",
        vertex?"vs_5_0":"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
    if (errors) fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);
    return FAILED(result) || FAILED(D3DWriteBlobToFile(blob.Get(),argv[3],TRUE));
}

