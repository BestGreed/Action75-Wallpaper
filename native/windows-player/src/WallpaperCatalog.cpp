#include "WallpaperCatalog.h"
#include "BundledWallpaperIds.h"
#include "Diagnostics.h"
#include "PackSettings.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace action75 {
namespace fs = std::filesystem;
using json = nlohmann::json;

bool validImportedRevision(std::string_view revision) {
  return revision.size() == 42 && revision.substr(0, 2) == "v-" &&
         std::all_of(revision.begin() + 2, revision.end(),
                     [](unsigned char c) { return std::isxdigit(c); });
}
bool samePackId(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](unsigned char left, unsigned char right) {
           return std::tolower(left) == std::tolower(right);
         });
}
static bool bundledWallpaperId(std::string_view id) {
  return std::any_of(BundledWallpaperIds.begin(), BundledWallpaperIds.end(),
                     [&](std::string_view bundled) { return samePackId(bundled, id); });
}
static json readManifest(const fs::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() > 1024 * 1024)
    throw std::runtime_error("Cannot read wallpaper manifest");
  input.seekg(0);
  return json::parse(input);
}

static bool fireworksAuthorManifest(const json &manifest) {
  static constexpr std::array<std::string_view, 10> fields = {
      "version", "id", "name", "kind", "description", "settings",
      "preview", "author", "license", "packVersion"};
  for (const auto &field : manifest.items()) {
    if (field.key() == "enabled" && field.value() == true)
      continue;
    if (std::find(fields.begin(), fields.end(), field.key()) == fields.end())
      return false;
  }
  return true;
}

static bool existingNonReparsePath(const fs::path &path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static fs::path strictPackPath(const fs::path &root, const fs::path &relative) {
  if (relative.empty() || relative.has_root_path())
    throw std::runtime_error("Native wallpaper path must be relative");
  auto path = fs::absolute(root).lexically_normal();
  const auto checkAttributes = [](const fs::path &item) {
    if (!existingNonReparsePath(item))
      throw std::runtime_error("Native wallpaper paths must exist and cannot be reparse points");
  };
  checkAttributes(path);
  const auto base = fs::canonical(path);
  for (const auto &part : relative) {
    const auto &name = part.native();
    if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == L':'; }))
      throw std::runtime_error("Invalid native wallpaper path component");
    path /= part;
    checkAttributes(path);
  }
  const auto resolved = fs::canonical(path);
  const auto relation = resolved.lexically_relative(base);
  if (relation.empty() || relation == L"." || *relation.begin() == L"..")
    throw std::runtime_error("Native wallpaper asset escapes its resource directory");
  return resolved;
}

static fs::path nativeAsset(const Pack &pack, const std::string &asset, bool published) {
  const auto path = fs::u8path(asset);
  auto part = path.begin();
  if (part != path.end() && samePackId(part->u8string(), ".action75-versions")) {
    if (!published || ++part == path.end() || !samePackId(part->u8string(), pack.importedRevision))
      throw std::runtime_error("Native wallpaper asset is outside the selected revision");
    fs::path relative;
    for (++part; part != path.end(); ++part)
      relative /= *part;
    return strictPackPath(pack.resourceDirectory, relative);
  }
  return strictPackPath(pack.resourceDirectory, path);
}

static void readNativePack(Pack &pack, const json &manifest, bool recovering) {
  static constexpr std::array<std::string_view, 14> fields = {
      "version", "id", "name", "kind", "description", "settings", "preview",
      "author", "license", "packVersion", "enabled", "abi", "entry", "_action75Revision"};
  for (const auto &field : manifest.items())
    if (std::find(fields.begin(), fields.end(), field.key()) == fields.end())
      throw std::runtime_error("Unsupported native wallpaper manifest property");
  const auto &abi = manifest.at("abi");
  if (!abi.is_number_integer() || abi != 1)
    throw std::runtime_error("Unsupported native wallpaper ABI");
  pack.resourceDirectory = pack.importedRevision.empty()
                               ? fs::canonical(pack.directory)
                               : strictPackPath(pack.directory, fs::u8path(".action75-versions/" +
                                                                          pack.importedRevision));
  const bool published = !recovering && !pack.importedRevision.empty();
  pack.nativeModule = nativeAsset(pack, manifest.at("entry").get<std::string>(), published);
  if (!samePackId(pack.nativeModule.extension().u8string(), ".dll") ||
      !fs::is_regular_file(pack.nativeModule))
    throw std::runtime_error("Native wallpaper entry must be a DLL file");
  if (manifest.contains("settings")) {
    pack.settings = nativeAsset(pack, manifest.at("settings").get<std::string>(), published);
    if (!samePackId(pack.settings.extension().u8string(), ".json") ||
        !fs::is_regular_file(pack.settings))
      throw std::runtime_error("Native wallpaper settings must be a JSON file");
  }
  if (manifest.contains("preview")) {
    const auto preview = nativeAsset(pack, manifest.at("preview").get<std::string>(), published);
    const auto extension = preview.extension().u8string();
    if ((!samePackId(extension, ".png") && !samePackId(extension, ".jpg") &&
         !samePackId(extension, ".jpeg")) || !fs::is_regular_file(preview))
      throw std::runtime_error("Invalid native wallpaper preview file");
  }
}

fs::path packAsset(const fs::path &root, const std::string &relative) {
  fs::path path = fs::u8path(relative);
  if (path.empty())
    return {};
  if (path.has_root_path() ||
      std::any_of(relative.begin(), relative.end(), [](unsigned char c) { return c < 32; }))
    throw std::runtime_error("Pack asset must be relative");
  auto full = fs::weakly_canonical(root / path), base = fs::weakly_canonical(root);
  auto relation = full.lexically_relative(base);
  if (relation.empty() || *relation.begin() == L"..")
    throw std::runtime_error("Pack asset escapes its directory");
  return full;
}
static bool isPngAsset(const fs::path &file) {
  auto extension = file.extension().u8string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".png" && fs::is_regular_file(file);
}
std::array<fs::path, 2> wallpaperDirectories(const fs::path &root) {
  auto configured = root / L"wallpapers-native";
  try {
    configured = configuredDirectory(root, "wallpaperDir", L"wallpapers-native");
  } catch (const std::exception &) {
    // An unreadable preference must not make the installed library unavailable.
  }
  return {configured, root / L"data/native-wallpapers"};
}
std::vector<Pack> packs(const fs::path &root, const ImportedRevision *recovery) {
  if (recovery && !validImportedRevision(recovery->revision))
    return {};
  const auto libraries = wallpaperDirectories(root);
  const auto bundled = root / L"wallpapers-native";
  std::error_code comparisonError;
  const bool separateBundled = !recovery && !fs::equivalent(libraries.front(), bundled, comparisonError);
  const std::array folders = {libraries[0], libraries[1], bundled};
  std::vector<Pack> result;
  for (std::size_t i = 0; i < (separateBundled ? folders.size() : libraries.size()); ++i) {
    const bool bundledOnly = i == libraries.size();
    // Migration removes the original manifest after copying the library.
    const bool bundledBlackHoleSource = bundledOnly || (i == 0 && (!separateBundled ||
        GetFileAttributesW((bundled / L"black-hole/manifest.json").c_str()) == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)));
    std::error_code scanError;
    // Each library is independent, including the case where its path is a regular file.
    for (fs::directory_iterator it(folders[i], scanError), end; !scanError && it != end;
         it.increment(scanError)) {
      const auto &entry = *it;
      std::error_code entryError;
      if (!entry.is_directory(entryError) || !existingNonReparsePath(entry.path()))
        continue;
      try {
        const auto folderId = entry.path().filename().u8string();
        if ((recovery && !samePackId(folderId, recovery->id)) ||
            (bundledOnly && !bundledWallpaperId(folderId)))
          continue;
        // The published manifest rewrites asset paths. A retained revision still has its
        // original manifest and resolves those paths within its immutable resource directory.
        const auto assets = recovery
                                ? strictPackPath(entry.path(), fs::u8path(".action75-versions/" +
                                                                        recovery->revision))
                                : entry.path();
        const auto manifest = assets / L"manifest.json";
        if (!existingNonReparsePath(manifest) || !fs::is_regular_file(manifest))
          continue;
        auto m = readManifest(manifest);
        const bool alternateManifest =
            !recovery && bundledBlackHoleSource && samePackId(folderId, "black-hole") &&
            m.value("kind", std::string()) == "blackHole" &&
            samePackId(m.value("id", std::string()), "black-hole") &&
            !m.contains("_action75Revision") &&
            GetFileAttributesW((assets / L"manifest.native.json").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (alternateManifest) {
          m = readManifest(strictPackPath(entry.path(), L"manifest.native.json"));
          if (m.value("kind", std::string()) != "native" ||
              !samePackId(m.value("id", std::string()), "black-hole") ||
              m.contains("_action75Revision"))
            continue;
        }
        if (m.value("version", 0) != 1)
          continue;
        Pack p;
        p.directory = entry.path();
        p.id = m.at("id").get<std::string>();
        p.name = m.at("name").get<std::string>();
        p.description = m.value("description", std::string());
        p.kind = m.value("kind", std::string("effekseer"));
        p.importedRevision = m.value("_action75Revision", std::string());
        const bool native = p.kind == "native";
        const bool authorKind = native || p.kind == "spriteBurst" || p.kind == "effekseer" ||
                                (p.kind == "fireworks" && fireworksAuthorManifest(m));
        if (recovery) {
          if (!samePackId(p.id, recovery->id) || !p.importedRevision.empty() || !authorKind ||
              m.contains("canvasEffect") || m.value("runtimeControls", false))
            continue;
          p.id = recovery->id;
          p.importedRevision = recovery->revision;
        }
        if ((!p.importedRevision.empty() && !validImportedRevision(p.importedRevision)) ||
            (bundledOnly && (!p.importedRevision.empty() || !samePackId(folderId, p.id))))
          continue;
        if (p.kind == "field") {
          p.fieldTheme = m.at("fieldTheme").get<unsigned>();
          if (p.fieldTheme > 3)
            continue;
        }
        if (native)
          readNativePack(p, m, recovery != nullptr);
        else if (m.contains("settings"))
          p.settings = packAsset(assets, m.at("settings").get<std::string>());
        if (m.contains("layout"))
          p.layout = packAsset(assets, m.at("layout").get<std::string>());
        p.effect = packAsset(assets, m.value("effect", std::string()));
        p.runtimeControls = m.value("runtimeControls", false);
        if (m.contains("effects")) {
          if (p.kind != "effekseer" || !m["effects"].is_array() || m["effects"].empty() ||
              m["effects"].size() > 32)
            continue;
          p.declaredEffectSlots = m["effects"].size();
          for (const auto &asset : m["effects"]) {
            const auto path = packAsset(assets, asset.get<std::string>());
            if (path.empty() || !fs::is_regular_file(path))
              throw std::runtime_error("Effect variant is missing");
            if (std::find(p.effects.begin(), p.effects.end(), path) == p.effects.end())
              p.effects.push_back(path);
          }
          p.effect = p.effects.front();
        } else if (p.kind == "effekseer" && !p.effect.empty()) {
          p.effects.push_back(p.effect);
        }
        p.background = packAsset(assets, m.value("background", std::string()));
        const auto canvas = m.value("canvasEffect", std::string());
        if (!canvas.empty()) {
          if (p.kind != "effekseer" || !p.background.empty())
            continue;
          p.canvasEffect = packAsset(assets, canvas);
          const auto canvasEntry = fs::symlink_status(assets / fs::u8path(canvas));
          auto extension = p.canvasEffect.extension().u8string();
          std::transform(extension.begin(), extension.end(), extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (!fs::is_regular_file(canvasEntry) || (extension != ".efk" && extension != ".efkefc"))
            continue;
        }
        p.scale = m.value("scale", 1.0f);
        if (!std::isfinite(p.scale) || p.scale <= 0 || p.scale > 1000)
          continue;
        if (native) {
          p.supported = true;
        } else if (p.kind == "waterSurface" || p.kind == "fireworks" || p.kind == "glassShatter") {
          p.supported = p.background.empty();
        } else if (p.kind == "proceduralBurst") {
          p.proceduralStyle = m.at("proceduralStyle").get<unsigned>();
          p.sprite.diameter = m.value("diameter", 240.f);
          p.sprite.duration = m.value("duration", 1.35f);
          if (p.proceduralStyle > 1 || !std::isfinite(p.sprite.diameter) ||
              p.sprite.diameter < 48 || p.sprite.diameter > 500 ||
              !std::isfinite(p.sprite.duration) || p.sprite.duration < .12f ||
              p.sprite.duration > 3)
            continue;
          p.supported = p.background.empty() || isPngAsset(p.background);
          if (p.proceduralStyle == 0) {
            p.filmShapes =
                packAsset(assets, m.value("filmShapes", std::string("film-shapes.png")));
            p.supported = p.supported && isPngAsset(p.filmShapes);
          }
        } else if (p.kind == "spriteBurst") {
          if (!m.at("columns").is_number() || !m.at("rows").is_number() ||
              !m.at("duration").is_number() || !m.at("diameter").is_number())
            continue;
          // Per JSON number semantics, 2 and 2.0 are the same valid row/column
          // count.
          const auto columns = m["columns"].get<double>(), rows = m["rows"].get<double>();
          const auto duration = m["duration"].get<double>(), diameter = m["diameter"].get<double>();
          if (columns < 1 || columns > 12 || rows < 1 || rows > 12 ||
              columns != std::floor(columns) || rows != std::floor(rows) ||
              !std::isfinite(duration) || duration < .12 || duration > 3 ||
              !std::isfinite(diameter) || diameter < 48 || diameter > 440)
            continue;
          const auto composition = m.value("composition", std::string("add"));
          if (composition != "add" && composition != "multiply")
            continue;
          p.sprite = {static_cast<unsigned>(columns), static_cast<unsigned>(rows),
                      static_cast<float>(duration), static_cast<float>(diameter),
                      composition == "multiply"};
          p.supported =
              SpriteBurst::valid(p.sprite) && isPngAsset(p.effect) && isPngAsset(p.background);
        } else {
          p.supported =
              ((p.kind == "keycap" || p.kind == "field") && !p.layout.empty() &&
               fs::is_regular_file(p.layout)) ||
              (p.kind == "effekseer" && !p.effect.empty() && fs::is_regular_file(p.effect)) ||
              ((p.kind == "image" || p.kind == "video") && !p.background.empty() &&
               fs::is_regular_file(p.background));
        }
        if (m.value("enabled", true) == false)
          p.supported = false;
        p.importTrialAllowed = authorKind && !p.runtimeControls && p.canvasEffect.empty();
        if (!PackSettingsStore::validPackId(p.id) || p.name.size() > 256)
          continue;
        if ((recovery || bundledOnly) && !p.supported)
          continue;
        const auto existing = std::find_if(result.begin(), result.end(), [&](const Pack &pack) {
          return samePackId(pack.id, p.id);
        });
        if (existing == result.end())
          result.push_back(std::move(p));
        else if (bundledOnly && existing->importedRevision.empty()) {
          // Upgrades replace the view of legacy bundled copies, while a managed
          // author pack with the same ID keeps its published version.
          p.id = existing->id;
          *existing = std::move(p);
        }
        if (recovery && !result.empty())
          return result;
      } catch (const std::exception &) { /* Invalid packs do not prevent valid
                                            packs from loading. */
      }
    }
  }
  return result;
}

} // namespace action75
