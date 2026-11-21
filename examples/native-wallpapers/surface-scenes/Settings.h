#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace surface {

using Json = nlohmann::json;
using Rgb = std::array<unsigned char, 3>;

class Settings {
public:
  explicit Settings(const std::filesystem::path &file) {
    std::ifstream stream(file);
    if (!stream)
      throw std::runtime_error("Missing surface wallpaper settings.json");
    stream >> schema_;
    for (const auto &group : schema_.at("groups")) {
      for (const auto &control : group.at("controls")) {
        const auto id = control.at("id").get<std::string>();
        controls_.emplace(id, &control);
        defaults_[id] = control.at("default");
      }
    }
    values_ = defaults_;
  }
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  void update(const char *text) {
    const auto patch = Json::parse(text ? text : "{}");
    if (!patch.is_object())
      throw std::runtime_error("Surface settings must be a JSON object");
    values_ = defaults_;
    for (const auto &[id, value] : patch.items())
      if (values_.contains(id))
        values_[id] = value;
  }

  double number(const std::string &id) const {
    const auto &desc = control(id);
    const auto &value = values_.at(id);
    double result = value.get<double>();
    if (!std::isfinite(result))
      throw std::runtime_error("Invalid numeric surface setting");
    if (desc.at("type") == "slider")
      result = std::clamp(result, desc.at("min").get<double>(), desc.at("max").get<double>());
    if (desc.contains("binding") && desc["binding"].contains("mapping")) {
      const auto &map = desc["binding"]["mapping"];
      const double start = map.at("inputMin").get<double>(), end = map.at("inputMax").get<double>();
      const double first = map.at("outputMin").get<double>(), last = map.at("outputMax").get<double>();
      const double t = std::clamp((result - start) / (end - start), 0.0, 1.0);
      result = first + (last - first) * t;
    }
    return result;
  }

  float scalar(const std::string &id) const { return static_cast<float>(number(id)); }
  unsigned index(const std::string &id) const {
    const auto &value = values_.at(id);
    const auto &desc = control(id);
    if (desc.at("type") == "select") {
      for (const auto &option : desc.at("options"))
        if (option.at("value") == value)
          return value.get<unsigned>();
      return desc.at("default").get<unsigned>();
    }
    return static_cast<unsigned>(number(id));
  }
  bool toggle(const std::string &id) const { return values_.at(id).get<bool>(); }
  Rgb color(const std::string &id) const { return values_.at(id).get<Rgb>(); }
  const Json &value(const std::string &id) const { return values_.at(id); }
  const Json &control(const std::string &id) const { return *controls_.at(id); }
  const Json &groups() const { return schema_.at("groups"); }

private:
  Json schema_, defaults_ = Json::object(), values_;
  std::map<std::string, const Json *> controls_;
};

inline float linear(unsigned color) {
  const float code = static_cast<float>(color) / 255.f;
  return code <= .04045f ? code / 12.92f : std::pow((code + .055f) / 1.055f, 2.4f);
}

} // namespace surface
