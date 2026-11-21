#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace atlas_packs {
namespace {
double finiteNumber(const json& value) {
  const auto number = value.is_boolean() ? static_cast<double>(value.get<bool>()) : value.get<double>();
  if (!std::isfinite(number) || std::abs(number) > 1000000.)
    throw std::runtime_error("Invalid numeric wallpaper value");
  return number;
}

double mappedNumber(const json& binding, const json& value) {
  const double x = finiteNumber(value);
  auto it = binding.find("mapping");
  if (it == binding.end() || it->value("type", std::string("identity")) == "identity")
    return x;
  const auto& mapping = *it;
  const auto type = mapping.at("type").get<std::string>();
  if (type == "piecewise") {
    const auto& points = mapping.at("points");
    if (x <= points.front()[0].get<double>())
      return points.front()[1].get<double>();
    for (std::size_t i = 1; i < points.size(); ++i) {
      if (const double right = points[i][0]; x <= right) {
        const double left = points[i - 1][0], start = points[i - 1][1], end = points[i][1];
        return start + (end - start) * (x - left) / (right - left);
      }
    }
    return points.back()[1].get<double>();
  }
  const double min = mapping.at("inputMin"), max = mapping.at("inputMax");
  double t = std::clamp((x - min) / (max - min), 0.0, 1.0);
  if (type == "power")
    t = std::pow(t, mapping.at("exponent").get<double>());
  const double start = mapping.at("outputMin"), end = mapping.at("outputMax");
  return start + (end - start) * t;
}

Rgb rgb(const json& value) {
  if (!value.is_array() || value.size() != 3)
    throw std::runtime_error("A color requires three RGB channels");
  Rgb result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    const auto channel = value[i].get<int>();
    if (!value[i].is_number_integer() || channel < 0 || channel > 255)
      throw std::runtime_error("Invalid RGB channel");
    result[i] = static_cast<uint8_t>(channel);
  }
  return result;
}

std::vector<Rgb> palette(const json& control, const json& value) {
  const auto preset = value.at("preset").get<std::string>();
  const json* colors = &value.at("colors");
  if (preset != "custom") {
    if (preset == "original")
      return {};
    const auto& choices = control.at("palettes");
    const auto match = std::find_if(choices.begin(), choices.end(), [&](const json& choice) {
      return choice.at("id") == preset;
    });
    if (match == choices.end())
      throw std::runtime_error("Unknown wallpaper palette");
    if (match->value("source", false))
      return {};
    colors = &match->at("colors");
  }
  if (!colors->is_array() || colors->empty() || colors->size() > 9)
    throw std::runtime_error("Invalid wallpaper palette size");
  std::vector<Rgb> result;
  for (const auto& color : *colors)
    result.push_back(rgb(color));
  return result;
}
} // namespace

json readJson(const fs::path& file) {
  std::ifstream input(file);
  return json::parse(input);
}

fs::path assetPath(const fs::path& pack, const json& name) {
  const auto relative = fs::u8path(name.get<std::string>());
  const auto normalized = relative.lexically_normal();
  if (relative.empty() || relative.has_root_path() || normalized.begin() == normalized.end() ||
      *normalized.begin() == L"..")
    throw std::runtime_error("Wallpaper assets must be inside the package");
  return pack / normalized;
}

Appearance evaluate(const json& schema, const char* values) {
  const auto profile = json::parse(values);
  if (!profile.is_object())
    throw std::runtime_error("Expected an object with wallpaper settings");
  Appearance result;
  std::optional<Rgb> solid;
  using FloatMember = float Appearance::*;
  static constexpr std::pair<std::string_view, FloatMember> attributes[]{
      {"effectBrightness", &Appearance::brightness}, {"effectScale", &Appearance::scale},
      {"effectIntensity", &Appearance::intensity}, {"effectSaturation", &Appearance::saturation},
      {"effectHue", &Appearance::hue}, {"effect.speed", &Appearance::speed},
      {"sprite.duration", &Appearance::duration},
      {"background.brightness", &Appearance::backgroundBrightness},
      {"background.saturation", &Appearance::backgroundSaturation},
      {"background.hue", &Appearance::backgroundHue},
  };

  for (const auto& group : schema.at("groups")) {
    for (const auto& control : group.at("controls")) {
      const auto& binding = control.at("binding");
      const auto target = binding.at("target").get<std::string>();
      const auto item = profile.find(control.at("id").get<std::string>());
      const auto& value = item == profile.end() ? control.at("default") : *item;

      if (target == "effectPalette") {
        result.palette = palette(control, value);
      } else if (target == "effectColor") {
        result.color = rgb(value);
      } else if (target == "background.color") {
        result.backgroundColor = rgb(value);
      } else if (target == "background.solidColor") {
        solid = rgb(value);
      } else {
        const auto mapped = static_cast<float>(mappedNumber(binding, value));
        if (target == "foreground.opacity")
          result.foregroundOpacity = std::clamp(mapped / 100.f, 0.f, 1.f);
        else if (target == "background.opacity")
          result.backgroundOpacity = std::clamp(mapped / 100.f, 0.f, 1.f);
        else if (target == "background.style")
          result.solidBackground = mapped == 1.f;
        else if (target.compare(0, 6, "field.") == 0)
          result.fields[target.substr(6)] = mapped;
        else if (target == "effect.dynamicInput")
          result.dynamicInputs.at(binding.at("index").get<unsigned>()) = mapped;
        else if (target == "material.uniform")
          result.uniforms.push_back({binding.at("name"), binding.value("component", 0u), mapped});
        else {
          const auto member = std::find_if(std::begin(attributes), std::end(attributes),
                                         [&](auto attribute) { return attribute.first == target; });
          if (member == std::end(attributes))
            throw std::runtime_error("Unsupported wallpaper control binding: " + target);
          result.*(member->second) = mapped;
        }
      }
    }
  }
  if (result.solidBackground && solid)
    result.backgroundColor = *solid;
  return result;
}
} // namespace atlas_packs
