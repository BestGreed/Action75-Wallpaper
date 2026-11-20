#include "PlayerConfig.h"
#include "RawKeyboard.h"
#include "MonitorSelection.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename F> void rejects(F function, const char* message) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}

nlohmann::json read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read test fixture: " + path.u8string());
    return nlohmann::json::parse(input);
}

bool close(float actual, double expected) {
    return std::abs(static_cast<double>(actual) - expected) < 1e-6;
}

void configTests(const std::filesystem::path& root) {
    const auto base = read(root / "existing-resources" / "finalmouse" / "action75-ue-keymap.json");
    const auto calibration = read(root / "assets" / "defaults" / "fm-calibration.json");
    const auto config = action75::PlayerConfig::fromDocuments(base, calibration);
    require(config.keys.size() == 83, "Actual Action75 mapping must contain all 83 hardware keys");
    require(config.deviceGroup == "VID_1CA6&PID_1A0B", "Default device must select Action75 USB group");
    require(config.revision == calibration.value("revision", std::string{}), "Preserve calibration revision");
    for (const auto& key : base["keys"]) {
        const auto code = key["code"].get<std::string>();
        const auto* mapped = config.findCode(code);
        require(mapped != nullptr, "Every base key must survive calibration merge");
        require(mapped->usage == key["usage"].get<std::uint16_t>(), "USB usage must remain unchanged");
        require(config.findUsage(mapped->usage) == mapped, "USB and code lookups must agree");
        const auto& points = calibration["points"];
        const double expectedX = points.contains(code) ? points[code]["x"].get<double>() : key["x"].get<double>() / 1920;
        const double expectedY = points.contains(code) ? points[code]["y"].get<double>() : key["y"].get<double>() / 555;
        require(close(mapped->x, expectedX) && close(mapped->y, expectedY), "Apply normalized calibration exactly once");
        require(mapped->x >= 0 && mapped->x <= 1 && mapped->y >= 0 && mapped->y <= 1, "All coordinates must be normalized");
    }
    const auto uncalibrated = action75::PlayerConfig::fromDocuments(base, nullptr);
    require(close(uncalibrated.findCode("Escape")->x, 96.0 / 1920), "Base normalization must match existing JS");
    auto partial = nlohmann::json{{"version",1},{"points",{{"KeyA",{{"x",0.0},{"y",1.0}}}}}};
    const auto merged = action75::PlayerConfig::fromDocuments(base, partial);
    require(merged.findCode("KeyA")->x == 0 && merged.findCode("KeyA")->y == 1, "Accept exact screen edges");
    require(merged.findCode("KeyW")->x == uncalibrated.findCode("KeyW")->x, "Unspecified overrides retain defaults");
    auto invalid = partial;
    invalid["points"]["UnknownKey"] = {{"x",0.5},{"y",0.5}};
    rejects([&] { action75::PlayerConfig::fromDocuments(base, invalid); }, "Reject unknown calibration key");
    invalid = partial; invalid["points"]["KeyA"]["x"] = -0.01;
    rejects([&] { action75::PlayerConfig::fromDocuments(base, invalid); }, "Reject negative coordinate");
    invalid = partial; invalid["points"]["KeyA"]["y"] = 1.01;
    rejects([&] { action75::PlayerConfig::fromDocuments(base, invalid); }, "Reject coordinate beyond screen");
    invalid = partial; invalid["points"]["KeyA"]["x"] = "0.5";
    rejects([&] { action75::PlayerConfig::fromDocuments(base, invalid); }, "Reject numeric strings");
    invalid = partial; invalid["points"]["KeyA"]["x"] = std::numeric_limits<double>::infinity();
    rejects([&] { action75::PlayerConfig::fromDocuments(base, invalid); }, "Reject nonfinite coordinate");
    auto badBase = base;
    badBase["keys"][1]["usage"] = badBase["keys"][0]["usage"];
    rejects([&] { action75::PlayerConfig::fromDocuments(badBase, nullptr); }, "Reject duplicate USB usage");
    badBase = base; badBase["keys"][1]["code"] = badBase["keys"][0]["code"];
    rejects([&] { action75::PlayerConfig::fromDocuments(badBase, nullptr); }, "Reject duplicate code");
    badBase = base; badBase["coordinateSpace"] = {1920,720};
    rejects([&] { action75::PlayerConfig::fromDocuments(badBase, nullptr); }, "Reject incorrect source coordinate space");

    auto retained = config;
    std::string error;
    require(!retained.tryReload(root / "__missing_native_core_test_root__", error), "Reload failure must be reported");
    require(!error.empty() && retained.keys.size() == config.keys.size() &&
            retained.findCode("KeyA")->x == config.findCode("KeyA")->x &&
            retained.revision == config.revision, "Invalid reload must preserve last valid mapping");

    // Every real mapped key must have a matching Windows scan translation.
    for (const auto& key : config.keys) {
        bool found = false;
        for (std::uint16_t scan = 1; scan < 128; ++scan) for (const bool ext : {false,true}) {
            const auto translated = action75::translateScan(scan, ext, false, 0);
            if (translated && translated->code == key.code) {
                require(translated->usage == key.usage, "Physical scan translation must retain the same USB usage");
                found = true;
            }
        }
        require(found, "All 83 mapped hardware keys require scan support");
    }
}

void calibrationRecoveryTests(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    const auto parent = fs::canonical(fs::temp_directory_path());
    const auto fixture = parent / ("action75-native-calibration-" + std::to_string(GetCurrentProcessId()));
    require(fs::create_directory(fixture), "Create a fresh calibration test fixture");
    struct Cleanup {
        fs::path fixture, parent;
        ~Cleanup() {
            std::error_code error;
            const auto resolved = fs::canonical(fixture, error);
            if (!error && resolved == fixture && resolved.parent_path() == parent)
                fs::remove_all(resolved, error);
        }
    } cleanup{fixture, parent};

    const auto relativeBase = fs::path("existing-resources/finalmouse/action75-ue-keymap.json");
    const auto relativePreset = fs::path("assets/defaults/fm-calibration.json");
    fs::create_directories((fixture / relativeBase).parent_path());
    fs::create_directories((fixture / relativePreset).parent_path());
    fs::create_directory(fixture / "data");
    fs::copy_file(root / relativeBase, fixture / relativeBase);
    fs::copy_file(root / relativePreset, fixture / relativePreset);
    const auto user = fixture / "data" / "fm-calibration.json";
    const auto preset = action75::PlayerConfig::load(fixture);
    const auto expected = read(fixture / relativePreset);
    require(preset.calibrationError.empty() && preset.revision.empty() && preset.keys.size() == 83 &&
            close(preset.findCode("Escape")->x, expected["points"]["Escape"]["x"].get<double>()),
            "Missing user calibration should use the packaged preset");

    auto prior = expected;
    prior["revision"] = "previous";
    prior["points"]["Escape"]["x"] = 0.92;
    auto retained = action75::PlayerConfig::fromDocuments(read(fixture / relativeBase), prior);
    retained.deviceGroup = "remembered-keyboard";
    for (const auto invalid : {"", "{", "null",
                               R"({"version":1,"points":{"Escape":{"x":0,"y":0},"KeyA":{"x":2,"y":0}}})"}) {
        std::ofstream(user, std::ios::binary) << invalid;
        const auto fallback = action75::PlayerConfig::load(fixture);
        require(!fallback.calibrationError.empty() && fallback.revision.empty() &&
                fallback.keys.size() == preset.keys.size() &&
                fallback.findCode("Escape")->x == preset.findCode("Escape")->x,
                "Damaged user calibration must fall back as a whole");
        std::string error;
        require(!retained.tryReload(fixture, error) && !error.empty() &&
                retained.revision == "previous" && close(retained.findCode("Escape")->x, 0.92) &&
                retained.deviceGroup == "remembered-keyboard",
                "Strict reload preserves the accepted mapping when repair has not been saved");
        std::ifstream stream(user, std::ios::binary);
        const std::string original{std::istreambuf_iterator<char>(stream), {}};
        require(original == invalid, "Loading defaults must preserve the user's original file");
    }

    retained = action75::PlayerConfig::load(fixture);
    retained.deviceGroup = "remembered-keyboard";
    fs::copy_file(fixture / relativePreset, user, fs::copy_options::overwrite_existing);
    std::string error;
    require(retained.tryReload(fixture, error) && retained.revision == expected["revision"].get<std::string>() &&
            retained.calibrationError.empty() && retained.deviceGroup == "remembered-keyboard",
            "Replacing the bad file with valid defaults should clear the recovery notice");
}

void inputTests() {
    using action75::translateScan;
    require(translateScan(29,false,false,VK_CONTROL)->code == "ControlLeft", "Left control scan");
    require(translateScan(29,true,false,VK_CONTROL)->code == "ControlRight", "Right control extended scan");
    require(translateScan(56,false,false,VK_MENU)->usage == 226, "Left Alt usage");
    require(translateScan(56,true,false,VK_MENU)->usage == 230, "Right Alt usage");
    require(translateScan(28,false,false,VK_RETURN)->code == "Enter", "Main Enter");
    require(translateScan(28,true,false,VK_RETURN)->code == "NumpadEnter", "Numpad Enter");
    require(translateScan(72,false,false,VK_NUMPAD8)->code == "Numpad8", "Numpad navigation distinction");
    require(translateScan(72,true,false,VK_UP)->code == "ArrowUp", "Extended navigation");
    require(!translateScan(42,true,false,VK_SHIFT), "Ignore PrintScreen synthetic extended Shift");
    require(!translateScan(29,false,true,VK_CONTROL), "Ignore Pause E1 synthetic Control");
    require(translateScan(69,false,true,VK_PAUSE)->code == "Pause", "Pause special case");
    require(!translateScan(0xFF,false,false,255), "Ignore keyboard overrun record");

    require(action75::matchesDeviceGroup(L"\\\\?\\hid#vid_1ca6&pid_1a0b&mi_01&col01", "VID_1CA6&PID_1A0B"), "USB group matching must be case insensitive");
    require(!action75::matchesDeviceGroup(L"HID#VID_1CA6&PID_1A0C", "VID_1CA6&PID_1A0B"), "Other keyboards must be filtered");
    require(!action75::matchesDeviceGroup(L"HID#VID_1CA6&PID_1A0BB", "VID_1CA6&PID_1A0B"), "USB identifiers must not match longer tokens");
    require(action75::matchesDeviceGroup(L"HID#OTHER", ""), "Empty group explicitly means all keyboards");

    const std::string group = "VID_1CA6&PID_1A0B";
    std::vector<action75::KeyboardDevice> devices{{nullptr, L"HID#VID_1234&PID_ABCD", false}};
    require(!action75::connectedKeyboardGroup(group, devices), "Reject setup when the chosen keyboard is removed");
    devices.push_back({nullptr, L"HID#vid_1ca6&pid_1a0b&mi_02", false});
    require(action75::connectedKeyboardGroup(group, devices), "Accept the same keyboard when it reconnects");
    require(!action75::connectedKeyboardGroup("", devices), "First run requires an explicit selection");

    action75::KeyboardState held;
    require(held.transition(1,4,true), "First key down emits");
    require(!held.transition(1,4,true), "Auto repeat must not emit twice");
    require(!held.transition(2,4,true), "Same usage in second collection remains one logical press");
    require(!held.transition(1,4,false), "Releasing first collection must not release second");
    require(held.transition(2,4,false), "Last collection release emits");
    require(!held.transition(2,4,false), "Duplicate release ignored");
    require(held.transition(1,4,true) && held.transition(1,22,true), "Simultaneous keys preserved");
    require(!held.transition(2,4,true), "Duplicate group press suppressed");
    const auto removed = held.removeDevice(1);
    require(removed.size() == 1 && removed[0] == 22, "Disconnect releases only usages no longer held elsewhere");
    const auto remaining = held.clear();
    require(remaining.size() == 1 && remaining[0] == 4 && held.clear().empty(), "Reset releases each logical key once");
}

void monitorRecoveryTests() {
    using action75::DisplayIdentity;
    using action75::Monitor;
    using action75::resolveMonitor;
    const DisplayIdentity keyboard{L"original-target-interface", L"keyboard-model:1234"};
    const Monitor main{L"DISPLAY1", {0, 0, 2560, 1440}, {}, true,
                       {L"primary-interface", L"main-model:1234"}};
    const Monitor unrelated{L"DISPLAY2", {2560, 0, 4480, 1080}, {}, false,
                            {L"external-interface", L"external-model:4321"}};
    Monitor reconnected{L"DISPLAY6", {-1920, -648, 0, 72}, {}, false, keyboard};
    std::vector<Monitor> current{main, unrelated};
    require(!resolveMonitor(current, keyboard), "A different display reused the old GDI name");
    current.push_back(reconnected);
    require(resolveMonitor(current, keyboard) == &current.back(),
            "The same target must resolve after a GDI name change");
    current.back().identity.path = L"new-port-interface";
    require(resolveMonitor(current, keyboard) == &current.back(),
            "An unambiguous hardware serial must survive a port change");
    require(!resolveMonitor(current, {keyboard.path, {}}),
            "A display with no matching interface or hardware serial requires confirmation");
    current[1].identity = {L"duplicate-interface", keyboard.serial};
    require(!resolveMonitor(current, keyboard), "Duplicate serials must be treated as ambiguous");
    current[1].identity = unrelated.identity;
    current.back().primary = true;
    require(!resolveMonitor(current, keyboard), "Reconnection cannot cover a new primary display");
    current.back().primary = false;
    require(!resolveMonitor(current, {}), "A saved legacy display name cannot establish identity");
    require(resolveMonitor(current, {}, true) == &current.back(), "Find a unique new keyboard display");
    reconnected.identity.path = L"another-keyboard";
    current.push_back(reconnected);
    require(!resolveMonitor(current, {}, true), "Several 1920x720 displays require explicit selection");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto root = argc > 1 ? std::filesystem::u8path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
        configTests(root);
        calibrationRecoveryTests(root);
        inputTests();
        monitorRecoveryTests();
        std::cout << "PASS: calibration, scan translation, device filtering, key state and display identity recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
