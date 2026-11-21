#include "PaintballImages12.h"

#include <DX12/LLGI.TextureDX12.h>
#include <EffekseerRendererLLGI/GraphicsDevice.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace atlas_packs {

PaintballImageUploads::PaintballImageUploads(ID3D12Device* device, ID3D12CommandQueue* queue)
    : device_(device), queue_(queue), type_(queue->GetDesc().Type) {}

ComPtr<ID3D12Resource> PaintballImageUploads::loadDds(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  std::array<uint32_t, 32> header{};
  input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
  const auto width = header[4], height = header[3], levels = header[7];
  unsigned maxLevels = 1;
  for (auto edge = std::max(width, height); edge > 1; edge >>= 1)
    ++maxLevels;
  if (!input || header[0] != 0x20534444 || header[1] != 124 ||
      header[19] != 32 || header[20] != 0x41 || header[21] != 0 || header[22] != 32 ||
      header[23] != 0xff || header[24] != 0xff00 || header[25] != 0xff0000 ||
      header[26] != 0xff000000 || header[28] != 0 || !width || !height ||
      width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      uint64_t(width) * height > 32 * 1024 * 1024 || !levels || levels > maxLevels)
    throw std::runtime_error("Invalid Paintball RGBA8 DDS");

  auto desc = textureDescription(width, height);
  desc.MipLevels = UINT16(levels);
  const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  const auto initialState = type_ == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_RESOURCE_STATE_COMMON
                                                                 : D3D12_RESOURCE_STATE_COPY_DEST;
  check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
      initialState, nullptr, IID_PPV_ARGS(&texture)), "Create immutable Paintball image");

  constexpr unsigned stagingBytes = 1024 * 1024;
  if (!commands_) {
    staging_ = Upload(device_, stagingBytes);
    check(device_->CreateCommandAllocator(type_, IID_PPV_ARGS(&allocator_)),
          "Allocate Paintball image staging commands");
    check(device_->CreateCommandList(0, type_, allocator_.Get(), nullptr,
                                     IID_PPV_ARGS(&commands_)), "Create Paintball image staging list");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
          "Create Paintball image staging fence");
  }
  destinations_.push_back(texture);
  for (unsigned mip = 0; mip < levels; ++mip) {
    const auto mipWidth = std::max(1u, width >> mip), mipHeight = std::max(1u, height >> mip);
    const auto rowBytes = mipWidth * 4;
    const auto pitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    for (unsigned y = 0; y < mipHeight;) {
      auto offset = (used_ + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                    ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
      if (offset + pitch > stagingBytes) {
        submit();
        offset = 0;
      }
      const unsigned rows = std::min(mipHeight - y, (stagingBytes - offset) / pitch);
      auto* row = staging_.bytes() + offset;
      for (unsigned n = 0; n < rows; ++n) {
        input.read(reinterpret_cast<char*>(row), rowBytes);
        row += pitch;
      }
      if (!input)
        throw std::runtime_error("Truncated Paintball DDS pixels");
      D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
      source.pResource = staging_.resource();
      source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint = {offset, {format, mipWidth, rows, 1, pitch}};
      destination.pResource = texture.Get();
      destination.SubresourceIndex = mip;
      commands_->CopyTextureRegion(&destination, 0, y, 0, &source, nullptr);
      used_ = offset + rows * pitch;
      y += rows;
    }
  }
  if (input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Unexpected trailing data in Paintball DDS");
  // The COPY queue promotes from COMMON and decays back to it at submission completion.
  if (type_ == D3D12_COMMAND_LIST_TYPE_DIRECT)
    transition(commands_.Get(), texture.Get(), initialState, sampleState());
  return texture;
}

void PaintballImageUploads::submit() {
  check(commands_->Close(), "Close Paintball image copy");
  ID3D12CommandList* lists[]{commands_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  check(queue_->Signal(fence_.Get(), ++fenceValue_), "Signal Paintball image copies");
  check(fence_->SetEventOnCompletion(fenceValue_, nullptr), "Wait for Paintball image copies");
  check(device_->GetDeviceRemovedReason(), "Complete Paintball image copies");
  check(allocator_->Reset(), "Reuse Paintball image copy allocator");
  check(commands_->Reset(allocator_.Get(), nullptr), "Reuse Paintball image copy list");
  used_ = 0;
}

void PaintballImageUploads::finish() {
  if (!commands_)
    return;
  submit();
  commands_.Reset();
  allocator_.Reset();
  fence_.Reset();
  fenceValue_ = 0;
  staging_ = {};
  destinations_.clear();
}

Effekseer::Backend::TextureRef paintballTexture(const Effekseer::Backend::GraphicsDeviceRef& graphics,
                                               ID3D12Resource* image, D3D12_RESOURCE_STATES state) {
  auto backend = graphics.DownCast<EffekseerRendererLLGI::Backend::GraphicsDevice>();
  auto* dx = static_cast<LLGI::GraphicsDX12*>(backend->GetGraphics());
  auto texture = LLGI::CreateSharedPtr(new LLGI::TextureDX12(dx, true));
  if (!texture->Initialize(image, format, state))
    throw std::runtime_error("Create paint texture view");
  return backend->CreateTexture(texture.get());
}

namespace {
class CachedImages final : public Effekseer::TextureLoader {
public:
  CachedImages(Effekseer::TextureLoaderRef original, Effekseer::Backend::GraphicsDeviceRef graphics,
               PaintballImageUploads& uploads, std::filesystem::path cacheDirectory)
      : original_(std::move(original)), graphics_(std::move(graphics)), uploads_(uploads),
        cacheDirectory_(std::move(cacheDirectory)) {}

  Effekseer::TextureRef Load(const char16_t* path, Effekseer::TextureType type) override {
    auto file = cacheDirectory_ / std::filesystem::path(path).filename();
    if (file.extension() != L".png")
      return original_->Load(path, type);
    file.replace_extension(L".dds");
    if (!std::filesystem::is_regular_file(file))
      return original_->Load(path, type);
    auto image = uploads_.loadDds(file);
    auto texture = Effekseer::MakeRefPtr<Effekseer::Texture>();
    texture->SetBackend(paintballTexture(graphics_, image.Get(), uploads_.sampleState()));
    return texture;
  }

  Effekseer::TextureRef Load(const void* data, int32_t size, Effekseer::TextureType type, bool mips) override {
    return original_->Load(data, size, type, mips);
  }

  void Unload(Effekseer::TextureRef texture) override { original_->Unload(std::move(texture)); }

private:
  Effekseer::TextureLoaderRef original_;
  Effekseer::Backend::GraphicsDeviceRef graphics_;
  PaintballImageUploads& uploads_;
  std::filesystem::path cacheDirectory_;
};
} // namespace

Effekseer::TextureLoaderRef paintballTextureLoader(Effekseer::TextureLoaderRef original,
    Effekseer::Backend::GraphicsDeviceRef graphics, PaintballImageUploads& uploads,
    std::filesystem::path cacheDirectory) {
  return Effekseer::MakeRefPtr<CachedImages>(std::move(original), std::move(graphics), uploads,
                                            std::move(cacheDirectory));
}
} // namespace atlas_packs
