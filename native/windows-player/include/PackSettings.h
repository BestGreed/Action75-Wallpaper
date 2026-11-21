#pragma once
#include "EffectPalette.h"
#include "VariantPlayback.h"
#include <array>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace action75 {
struct PackUniformValue {
  std::string name;
  unsigned component=0;
  float value=0;
};
enum class WaterBackdrop { Moonlit, Lagoon, Dusk, Obsidian, Custom };
struct WaterSurfaceAppearance {
  WaterBackdrop backdrop = WaterBackdrop::Moonlit;
  float damping = 1.8f, ambient = .72f;
  float impactMin = .62f, impactMax = 1.45f;
  float texture = .50f, caustics = .45f;
};
enum class GlassBackdrop { Midnight, Amber, Silver, Solid };
struct GlassShatterAppearance {
  static constexpr unsigned maxBranches = 7, maxChips = 4;
  static constexpr double maxImpactStrength = 3.2, maxSpread = 2.15;
  GlassBackdrop backdrop = GlassBackdrop::Midnight;
  float impactMin = .60f, impactMax = 1.45f;
  float spread = 1, duration = 1.20f, seamWidth = 1, glints = 1, refraction = 1;
  unsigned branches = 5, chips = 1;
};
struct FireworksAppearance : VariantPlaybackRules {
  static constexpr unsigned shapeCount = maxVariants, heartShape = 8, paletteLimit = 8;
  static constexpr std::array<unsigned, shapeCount> legacyOrder{1, 4, heartShape, 2, 7, 3, 5, 6};
  FireworksAppearance() : VariantPlaybackRules{0, 0b10001111, 1, legacyOrder, false} {}
  unsigned quality = 1;
  bool randomPalette = false;
  // An empty entry represents the native colors of the chosen shape.
  std::vector<std::vector<EffectRgb>> paletteChoices;
  float density = .82f, trail = 1, gravity = 1, lifetime = 1, shimmer = .85f;
  float embers = 1, bloom = .82f, smoke = .55f, reflection = .56f, horizon = .87f;
};
struct PackAppearance {
  nlohmann::json keycap=nlohmann::json::object();
  nlohmann::json field=nlohmann::json::object();
  WaterSurfaceAppearance waterSurface;
  GlassShatterAppearance glassShatter;
  FireworksAppearance fireworks;
  std::optional<VariantPlaybackRules> playback;
  unsigned idleFps=30;
  float idleDelay=3;
  EffectRgb keycapOutlineColor{62,140,166};
  float brightness=1, scale=1, intensity=1, saturation=1, hue=0, speed=1, spriteDuration=0;
  unsigned backgroundStyle=0, backgroundFit=0;
  float backgroundZoom=1,backgroundPanX=0,backgroundPanY=0;
  float backgroundBrightness=1,backgroundSaturation=1,backgroundHue=0;
  EffectRgb backgroundColor{255,255,255};
  EffectRgb color{255,255,255};
  std::string palette="original";
  std::vector<EffectRgb> paletteColors=defaultCustomPalette();
  std::vector<EffectRgb> resolvedPaletteColors;
  bool paletteOverride=false;
  bool sourcePalette=false;
  std::vector<PackUniformValue> uniforms;
  std::array<std::optional<float>,4> dynamicInputs{};
};
struct VariantPlayback;
struct PackSettingsCandidate {
  std::string id;
  nlohmann::json schema, values;
  std::uint64_t revision=1;
  bool stored=false;
  std::shared_ptr<const VariantPlayback> playback;
  bool authorValues=false;
  // Set only by an explicit reset that could not load the saved profile.
  std::filesystem::path recoveryBackup;
};

// Schemas and profiles are read only at discovery/explicit settings operations.
// Applying a compiled appearance never reads files or evaluates expressions.
class PackSettingsStore {
public:
  struct SchemaStamp {
    std::filesystem::path directory, file;
    std::filesystem::file_time_type modified;
    std::uintmax_t size;
    bool authorValues;
  };
  using CatalogSchemas = std::unordered_map<std::string, SchemaStamp>;

  explicit PackSettingsStore(std::filesystem::path stateDirectory);
  void registerPack(const std::string& id, const std::filesystem::path& directory,
                    const std::filesystem::path& schemaFile, std::string_view kind={});
  void retainPacks(const std::unordered_set<std::string>& ids);
  CatalogSchemas catalogSchemas() const;
  static bool unchangedSchema(const SchemaStamp& previous, const std::filesystem::path& directory,
                              const std::filesystem::path& file, std::string_view kind);
  void adoptCatalog(PackSettingsStore& prepared, const std::unordered_set<std::string>& unchanged,
                    const std::string& keepActive, const std::string& rejectedId);
  void refresh(const std::string& id);
  bool hasSchema(const std::string& id) const;
  std::string schemaError(const std::string& id) const;
  PackSettingsCandidate current(const std::string& id);
  PackSettingsCandidate prepare(const std::string& id, const nlohmann::json& patch,
                               std::optional<std::uint64_t> expected={}, bool reset=false);
  PackSettingsCandidate migrate(const std::string& id, const nlohmann::json& legacy);
  void commit(const PackSettingsCandidate& candidate, bool persist=true);
  static PackSettingsCandidate packageControls(const std::filesystem::path& schemaFile);
  static PackAppearance evaluate(const PackSettingsCandidate& candidate);
  nlohmann::json legacyPatch(const std::string& id, const nlohmann::json& legacy);
  nlohmann::json bundle(const PackSettingsCandidate& candidate) const;
  static bool validPackId(const std::string& id);

private:
  struct Entry {
    std::filesystem::path directory, schemaFile;
    std::filesystem::file_time_type schemaTime{};
    std::uintmax_t schemaBytes=0;
    std::string schemaSource;
    nlohmann::json defaults=nlohmann::json::object();
    std::shared_ptr<const VariantPlayback> playback;
    std::string error;
    PackSettingsCandidate profile;
    bool authorValues=false;
    bool loaded=false;
    bool backupBeforeWrite=false;
    std::uint64_t persistedRevision=0;
    std::uint64_t revisionFloor=1;
  };
  const nlohmann::json& materializeSchema(const std::string& id, Entry& item);
  Entry& entry(const std::string& id);
  const Entry& entry(const std::string& id) const;
  std::filesystem::path profileFile(const std::string& id) const;
  std::filesystem::path stateDirectory_;
  std::unordered_map<std::string,Entry> entries_;
  std::string expandedId_;
};
}
