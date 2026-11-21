#pragma once
#include <array>
#include <cstddef>

namespace surface {

struct VariantPlaybackRules {
  static constexpr unsigned maxVariants = 10;
  unsigned shape = 0, selected = 0, keyInterval = 1;
  std::array<unsigned, maxVariants> order{};
  bool shuffle = false;
};

class VariantPlaybackState {
public:
  explicit VariantPlaybackState(VariantPlaybackRules rules = {}) : rules_(rules) {}

  void configure(const VariantPlaybackRules &rules) noexcept {
    if (rules_.keyInterval != rules.keyInterval)
      keysSinceBurst_ = 0;
    if (rules_.order != rules.order)
      rewind();
    rules_ = rules;
  }

  bool readyForKeyBurst() noexcept {
    if (++keysSinceBurst_ < rules_.keyInterval)
      return false;
    keysSinceBurst_ = 0;
    return true;
  }

  void rewind() noexcept { next_ = 0; }

  template<class RandomIndex> unsigned choose(RandomIndex randomIndex) noexcept {
    if (rules_.shape)
      return rules_.shape <= VariantPlaybackRules::maxVariants ? rules_.shape : 0;
    const auto enabled = [&](unsigned slot) {
      return slot && slot <= VariantPlaybackRules::maxVariants &&
             (rules_.selected & (1u << (slot - 1)));
    };
    if (rules_.shuffle) {
      std::array<unsigned, VariantPlaybackRules::maxVariants> selected{};
      unsigned count = 0;
      for (const auto slot : rules_.order)
        if (enabled(slot))
          selected[count++] = slot;
      return count ? selected[randomIndex(count)] : 0;
    }
    for (std::size_t i = 0; i < rules_.order.size(); ++i) {
      const auto index = (next_ + i) % rules_.order.size();
      const auto slot = rules_.order[index];
      if (enabled(slot)) {
        next_ = (index + 1) % rules_.order.size();
        return slot;
      }
    }
    return 0;
  }

private:
  VariantPlaybackRules rules_;
  unsigned keysSinceBurst_ = 0;
  std::size_t next_ = 0;
};

} // namespace surface
