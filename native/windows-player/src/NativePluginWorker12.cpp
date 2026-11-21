#include "NativePluginWorker12.h"

namespace action75::worker {
HRESULT WorkerTarget12::initialize(IDXGIFactory4 *factory, IDXGIAdapter *adapter, HANDLE targetHandle,
                                   HANDLE syncHandle, unsigned width, unsigned height, bool composition) {
  HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
  if (FAILED(hr))
    return hr;
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  if (FAILED(hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_))))
    return hr;
  if (composition) {
    ComPtr<IDXGIFactoryMedia> media;
    ComPtr<IDXGISwapChain1> chain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(hr = factory->QueryInterface(IID_PPV_ARGS(&media))) ||
        FAILED(hr = media->CreateSwapChainForCompositionSurfaceHandle(queue_.Get(), targetHandle,
                                                                      &desc, nullptr, &chain)) ||
        FAILED(hr = chain.As(&chain_)) ||
        FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
      return hr;
    removed_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!removed_.value)
      return HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr = fence_->SetEventOnCompletion(UINT64_MAX, removed_.value)))
      return hr;
    for (UINT index = 0; index < 2; ++index)
      if (FAILED(hr = chain_->GetBuffer(index, IID_PPV_ARGS(&buffers_[index]))))
        return hr;
  } else {
    if (FAILED(hr = device_->OpenSharedHandle(targetHandle, IID_PPV_ARGS(&target_))) ||
        FAILED(hr = device_->OpenSharedHandle(syncHandle, IID_PPV_ARGS(&fence_))))
      return hr;
    const auto desc = target_->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != width ||
        desc.Height != height || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
      return E_INVALIDARG;
  }
  width_ = width;
  height_ = height;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, composition ? 2u : 1u};
  if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_))) ||
      FAILED(hr = device_->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator_))) ||
      FAILED(hr = device_->CreateCommandList(0, queueDesc.Type, allocator_.Get(), nullptr,
                                             IID_PPV_ARGS(&commands_))) ||
      FAILED(hr = commands_->Close()))
    return hr;
  rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
  if (composition) {
    for (const auto &buffer : buffers_) {
      device_->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
      rtv.ptr += rtvStride_;
    }
  } else {
    device_->CreateRenderTargetView(target_.Get(), nullptr, rtv);
  }
  completed_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  return completed_.value ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WorkerTarget12::finish() {
  if (!submitted_)
    return S_OK;
  // The composition worker watches device removal separately. Once it has
  // confirmed a submission, more callbacks can use that result until the next submit.
  if (removed_.value && completedSubmission_ == submitted_)
    return S_OK;
  const auto done = fence_->GetCompletedValue();
  if (done == UINT64_MAX)
    return DXGI_ERROR_DEVICE_REMOVED;
  if (done >= submitted_) {
    completedSubmission_ = submitted_;
    return S_OK;
  }
  HRESULT hr = fence_->SetEventOnCompletion(submitted_, completed_.value);
  if (FAILED(hr))
    return hr;
  const auto result = WaitForSingleObject(completed_.value, FrameCompletionTimeoutMs);
  if (result == WAIT_OBJECT_0) {
    hr = device_->GetDeviceRemovedReason();
    if (SUCCEEDED(hr))
      completedSubmission_ = submitted_;
    return hr;
  }
  return result == WAIT_TIMEOUT ? DXGI_ERROR_WAIT_TIMEOUT : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WorkerTarget12::render(Mailbox &msg, const A75Wallpaper12 &api, void *state) {
  if (!chain_ && (msg.waitValue < submitted_ || msg.signalValue <= msg.waitValue || msg.signalValue == UINT64_MAX))
    return E_INVALIDARG;
  HRESULT hr = finish();
  if (FAILED(hr) || FAILED(hr = allocator_->Reset()) ||
      FAILED(hr = commands_->Reset(allocator_.Get(), nullptr)))
    return hr;
  const UINT index = chain_ ? chain_->GetCurrentBackBufferIndex() : 0;
  auto *target = chain_ ? buffers_[index].Get() : target_.Get();
  auto rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += index * rtvStride_;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET};
  commands_->ResourceBarrier(1, &barrier);
  commands_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0, 0, float(width_), float(height_), 0, 1};
  const D3D12_RECT scissor{0, 0, LONG(width_), LONG(height_)};
  commands_->RSSetViewports(1, &viewport);
  commands_->RSSetScissorRects(1, &scissor);
  const A75Frame12 frame{sizeof(frame), width_, height_, msg.seconds, commands_.Get(), target, rtv};
  msg.delay = api.render(state, &frame);
  if (msg.delay < -1)
    return E_FAIL;
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  commands_->ResourceBarrier(1, &barrier);
  if (FAILED(hr = commands_->Close()) || (!chain_ && FAILED(hr = queue_->Wait(fence_.Get(), msg.waitValue))))
    return hr;
  ID3D12CommandList *lists[]{commands_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  const auto signal = chain_ ? submitted_ + 1 : msg.signalValue;
  if (FAILED(hr = queue_->Signal(fence_.Get(), signal)))
    return hr;
  submitted_ = signal;
  if (chain_) {
    if (msg.waitForFrame) {
      msg.presentResult = S_OK;
      return finish();
    }
    msg.presentResult = chain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (FAILED(msg.presentResult) && msg.presentResult != DXGI_ERROR_WAS_STILL_DRAWING)
      return msg.presentResult;
  }
  return S_OK;
}
} // namespace action75::worker
