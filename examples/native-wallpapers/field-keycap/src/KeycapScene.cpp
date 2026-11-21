#include "KeycapScene.h"
#include "KeycapGraded.h"
#include "KeycapPixel.h"
#include "KeycapVertex.h"

#include <set>
#include <stdexcept>

namespace wallpaper {
namespace {
struct alignas(16) Constants {
  float display[4];
  float style[4];
  float motion[4];
  ColorRow lineColor, glowColor;
  float rects[96][4];
  float pulses[96][4];
  Background::Constants background;
};
static_assert(sizeof(Constants) == (5 + 96 * 2 + 4) * 16);
} // namespace

KeycapScene::KeycapScene(const A75Create12 &host) : graphics_(host.device, sizeof(Constants), false) {
  const auto layout = readJson(std::filesystem::path(host.pack_dir) / L"layout.json");
  const auto &entries = layout.at("keys");
  if (layout.at("coordinateSpace") != Json::array({1920, 720}) || entries.size() != keys_.size())
    throw std::runtime_error("Expected an 84-key 1920x720 layout");
  std::set<std::string> codes;
  unsigned index = 0;
  for (const auto &entry : entries) {
    const auto &center = entry.at("centerPx");
    const auto &size = entry.at("sizePx");
    Key key{entry.at("code"), center.at("x"), center.at("y"), size.at("width"), size.at("height")};
    // Use the original geometry, including keys clipped by the display bezel.
    if (!codes.insert(key.code).second || !std::isfinite(key.x + key.y + key.width + key.height) ||
        key.width <= 0 || key.height <= 0 || key.width > 1920 || key.height > 720 ||
        key.x + key.width / 2 <= 0 || key.y + key.height / 2 <= 0 ||
        key.x - key.width / 2 >= 1920 || key.y - key.height / 2 >= 720)
      throw std::runtime_error("Invalid cap bounds");
    keys_[index++] = std::move(key);
  }
  normal_ = graphics_.pipeline(shader(KeycapVertex), shader(KeycapPixel), OutputFormat);
}

void KeycapScene::configure(const Json &values) {
  background_.configure(values);
  attack_ = values.value("attack", .045f);
  hold_ = values.value("hold", .07f);
  release_ = values.value("release", .65f);
  line_ = values.value("lineWidth", 1.2f);
  radius_ = values.value("radius", 8.f);
  outline_ = values.value("outline", .22f);
  gain_ = values.value("brightness", 1.1f) * values.value("intensity", 1.f);
  softness_ = values.value("softness", .65f);
  design_ = values.value("design", 0.f);
  flowSpeed_ = values.value("flowSpeed", .45f);
  detail_ = values.value("detail", .65f);
  chromatic_ = values.value("chromatic", .7f);
  ambient_ = values.value("ambient", .35f);
  offsetX_ = values.value("offsetX", 0.f);
  offsetY_ = values.value("offsetY", 0.f);
  scaleX_ = values.value("scaleX", 1.f);
  scaleY_ = values.value("scaleY", 1.f);
  gap_ = values.value("capScale", 1.f);
  const auto color = values.value("color", Rgb{64, 217, 255});
  const auto outlineColor = values.value("outlineColor", Rgb{62, 140, 166});
  for (unsigned channel = 0; channel < 3; ++channel) {
    glowColor_[channel] = color[channel] / 255.f;
    lineColor_[channel] = outlineColor[channel] / 255.f;
  }
}

float KeycapScene::level(const Key &key, double now) const {
  if (key.start < 0)
    return 0;
  const float age = static_cast<float>(std::max(0., now - key.start));
  const auto smooth = [](float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3 - 2 * x);
  };
  if (age < attack_)
    return key.initial + (1 - key.initial) * smooth(age / attack_);
  if (age < attack_ + hold_)
    return 1;
  return 1 - smooth((age - attack_ - hold_) / release_);
}

void KeycapScene::press(const A75Event &event) {
  if (!event.code)
    return;
  for (auto &key : keys_) {
    if (key.code == event.code) {
      key.initial = level(key, event.seconds);
      key.start = event.seconds;
      break;
    }
  }
}

void KeycapScene::clear() {
  for (auto &key : keys_) {
    key.start = -1;
    key.initial = 0;
  }
}

bool KeycapScene::animated() const {
  return !background_.solid && flowSpeed_ > 0 &&
         (outline_ > 0 || (ambient_ > 0 && (chromatic_ > 0 || (design_ >= .5f && detail_ > 0))));
}

int32_t KeycapScene::render(const A75Frame12 &frame, bool paused) {
  if (background_.gpu.rows[2][3] == 0 && background_.gpu.solid[3] == 0) {
    frame.commands->ClearRenderTargetView(frame.rtv, background_.clearColor().data(), 0, nullptr);
    return -1;
  }
  if (background_.edited && !graded_)
    graded_ = graphics_.pipeline(shader(KeycapVertex), shader(KeycapGraded), OutputFormat);
  const auto width = frame.width, height = frame.height;
  const double now = frame.seconds;
  Constants constants{};
  constants.background = background_.gpu;
  constants.display[0] = static_cast<float>(width);
  constants.display[1] = static_cast<float>(height);
  constants.display[2] = line_ * height / 720.f;
  constants.display[3] = radius_ * height / 720.f;
  constants.style[0] = outline_;
  constants.style[1] = gain_;
  constants.style[2] = softness_;
  constants.style[3] = design_;
  constants.motion[0] = static_cast<float>(std::fmod(now, 4096.)) * flowSpeed_;
  constants.motion[1] = detail_;
  constants.motion[2] = chromatic_;
  constants.motion[3] = ambient_;
  constants.lineColor = lineColor_;
  constants.glowColor = glowColor_;
  bool active = false;
  for (std::size_t i = 0; i < keys_.size(); ++i) {
    auto &key = keys_[i];
    const float pulse = level(key, now);
    if (pulse <= 0 && now - key.start >= attack_ + hold_ + release_)
      key.start = -1;
    active |= key.start >= 0;
    constants.rects[i][0] = ((key.x - 960) * scaleX_ + 960 + offsetX_) * width / 1920.f;
    constants.rects[i][1] = ((key.y - 360) * scaleY_ + 360 + offsetY_) * height / 720.f;
    constants.rects[i][2] = key.width * scaleX_ * gap_ * width / 1920.f;
    constants.rects[i][3] = key.height * scaleY_ * gap_ * height / 720.f;
    constants.pulses[i][0] = pulse;
    constants.pulses[i][1] = key.start >= 0 ? static_cast<float>(now - key.start) : 0;
  }

  auto *commands = frame.commands;
  commands->ClearRenderTargetView(frame.rtv, background_.clearColor().data(), 0, nullptr);
  graphics_.bind(commands, &constants);
  commands->SetPipelineState(background_.edited ? graded_.Get() : normal_.Get());
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commands->DrawInstanced(4, static_cast<UINT>(keys_.size()), 0, 0);
  return !paused && ((active && background_.gpu.rows[2][3] > 0) || (animated() && background_.gpu.solid[3] > 0)) ? 0 : -1;
}
} // namespace wallpaper
