void Fireworks12::configure(const PackAppearance &appearance) {
  if (appearance_.lifetime != appearance.fireworks.lifetime ||
      appearance_.trail != appearance.fireworks.trail || lifespan_[0] == 0) {
    for (unsigned shape = 0; shape < shapeCount; ++shape) {
      float duration = 0;
      for (const auto &timing : timings_[shape])
        duration = std::max(duration, timing[0] + timing[1] * appearance.fireworks.lifetime +
                            timing[2] * appearance.fireworks.trail);
      lifespan_[shape] = duration;
    }
  }
  playback_.configure(appearance.fireworks);
  appearance_ = appearance.fireworks;
  brightness_ = appearance.brightness;
  scale_ = appearance.scale;
  speed_ = appearance.speed;
  intensity_ = appearance.intensity;
  data_.background = {linear(appearance.backgroundColor[0]), linear(appearance.backgroundColor[1]),
                      linear(appearance.backgroundColor[2]), appearance.backgroundBrightness};
  data_.opacity = {appearance.backgroundOpacity, appearance.foregroundOpacity, 0, 0};
  data_.skyGrade = LayerColor::make(appearance.backgroundHue, appearance.backgroundSaturation);
  data_.skyGrade.rows[1][3] = static_cast<float>(appearance.backgroundStyle);
  data_.sparkGrade = LayerColor::make(appearance.hue, appearance.saturation);
  paletteSizes_.fill(0);
  const auto uploadPalette = [&](unsigned slot, const std::vector<EffectRgb> &colors) {
    paletteSizes_[slot] = static_cast<unsigned>(std::min<std::size_t>(colorsPerPalette, colors.size()));
    for (unsigned i = 0; i < paletteSizes_[slot]; ++i) {
      const auto &rgb = colors[i];
      data_.palette[slot * colorsPerPalette + i] = {linear(rgb[0]), linear(rgb[1]), linear(rgb[2]), 0};
    }
  };
  if (appearance.paletteOverride)
    uploadPalette(0, appearance.resolvedPaletteColors);
  paletteChoices_ = static_cast<unsigned>(std::min<std::size_t>(
      FireworksAppearance::paletteLimit, appearance_.paletteChoices.size()));
  for (unsigned i = 0; i < paletteChoices_; ++i)
    uploadPalette(i + 1, appearance_.paletteChoices[i]);
  if (brightness_ <= 0 || intensity_ <= 0)
    clear();
}

float Fireworks12::random() noexcept { return sample(randomState_); }

unsigned Fireworks12::choosePalette() noexcept {
  if (!appearance_.randomPalette || !paletteChoices_)
    return 0;
  const bool skipLast = paletteChoices_ > 1 && lastPalette_ > 0 && lastPalette_ <= paletteChoices_;
  const unsigned choices = paletteChoices_ - static_cast<unsigned>(skipLast);
  const unsigned candidate = 1 + static_cast<unsigned>(random() * choices);
  return candidate + static_cast<unsigned>(skipLast && candidate >= lastPalette_);
}

void Fireworks12::discardExpired(double now) noexcept {
  unsigned retained = 0;
  for (unsigned i = 0; i < count_; ++i)
    if (bursts_[i].expires > now)
      bursts_[retained++] = bursts_[i];
  count_ = retained;
}

bool Fireworks12::push(double now, float x, float y) noexcept {
  if (!std::isfinite(now) || !std::isfinite(x) || !std::isfinite(y) ||
      x < 0 || x > 1 || y < 0 || y > 1 || brightness_ <= 0 || intensity_ <= 0)
    return false;
  const unsigned shape = playback_.choose(
      [this](unsigned count) { return static_cast<unsigned>(random() * count); });
  if (!shape)
    return false;
  discardExpired(now);
  if (count_ == maxBursts) {
    // Oldest remnants leave first. The input being processed always owns a slot.
    std::move(bursts_.begin() + 1, bursts_.end(), bursts_.begin());
    --count_;
  }
  const float speed = std::max(.1f, speed_);
  const float angle = shape == FireworksAppearance::heartShape ? 0 :
                      (random() - .5f) * (shape == 4 || shape == 7 ? .19f : 1.3f);
  const float radius = scale_ * (.92f + random() * .16f);
  lastPalette_ = choosePalette();
  bursts_[count_++] = {now, now + lifespan_[shape - 1] / speed,
                      {{x, y, 0, radius},
                       {appearance_.trail, appearance_.gravity, appearance_.lifetime,
                        (random() - .40f) * .075f},
                       {std::cos(angle), std::sin(angle), static_cast<float>(shape - 1), 1},
                       {}},
                      speed, lastPalette_};
  lastShape_ = shape;
  return true;
}

void Fireworks12::clear() noexcept {
  playback_.rewind();
  count_ = visibleSparks_ = lastShape_ = lastPalette_ = 0;
}

