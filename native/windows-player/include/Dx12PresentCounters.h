#pragma once

#include <cstdint>
#include <dxgi.h>

namespace action75 {
struct Dx12PresentCounters {
  std::uint64_t drawCalls = 0;
  std::uint64_t presentResults = 0, submitted = 0, busy = 0, occluded = 0, otherResults = 0;
  std::uint64_t retries = 0, retriesWithDraw = 0, idleRetriesWithDraw = 0;

  void recordPresent(HRESULT result, bool withDraw, bool idleDraw) noexcept {
    ++presentResults;
    if (pending_) {
      ++retries;
      retriesWithDraw += withDraw;
      idleRetriesWithDraw += withDraw && idleDraw;
    }
    pending_ = result == DXGI_ERROR_WAS_STILL_DRAWING;
    if (pending_)
      ++busy;
    else if (result == S_OK)
      ++submitted;
    else if (result == DXGI_STATUS_OCCLUDED)
      ++occluded;
    else
      ++otherResults;
  }

  void discardPending() noexcept { pending_ = false; }

private:
  bool pending_ = false;
};
} // namespace action75
