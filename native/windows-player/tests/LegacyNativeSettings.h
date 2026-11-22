#pragma once

#include "PackSettings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace action75::test {

class LegacyNativeSettings {
public:
  explicit LegacyNativeSettings(std::filesystem::path directory)
      : directory_(std::move(directory)), schema_(directory_ / "settings.json") {
    if (!std::filesystem::create_directory(directory_))
      throw std::runtime_error("Create test wallpaper settings directory");
  }

  ~LegacyNativeSettings() {
    std::error_code error;
    std::filesystem::remove(schema_, error);
    std::filesystem::remove(directory_, error);
  }

  void registerPack(PackSettingsStore &store, const std::string &id,
                    const std::filesystem::path &pack) const {
    store.registerPack(id, pack, pack / "settings.json", "native");
    if (!store.hasSchema(id))
      throw std::runtime_error(store.schemaError(id));
    auto rendererSchema = store.current(id).schema;
    // D3D11 regression tests use renderer bindings and playback controls.
    // Layer opacity belongs to the native plugin.
    for (auto &group : rendererSchema.at("groups")) {
      if (group.contains("playback"))
        continue;
      auto &controls = group.at("controls");
      controls.erase(std::remove_if(controls.begin(), controls.end(),
          [](const auto &control) {
            return !control.contains("binding") || control.value("appearance", "") == "opacity";
          }), controls.end());
    }
    std::ofstream stream(schema_, std::ios::binary);
    stream << rendererSchema.dump();
    stream.close();
    if (!stream)
      throw std::runtime_error("Write test wallpaper settings");
    store.registerPack(id, directory_, schema_);
    if (!store.hasSchema(id))
      throw std::runtime_error(store.schemaError(id));
  }

private:
  std::filesystem::path directory_, schema_;
};

} // namespace action75::test
