#pragma once

#include "PackSettings.h"
#include "SpriteBurst.h"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace action75 {

struct Pack {
  std::string id, name, description, kind = "effekseer";
  std::string importedRevision, desktopOpacityControl;
  std::filesystem::path directory, effect, background, canvasEffect, settings, layout, filmShapes;
  std::filesystem::path nativeModule, resourceDirectory;
  std::vector<std::filesystem::path> effects;
  std::size_t declaredEffectSlots = 0;
  bool usesD3D12 = false;
  bool supported = false;
  bool importTrialAllowed = false;
  bool runtimeControls = false;
  float scale = 1;
  SpriteBurst::Params sprite;
  unsigned fieldTheme = 0, proceduralStyle = 0;
};
struct ImportedRevision {
  std::string id, revision;
};
bool validImportedRevision(std::string_view revision);
bool samePackId(std::string_view a, std::string_view b);
std::filesystem::path packAsset(const std::filesystem::path &root, const std::string &relative);
std::array<std::filesystem::path, 2> wallpaperDirectories(const std::filesystem::path &root);
std::vector<Pack> packs(const std::filesystem::path &root, const ImportedRevision *recovery = nullptr);

struct WallpaperCatalogSnapshot {
  std::vector<Pack> entries;
  PackSettingsStore settings;
  std::optional<Pack> retainedActive;
  std::unordered_set<std::string> unchangedSchemas;
};
WallpaperCatalogSnapshot scanWallpaperCatalog(const std::filesystem::path &root,
                                              const std::filesystem::path &stateDirectory,
                                              const std::optional<Pack> &activeImport,
                                              const PackSettingsStore::CatalogSchemas &schemas);

} // namespace action75
