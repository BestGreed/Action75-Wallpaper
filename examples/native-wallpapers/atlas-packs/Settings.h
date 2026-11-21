#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <vector>

namespace atlas_packs {
using json = nlohmann::json;
namespace fs = std::filesystem;
using Rgb = std::array<uint8_t, 3>;

struct Uniform {
  std::string name;
  unsigned component;
  float value;
};

struct Appearance {
  float brightness = 1, scale = 1, intensity = 1, saturation = 1, hue = 0;
  float speed = 1, duration = 0;
  bool solidBackground = false;
  float backgroundBrightness = 1, backgroundSaturation = 1, backgroundHue = 0;
  Rgb color{255, 255, 255}, backgroundColor{255, 255, 255};
  std::vector<Rgb> palette;
  std::unordered_map<std::string, float> fields;
  std::vector<Uniform> uniforms;
  std::array<std::optional<float>, 4> dynamicInputs{};
};

json readJson(const fs::path& file);
fs::path assetPath(const fs::path& pack, const json& name);
Appearance evaluate(const json& schema, const char* values);
} // namespace atlas_packs
