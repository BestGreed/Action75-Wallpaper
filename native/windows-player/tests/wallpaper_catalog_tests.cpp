#include "WallpaperCatalog.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;
using action75::Pack;
using action75::packs;

static void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Fixture {
  const fs::path temporary = fs::canonical(fs::temp_directory_path());
  const fs::path root = temporary / ("action75-catalog-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));

  Fixture() { require(fs::create_directory(root), "create temporary catalogue fixture"); }
  ~Fixture() {
    std::error_code error;
    const auto actual = fs::canonical(root, error);
    if (!error && actual.parent_path() == temporary && actual.filename() == root.filename())
      fs::remove_all(actual, error);
  }
};

static void write(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << value;
  require(bool(out), "write catalogue fixture");
}

static void sprite(const fs::path &directory, const std::string &id, const std::string &name,
                   const std::string &revision = {}) {
  json manifest = {{"id", id}, {"version", 1}, {"name", name}, {"kind", "spriteBurst"},
                   {"columns", 2}, {"rows", 1}, {"duration", 1}, {"diameter", 120},
                   {"background", "background.png"}, {"effect", "burst.png"}};
  if (!revision.empty())
    manifest["_action75Revision"] = revision;
  write(directory / "manifest.json", manifest.dump());
  write(directory / "background.png", "catalogue fixture background");
  write(directory / "burst.png", "catalogue fixture atlas");
}

static const Pack *find(const std::vector<Pack> &catalogue, std::string_view id) {
  const auto found = std::find_if(catalogue.begin(), catalogue.end(), [&](const Pack &pack) {
    return action75::samePackId(pack.id, id);
  });
  return found == catalogue.end() ? nullptr : &*found;
}

static void checkFireworksImport(const fs::path &root) {
  const std::string id = "author-fireworks";
  const std::string revision = "v-" + std::string(40, '2');
  const auto package = root / "wallpapers-native" / id;
  const auto manifest = package / "manifest.json";
  const auto settings = "options/settings.json";
  const auto preview = "art/preview.png";
  const json author{{"version", 1}, {"id", id}, {"kind", "fireworks"},
                    {"name", "Author fireworks"}, {"settings", settings}, {"preview", preview}};
  write(manifest, author.dump());
  write(package / settings, R"({"version":1,"groups":[]})");
  write(package / preview, "catalogue fixture cover");
  auto catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue.front().supported &&
              catalogue.front().importTrialAllowed,
          "fireworks author manifest can be staged and tried");

  auto unsupported = author;
  unsupported["effect"] = "external.efkefc";
  write(manifest, unsupported.dump());
  catalogue = packs(root);
  require(catalogue.size() == 1 && !catalogue.front().importTrialAllowed,
          "fireworks trial rejects an external effect field");

  const auto prefix = ".action75-versions/" + revision + "/";
  const auto snapshot = package / fs::u8path(prefix);
  auto published = author;
  published["settings"] = prefix + settings;
  published["preview"] = prefix + preview;
  published["_action75Revision"] = revision;
  write(manifest, published.dump());
  write(snapshot / "manifest.json", author.dump());
  write(snapshot / settings, R"({"version":1,"groups":[]})");
  write(snapshot / preview, "catalogue fixture cover");

  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue.front().supported &&
              catalogue.front().importedRevision == revision &&
              fs::equivalent(catalogue.front().settings, snapshot / settings),
          "published fireworks settings resolve to the selected revision");
  const action75::ImportedRevision recovery{id, revision};
  catalogue = packs(root, &recovery);
  require(catalogue.size() == 1 && catalogue.front().kind == "fireworks" &&
              catalogue.front().importedRevision == revision &&
              fs::equivalent(catalogue.front().settings, snapshot / settings),
          "fireworks can recover their retained author manifest and settings");
  write(snapshot / "manifest.json", unsupported.dump());
  require(packs(root, &recovery).empty(), "fireworks recovery rejects an external effect field");
  std::cout << "PASS: safe fireworks packs can be tried, published and recovered\n";
}

static json nativeManifest(const std::string &id) {
  return {{"version", 1}, {"id", id}, {"name", "Native wallpaper"},
          {"kind", "native"}, {"abi", 1}, {"entry", "bin/sparks.dll"},
          {"settings", "config/settings.json"}};
}

static void checkNativeImport(const fs::path &root) {
  const std::string id = "native-sparks";
  const auto directory = root / "wallpapers-native" / id;
  const auto manifest = directory / "manifest.json";
  const auto author = nativeManifest(id);
  write(manifest, author.dump());
  write(directory / "bin/sparks.dll", "metadata fixture; intentionally not executable");
  write(directory / "config/settings.json", R"({"version":1,"groups":[]})");
  auto catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue[0].supported && catalogue[0].importTrialAllowed &&
              catalogue[0].resourceDirectory == fs::canonical(directory) &&
              catalogue[0].nativeModule == fs::canonical(directory / "bin/sparks.dll"),
          "native scan resolves the resource root and DLL without loading code");

  auto dx12 = author;
  dx12["abi"] = 2;
  dx12["renderer"] = "d3d12";
  write(manifest, dx12.dump());
  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue.front().usesD3D12, "native D3D12 scan");
  dx12["abi"] = 1;
  write(manifest, dx12.dump());
  require(packs(root).empty(), "accepted D3D12 entry as ABI 1");

  for (const auto &abi : {json(2), json("1"), json(1.0), json(true)}) {
    auto invalid = author;
    invalid["abi"] = abi;
    write(manifest, invalid.dump());
    require(packs(root).empty(), "native scan accepted an unsupported ABI");
  }
  write(directory.parent_path() / "outside.dll", "outside module");
  write(directory / "bin/sparks.exe", "wrong file type");
  fs::create_directory(directory / "bin/directory.dll");
  for (const auto &entry : {std::string("../outside.dll"),
                            (directory.parent_path() / "outside.dll").u8string(),
                            std::string("bin/../bin/sparks.dll"),
                            std::string("bin/.. /bin/sparks.dll"),
                            std::string("bin/sparks.dll:alternate"),
                            std::string("bin/sparks.exe"),
                            std::string("bin/directory.dll"),
                            std::string("bin/missing.dll")}) {
    auto invalid = author;
    invalid["entry"] = entry;
    write(manifest, invalid.dump());
    require(packs(root).empty(), "native entry escaped the relative DLL file contract");
  }
  std::error_code linkError;
  fs::create_symlink(directory / "bin/sparks.dll", directory / "bin/alias.dll", linkError);
  if (!linkError) {
    auto invalid = author;
    invalid["entry"] = "bin/alias.dll";
    write(manifest, invalid.dump());
    require(packs(root).empty(), "native scan accepted a reparse point inside the package");
    fs::remove(directory / "bin/alias.dll");
  }

  const auto revision = "v-" + std::string(40, '3');
  const auto otherRevision = "v-" + std::string(40, '4');
  const auto prefix = ".action75-versions/" + revision + "/";
  const auto snapshot = directory / fs::u8path(prefix);
  auto published = author;
  published["_action75Revision"] = revision;
  published["settings"] = prefix + "config/settings.json";
  published["entry"] = prefix + "bin/sparks.dll";
  write(snapshot / "manifest.json", author.dump());
  write(snapshot / "bin/sparks.dll", "retained module");
  write(snapshot / "config/settings.json", R"({"version":1,"groups":[]})");
  write(manifest, published.dump());
  const auto expectSnapshot = [&](const std::vector<Pack> &found) {
    return found.size() == 1 && found[0].supported && found[0].importTrialAllowed &&
           found[0].importedRevision == revision &&
           found[0].resourceDirectory == fs::canonical(snapshot) &&
           found[0].nativeModule == fs::canonical(snapshot / "bin/sparks.dll") &&
           found[0].settings == fs::canonical(snapshot / "config/settings.json");
  };
  require(expectSnapshot(packs(root)), "published native paths did not resolve within their version");
  const action75::ImportedRevision recovery{id, revision};
  require(expectSnapshot(packs(root, &recovery)), "native recovery did not resolve the original manifest");
  published["entry"] = "bin/sparks.dll";
  write(manifest, published.dump());
  require(expectSnapshot(packs(root)), "version-relative native entry looked up the wrong module");

  const auto otherPrefix = ".action75-versions/" + otherRevision + "/";
  write(directory / fs::u8path(otherPrefix + "bin/sparks.dll"), "module from unselected revision");
  published["entry"] = otherPrefix + "bin/sparks.dll";
  write(manifest, published.dump());
  require(packs(root).empty(), "published native DLL from another revision was accepted");
  require(expectSnapshot(packs(root, &recovery)), "bad published path broke retained native recovery");
  std::cout << "PASS: native discovery is metadata-only and restricted to the selected resource root\n";
}

static void checkBundledBlackHole(const fs::path &root) {
  const auto official = root / "wallpapers-native/black-hole";
  const auto custom = root / "elsewhere/black-hole";
  const auto config = root / "data/app-settings.json";
  const json legacy = {{"version", 1}, {"id", "black-hole"}, {"name", "Legacy"},
                       {"kind", "blackHole"}};
  write(official / "manifest.json", legacy.dump());
  auto catalogue = packs(root);
  require(catalogue.size() == 1 && !catalogue[0].supported,
          "a legacy black hole without a native manifest must be unavailable");

  auto native = nativeManifest("black-hole");
  native.erase("settings");
  write(official / "manifest.native.json", native.dump());
  write(official / "bin/sparks.dll", "official module");
  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue[0].supported && catalogue[0].kind == "native" &&
              catalogue[0].nativeModule == fs::canonical(official / "bin/sparks.dll") &&
              catalogue[0].resourceDirectory == fs::canonical(official),
          "official black hole did not load the validated native manifest");

  write(custom / "manifest.json", legacy.dump());
  write(custom / "manifest.native.json", native.dump());
  write(custom / "bin/sparks.dll", "outside the official directory");
  write(config, json({{"wallpaperDir", custom.parent_path().u8string()}}).dump());
  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue[0].nativeModule == fs::canonical(official / "bin/sparks.dll"),
          "custom directory overrode the official alternate manifest");
  native["abi"] = 2;
  write(official / "manifest.native.json", native.dump());
  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue[0].directory == custom &&
              !catalogue[0].supported && catalogue[0].nativeModule.empty(),
          "an invalid official manifest or an alternate manifest in a custom library was accepted");
  fs::remove(official / "manifest.json");
  catalogue = packs(root);
  require(catalogue.size() == 1 && catalogue[0].supported &&
              catalogue[0].nativeModule == fs::canonical(custom / "bin/sparks.dll"),
          "migrated black hole did not survive removal of the original manifest");
}

int main() {
  try {
    Fixture fixture;
    const auto library = fixture.root / "custom-library";
    const auto installed = fixture.root / "wallpapers-native";
    const auto legacy = fixture.root / "data/native-wallpapers";
    const auto config = fixture.root / "data/app-settings.json";
    write(config, json({{"wallpaperDir", library.u8string()}}).dump());

    sprite(library / "AfterImage", "AfterImage", "migrated version");
    sprite(library / "author-private", "author-private", "author pack");
    sprite(legacy / "legacy-private", "legacy-private", "legacy pack");
    sprite(installed / "afterimage", "afterimage", "upgraded version");
    sprite(installed / "glass-shatter", "glass-shatter", "new bundled pack");
    sprite(installed / "unlisted-private", "unlisted-private", "private content outside custom library");
    sprite(installed / "carrier", "unlisted-alias", "unlisted ID in a reserved directory");

    auto catalogue = packs(fixture.root);
    const auto *upgrade = find(catalogue, "afterimage");
    require(upgrade && upgrade->id == "AfterImage" && upgrade->name == "upgraded version" &&
                upgrade->directory == installed / "afterimage",
            "bundled upgrade replaces the migrated entry and preserves its existing ID");
    require(find(catalogue, "glass-shatter") && find(catalogue, "author-private") &&
                find(catalogue, "legacy-private"),
            "new bundled packs, author packs and legacy packs coexist");
    require(!find(catalogue, "unlisted-private") && !find(catalogue, "unlisted-alias"),
            "the extra bundled scan does not expose unlisted content");
    require(fs::is_regular_file(library / "AfterImage/background.png") &&
                fs::is_regular_file(installed / "unlisted-private/background.png"),
            "discovery leaves legacy and private source files on disk");
    std::cout << "PASS: migrated library merges new and upgraded bundled packs with private packs\n";

    const std::string revision = "v-" + std::string(40, '1');
    sprite(library / "AfterImage", "AfterImage", "managed author pack", revision);
    sprite(library / "AfterImage/.action75-versions" / revision, "afterimage", "retained version");
    catalogue = packs(fixture.root);
    const auto *managed = find(catalogue, "afterimage");
    require(managed && managed->directory == library / "AfterImage" &&
                managed->importedRevision == revision,
            "bundled entry does not shadow a same-ID managed author pack");
    const action75::ImportedRevision recovery{"AfterImage", revision};
    const auto retained = packs(fixture.root, &recovery);
    require(retained.size() == 1 && retained.front().name == "retained version" &&
                retained.front().directory == library / "AfterImage",
            "retained author revision still resolves through the configured library");
    std::cout << "PASS: same-ID managed authors and their recovery revisions keep precedence\n";

    const auto invalid = fixture.root / "library-now-a-file";
    write(invalid, "not a directory");
    write(config, json({{"wallpaperDir", invalid.u8string()}}).dump());
    catalogue = packs(fixture.root);
    require(find(catalogue, "afterimage") && find(catalogue, "glass-shatter") &&
                find(catalogue, "legacy-private"),
            "enumerating a configured file is isolated to that library");
    require(!find(catalogue, "author-private") && !find(catalogue, "unlisted-private") &&
                fs::is_regular_file(invalid),
            "bad preference does not change or widen the library on disk");
    std::cout << "PASS: a regular file at the configured path leaves the other libraries available\n";

    write(config, "{}");
    require(find(packs(fixture.root), "unlisted-private"),
            "the default configured library still accepts user packs");
    std::cout << "PASS: user packs remain available when the default library is expressly selected\n";
    checkFireworksImport(fixture.root / "fireworks-import");
    checkNativeImport(fixture.root / "native-import");
    checkBundledBlackHole(fixture.root / "native-built-in");
    return 0;
  } catch (const std::exception &failure) {
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
