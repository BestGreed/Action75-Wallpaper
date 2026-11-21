#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <cstdint>

namespace atlas_packs {
struct SpriteBurstSample {
  float x, y, frame, gain;
};

class SpriteBurstState {
public:
  static constexpr std::size_t capacity = 24;
  void reset() noexcept { count_ = 0; }
  std::size_t count() const noexcept { return count_; }
  bool push(double now, float life, float x, float y, float gain = 1,
            std::uint32_t paletteIndex = 0) noexcept {
    if (!std::isfinite(now) || !std::isfinite(life) || life <= 0 ||
        !std::isfinite(x) || x < 0 || x > 1 ||
        !std::isfinite(y) || y < 0 || y > 1 || !std::isfinite(gain) || gain <= 0)
      return false;
    discardExpired(now, life);
    if (count_ == capacity) {
      std::move(events_.begin() + 1, events_.end(), events_.begin());
      --count_;
    }
    events_[count_++] = {now, x, y, std::min(gain, 1.0f), paletteIndex};
    return true;
  }
  std::size_t sample(double now, float life, unsigned frames,
                     std::array<SpriteBurstSample, capacity> &out,
                     std::array<std::uint32_t, capacity>* paletteIndices = nullptr) noexcept {
    discardExpired(now, life);
    if (frames < 2) return 0;
    // Restrict the energy of synthetic storms while keeping the first strike clear.
    const float gainBudget = count_ > 8 ? std::sqrt(8.0f / static_cast<float>(count_)) : 1.0f;
    for (std::size_t i = 0; i < count_; ++i) {
      const auto &e = events_[i];
      const double t = std::clamp((now - e.born) / life, 0.0, 1.0);
      out[i] = {e.x, e.y, static_cast<float>(t * (frames - 1)), e.gain * gainBudget};
      if (paletteIndices) (*paletteIndices)[i]=e.paletteIndex;
    }
    return count_;
  }
private:
  void discardExpired(double now, float life) noexcept {
    std::size_t live = 0;
    for (std::size_t i = 0; i < count_; ++i)
      if (now - events_[i].born < life) events_[live++] = events_[i];
    count_ = live;
  }
  struct Event { double born; float x, y, gain; std::uint32_t paletteIndex; };
  std::array<Event, capacity> events_{};
  std::size_t count_ = 0;
};
}
