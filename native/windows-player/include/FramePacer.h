#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace action75 {
// No wall clock, timer allocation or side effects: tested independently.
// A stopped/static scene has no deadline, and never requests a periodic wake.
class FramePacer {
public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  using Duration = Clock::duration;
  struct Plan {
    bool due = false;
    bool timed = false;
    Duration remaining{};
  };
  static Duration period(unsigned fps) noexcept {
    const auto safe = std::clamp(fps, 1u, 120u);
    return std::chrono::duration_cast<Duration>(std::chrono::nanoseconds((1000000000ll+safe/2)/safe));
  }
  Plan plan(Time now, bool presentable, bool dirty, bool animating) const noexcept {
    if (!presentable) return {};
    if (dirty) return {true, false, {}};
    if (!animating) return {};
    if (!armed_ || now >= deadline_) return {true, false, {}};
    return {false, true, deadline_-now};
  }
  void rendered(Time now, unsigned fps, bool remainsActive, bool resetPhase=false) noexcept {
    lastRendered_=now;
    const auto p=period(fps);
    if (!remainsActive) {
      armed_=false;
      return;
    }
    // Preserve phase during normal scheduler jitter. After a miss of half a
    // period, rebase instead of following a long frame with a short burst.
    if (!resetPhase && armed_ && period_==p && now>=deadline_ && now-deadline_<=p/2)
      deadline_+=p;
    else
      deadline_=now+p;
    period_=p;
    armed_=true;
  }
  // Rate changes are relative to the last submitted frame. A key can wake the
  // idle rate immediately without inserting a frame inside the active budget.
  void setRate(unsigned fps) noexcept {
    const auto next=period(fps);
    if(armed_ && next!=period_){period_=next;deadline_=lastRendered_+next;}
  }
  static std::uint32_t ceilingMilliseconds(Duration d) noexcept {
    if (d<=Duration::zero()) return 1;
    const auto n=std::chrono::ceil<std::chrono::milliseconds>(d).count();
    return static_cast<std::uint32_t>(std::min<std::int64_t>(n,0xfffffffeull));
  }
  static std::int64_t relativeHundredNanoseconds(Duration d) noexcept {
    using Ticks=std::chrono::duration<std::int64_t,std::ratio<1,10000000>>;
    return -std::max<std::int64_t>(1,std::chrono::ceil<Ticks>(d).count());
  }
  void reset() noexcept { armed_=false; }
private:
  bool armed_=false;
  Time deadline_{};
  Time lastRendered_{};
  Duration period_{};
};
}
