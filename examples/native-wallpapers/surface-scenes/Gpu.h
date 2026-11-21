#pragma once

#include <cstddef>
#include <d3d12.h>
#include <wrl/client.h>

namespace surface {

using Microsoft::WRL::ComPtr;

struct View {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

struct Texture {
  ComPtr<ID3D12Resource> resource;
  View srv, uav;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct StructuredBuffer {
  ComPtr<ID3D12Resource> resource, staging;
  View srv;
  bool pending = true;
};

enum class Blend { Opaque, Alpha, Coverage, Add, Pressure };

class Gpu {
public:
  static constexpr DXGI_FORMAT outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  static constexpr unsigned srvSlot(unsigned shaderRegister) { return 2 + shaderRegister; }
  static constexpr unsigned uavSlot(unsigned shaderRegister) { return 6 + shaderRegister; }

  explicit Gpu(ID3D12Device *device, D3D12_FILTER clampFilter);
  ~Gpu();
  Gpu(const Gpu &) = delete;
  Gpu &operator=(const Gpu &) = delete;

  ID3D12Device *device() const { return device_; }
  View nullSrv() const { return nullSrv_; }
  void begin(ID3D12GraphicsCommandList *commands);
  D3D12_GPU_VIRTUAL_ADDRESS constantBytes(const void *data, std::size_t size);
  template<typename T> D3D12_GPU_VIRTUAL_ADDRESS constants(const T &data) {
    return constantBytes(&data, sizeof data);
  }

  ComPtr<ID3D12PipelineState> graphics(D3D12_SHADER_BYTECODE vs, D3D12_SHADER_BYTECODE ps,
                                      DXGI_FORMAT format = outputFormat, Blend blend = Blend::Opaque);
  ComPtr<ID3D12PipelineState> compute(D3D12_SHADER_BYTECODE cs);

  void texture(Texture &out, unsigned width, unsigned height, DXGI_FORMAT format,
               D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial, unsigned mips = 1);
  View srv(ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip, unsigned levels);
  View uav(ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip);
  StructuredBuffer structured(const void *data, unsigned count, unsigned stride);
  static void prepare(ID3D12GraphicsCommandList *commands, StructuredBuffer &buffer);

  static void target(ID3D12GraphicsCommandList *commands, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                     unsigned width, unsigned height);
  static void transition(ID3D12GraphicsCommandList *commands, ID3D12Resource *resource,
                         D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
                         unsigned subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
  static void transition(ID3D12GraphicsCommandList *commands, Texture &texture,
                         D3D12_RESOURCE_STATES to);

private:
  View allocateView();
  void writeSrv(View view, ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip, unsigned levels);
  void writeUav(View view, ID3D12Resource *resource, DXGI_FORMAT format, unsigned mip);
  ComPtr<ID3D12Resource> buffer(std::size_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state);

  ID3D12Device *device_;
  ComPtr<ID3D12RootSignature> root_;
  ComPtr<ID3D12DescriptorHeap> visible_, cpu_, rtvs_;
  ComPtr<ID3D12Resource> constants_;
  std::byte *mapped_ = nullptr;
  std::size_t offset_ = 0;
  unsigned viewCount_ = 0, targetCount_ = 0, viewStride_ = 0, targetStride_ = 0;
  View nullSrv_;
};

template<std::size_t N> D3D12_SHADER_BYTECODE shader(const BYTE (&code)[N]) {
  return {code, N};
}

} // namespace surface
