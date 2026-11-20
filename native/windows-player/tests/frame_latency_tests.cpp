#include "FrameLatencyGuard.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace action75;
using namespace std::chrono_literals;
static void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int main() {
  try {
    FrameLatencyGuard guard;
    FramePacer::Time now{};
    require(guard.waitMilliseconds(now) == 50, "initial latency deadline");
    for (int elapsed = 1; elapsed < 50; ++elapsed)
      require(guard.waitMilliseconds(now + elapsed * 1ms) == 50 - elapsed,
              "input/IPC wakes extended pending frame deadline");
    require(guard.waitMilliseconds(now + 50ms) == 0 && guard.bypassed(),
            "missing DXGI signal still starves due frame");
    require(guard.timeouts() == 1, "missing timeout accounting");
    for (int elapsed = 51; elapsed < 200; ++elapsed)
      require(guard.waitMilliseconds(now + elapsed * 1ms) == 0,
              "stalled latency gate added another delay per frame");
    require(guard.timeouts() == 1, "fallback counted a timeout each frame");
    guard.ready();
    require(!guard.bypassed() && guard.waitMilliseconds(now + 1s) == 50,
            "healthy DXGI signal did not recover normal gate");
    guard.cancelPending();
    require(guard.waitMilliseconds(now + 20s) == 50,
            "static/hidden period retained an expired pending deadline");

    // Missing DXGI signals and recurring messages still honor the selected
    // render cap after the single bounded recovery interval.
    for (unsigned fps : {30u, 60u, 120u}) {
      FrameLatencyGuard gate;
      FramePacer pacer;
      FramePacer::Time t{};
      unsigned frames = 0;
      bool retryPending = true;
      for (; t.time_since_epoch() < 1050ms; t += 1ms) {
        const auto plan = pacer.plan(t, true, false, retryPending);
        if (!plan.due || gate.waitMilliseconds(t)) continue;
        ++frames;
        pacer.rendered(t, fps, retryPending);
      }
      require(frames > fps * 9 / 10 && frames <= fps + 1,
              "stalled latency gate starved or exceeded capped rendering");
      // A successful static repaint clears retry motion and stops all wakes.
      retryPending = false;
      pacer.rendered(t, fps, retryPending);
      require(!pacer.plan(t + 5s, true, false, retryPending).due &&
              !pacer.plan(t + 5s, true, false, retryPending).timed,
              "successful static repaint retained periodic rendering");
    }
    std::cout << "frame latency recovery: 8 scenarios passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
