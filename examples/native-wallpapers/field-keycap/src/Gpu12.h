#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace wallpaper {
using Microsoft::WRL::ComPtr;

constexpr auto OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr unsigned TextureSlots = 6;

void check(HRESULT result, const char *operation);

template <std::size_t N>
D3D12_SHADER_BYTECODE shader(const BYTE (&code)[N]) {
  return {code, N};
}

ComPtr<ID3D12Resource> texture(ID3D12Device *, unsigned width, unsigned height, DXGI_FORMAT,
                              D3D12_RESOURCE_FLAGS, D3D12_RESOURCE_STATES,
                              const D3D12_CLEAR_VALUE *clear = nullptr);
void transition(ID3D12GraphicsCommandList *, ID3D12Resource *, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after);
void setTarget(ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, unsigned width,
               unsigned height);

class Graphics12 {
public:
  Graphics12(ID3D12Device *, std::size_t constantSize, bool useTextures);
  Graphics12(const Graphics12 &) = delete;
  Graphics12 &operator=(const Graphics12 &) = delete;
  ~Graphics12();

  ComPtr<ID3D12PipelineState> pipeline(D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
                                     DXGI_FORMAT format, bool weighted = false) const;
  void bind(ID3D12GraphicsCommandList *, const void *constants) const;

private:
  ID3D12Device *device_;
  std::size_t constantSize_;
  ComPtr<ID3D12RootSignature> signature_;
  ComPtr<ID3D12Resource> constants_;
  void *mapped_ = nullptr;
};

// The ABI guarantees that the GPU has finished the previous callback's commands.
// Copies use the borrowed list and keep their source allocations alive until then.
class TextureUpload {
public:
  TextureUpload(ID3D12Device *, unsigned width, unsigned height, DXGI_FORMAT);
  ID3D12Resource *get() const { return texture_.Get(); }
  void stage(const void *pixels, std::size_t stride);
  void record(ID3D12GraphicsCommandList *);

private:
  ComPtr<ID3D12Resource> texture_, upload_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
  UINT rows_ = 0;
  UINT64 rowBytes_ = 0;
  D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COPY_DEST;
};
} // namespace wallpaper
