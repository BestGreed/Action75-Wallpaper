#pragma once
#include "FramePacer.h"
#include <chrono>
#include <cstdint>

namespace action75 {
// A DXGI latency event is advisory while another application changes display
// mode. Its deadline belongs to the pending frame, not to each message wake.
// Once stalled, poll it only at normal frame deadlines until it signals again.
class FrameLatencyGuard {
public:
  using Time = FramePacer::Time;
  std::uint32_t waitMilliseconds(Time now) noexcept {
    if (bypassed_) return 0;
    if (!waiting_) {
      deadline_ = now + std::chrono::milliseconds(50);
      waiting_ = true;
    }
    if (now >= deadline_) {
      waiting_ = false;
      bypassed_ = true;
      ++timeouts_;
      return 0;
    }
    return FramePacer::ceilingMilliseconds(deadline_ - now);
  }
  void ready() noexcept { waiting_ = false; bypassed_ = false; }
  void cancelPending() noexcept { waiting_ = false; }
  bool bypassed() const noexcept { return bypassed_; }
  std::uint64_t timeouts() const noexcept { return timeouts_; }
private:
  bool waiting_ = false, bypassed_ = false;
  Time deadline_{};
  std::uint64_t timeouts_ = 0;
};
}
