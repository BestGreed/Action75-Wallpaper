#pragma once

#include "Action75Wallpaper12.h"
#include <filesystem>
#include <vector>
#include <wrl/client.h>

namespace atlas_packs {
using Microsoft::WRL::ComPtr;
inline constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

void check(HRESULT result, const char* operation);
D3D12_RESOURCE_DESC textureDescription(unsigned width, unsigned height);
D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type);
void transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
void bindTarget(const A75Frame12& frame);

struct Image {
  unsigned width = 0, height = 0;
  std::vector<uint8_t> pixels;
};

Image decodeImage(const std::filesystem::path& path, unsigned limit);

class Upload {
public:
  Upload() = default;
  Upload(ID3D12Device* device, size_t bytes);
  void write(const void* data, size_t size) const;
  ID3D12Resource* resource() const { return resource_.Get(); }
  D3D12_GPU_VIRTUAL_ADDRESS address() const { return resource_->GetGPUVirtualAddress(); }
  uint8_t* bytes() const { return mapped_; }

private:
  ComPtr<ID3D12Resource> resource_;
  uint8_t* mapped_ = nullptr;
  size_t size_ = 0;
};

class Texture {
public:
  void load(ID3D12Device* device, const Image& image);
  void attach(ID3D12Device* device, ComPtr<ID3D12Resource> source);
  void prepare(ID3D12GraphicsCommandList* commands);
  ID3D12DescriptorHeap* heap() const { return heap_.Get(); }
  ID3D12Resource* resource() const { return resource_.Get(); }

private:
  ComPtr<ID3D12Resource> resource_;
  ComPtr<ID3D12DescriptorHeap> heap_;
  Upload upload_;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout_{};
  bool copied_ = false;
};

enum class Blend { Opaque, Add, Multiply };

class Pipeline {
public:
  Pipeline(ID3D12Device* device, D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
           Blend blend, bool texture, D3D12_INPUT_LAYOUT_DESC input = {});
  void bind(ID3D12GraphicsCommandList* commands, const Upload& constants,
            const Texture* texture = nullptr) const;

private:
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12PipelineState> pipeline_;
};
} // namespace atlas_packs
