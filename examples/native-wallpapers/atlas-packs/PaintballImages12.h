#pragma once

#include "Graphics.h"

#include <Effekseer.h>
#include <EffekseerRendererDX12.h>

namespace atlas_packs {

class PaintballImageUploads {
public:
  PaintballImageUploads(ID3D12Device* device, ID3D12CommandQueue* queue);
  ComPtr<ID3D12Resource> loadDds(const std::filesystem::path& file);
  void finish();
  D3D12_RESOURCE_STATES sampleState() const {
    return type_ == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_RESOURCE_STATE_COMMON
                                               : D3D12_RESOURCE_STATE_GENERIC_READ;
  }

private:
  void submit();
  ID3D12Device* device_;
  ID3D12CommandQueue* queue_;
  const D3D12_COMMAND_LIST_TYPE type_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> commands_;
  ComPtr<ID3D12Fence> fence_;
  Upload staging_;
  uint64_t fenceValue_ = 0;
  unsigned used_ = 0;
  std::vector<ComPtr<ID3D12Resource>> destinations_;
};

Effekseer::Backend::TextureRef paintballTexture(const Effekseer::Backend::GraphicsDeviceRef& graphics,
                                               ID3D12Resource* image, D3D12_RESOURCE_STATES state);

Effekseer::TextureLoaderRef paintballTextureLoader(Effekseer::TextureLoaderRef original,
    Effekseer::Backend::GraphicsDeviceRef graphics, PaintballImageUploads& uploads,
    std::filesystem::path cacheDirectory);

} // namespace atlas_packs
