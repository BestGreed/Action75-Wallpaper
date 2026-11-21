#pragma once
#include "Action75Wallpaper12.h"
#include "NativePluginWire.h"
#include <dxgi1_4.h>

namespace action75::worker {
class WorkerTarget12 {
public:
  HRESULT initialize(IDXGIFactory4 *factory, IDXGIAdapter *adapter, HANDLE targetHandle,
                     HANDLE syncHandle, unsigned width, unsigned height, bool composition);
  HRESULT render(Mailbox &msg, const A75Wallpaper12 &api, void *state);
  HRESULT finish();
  ID3D12Device *device() const { return device_.Get(); }
  HANDLE removalEvent() const { return removed_.value; }
  bool deviceLost() const { return device_ && FAILED(device_->GetDeviceRemovedReason()); }

private:
  Handle completed_, removed_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> chain_;
  ComPtr<ID3D12Resource> buffers_[2];
  ComPtr<ID3D12Resource> target_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12DescriptorHeap> rtvHeap_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> commands_;
  UINT rtvStride_ = 0;
  unsigned width_ = 0, height_ = 0;
  uint64_t submitted_ = 0;
  uint64_t completedSubmission_ = 0;
};
} // namespace action75::worker
