#include "PlayerConfig.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace action75 {
namespace {

nlohmann::json readDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read configuration: " + path.u8string());
    // nlohmann accepts an optional UTF-8 BOM, as the existing JS reader does.
    return nlohmann::json::parse(stream);
}

bool validCode(const std::string& code) {
    if (code.empty() || code.size() > 32) return false;
    return std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

double coordinate(const nlohmann::json& object, const char* name, double maximum) {
    if (!object.contains(name) || !object[name].is_number())
        throw std::runtime_error(std::string("Missing or nonnumeric coordinate: ") + name);
    const double value = object[name].get<double>();
    if (!std::isfinite(value) || value < 0 || value > maximum)
        throw std::runtime_error(std::string("Coordinate out of bounds: ") + name);
    return value;
}

void applyCalibration(PlayerConfig& config, const nlohmann::json& calibration) {
    if (!calibration.is_object() || !calibration.contains("version") ||
        calibration["version"] != 1 || !calibration.contains("points") ||
        !calibration["points"].is_object())
        throw std::runtime_error("Invalid calibration format");
    for (auto it = calibration["points"].begin(); it != calibration["points"].end(); ++it) {
        auto key = std::find_if(config.keys.begin(), config.keys.end(),
                                [&](const KeyMapping& k) { return k.code == it.key(); });
        if (key == config.keys.end() || !it.value().is_object())
            throw std::runtime_error("Unknown key in calibration");
        key->x = static_cast<float>(coordinate(it.value(), "x", 1));
        key->y = static_cast<float>(coordinate(it.value(), "y", 1));
    }
    if (calibration.contains("revision") && calibration["revision"].is_string())
        config.revision = calibration["revision"].get<std::string>();
}

PlayerConfig loadConfig(const std::filesystem::path& root, bool recoverUserCalibration) {
    auto base = PlayerConfig::fromDocuments(
        readDocument(root / "existing-resources" / "finalmouse" / "action75-ue-keymap.json"),
        nullptr);
    const auto user = root / "data" / "fm-calibration.json";
    std::string calibrationError;
    try {
        if (std::filesystem::exists(user)) {
            auto candidate = base;
            applyCalibration(candidate, readDocument(user));
            return candidate;
        }
    } catch (const std::exception& exception) {
        if (!recoverUserCalibration) throw;
        calibrationError = exception.what();
    }

    const auto preset = root / "assets" / "defaults" / "fm-calibration.json";
    if (std::filesystem::exists(preset)) applyCalibration(base, readDocument(preset));
    // A revision identifies an accepted user calibration in the reload handshake.
    base.revision.clear();
    base.calibrationError = std::move(calibrationError);
    return base;
}

} // namespace

PlayerConfig PlayerConfig::load(const std::filesystem::path& projectRoot) {
    return loadConfig(projectRoot, true);
}

PlayerConfig PlayerConfig::fromDocuments(const nlohmann::json& defaults,
                                        const nlohmann::json& calibration) {
    if (!defaults.is_object() || !defaults.contains("coordinateSpace") ||
        defaults["coordinateSpace"] != nlohmann::json::array({1920, 555}) ||
        !defaults.contains("keys") || !defaults["keys"].is_array() ||
        defaults["keys"].empty() || defaults["keys"].size() > 100)
        throw std::runtime_error("Invalid base keymap");

    PlayerConfig result;
    std::unordered_set<std::string> codes;
    std::unordered_set<std::uint16_t> usages;
    for (const auto& key : defaults["keys"]) {
        if (!key.is_object() || !key.contains("code") || !key["code"].is_string() ||
            !key.contains("usage") || !key["usage"].is_number_integer())
            throw std::runtime_error("Invalid key definition");
        const auto code = key["code"].get<std::string>();
        const auto usage = key["usage"].get<std::int64_t>();
        if (!validCode(code) || usage < 1 || usage > 255 || !codes.insert(code).second ||
            !usages.insert(static_cast<std::uint16_t>(usage)).second)
            throw std::runtime_error("Invalid or duplicate key code / USB usage");
        result.keys.push_back({code, static_cast<std::uint16_t>(usage),
                               static_cast<float>(coordinate(key, "x", 1920) / 1920),
                               static_cast<float>(coordinate(key, "y", 555) / 555)});
    }

    if (!calibration.is_null()) applyCalibration(result, calibration);
    return result;
}

bool PlayerConfig::tryReload(const std::filesystem::path& projectRoot, std::string& error) noexcept {
    try {
        auto candidate = loadConfig(projectRoot, false);
        // This file only describes calibration. Preserve the host's selected
        // device filter when live-reloading coordinates.
        candidate.deviceGroup = deviceGroup;
        *this = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "Unknown configuration error";
        return false;
    }
}

const KeyMapping* PlayerConfig::findCode(std::string_view code) const noexcept {
    const auto it = std::find_if(keys.begin(), keys.end(),
                                 [&](const KeyMapping& key) { return key.code == code; });
    return it == keys.end() ? nullptr : &*it;
}

const KeyMapping* PlayerConfig::findUsage(std::uint16_t usage) const noexcept {
    const auto it = std::find_if(keys.begin(), keys.end(),
                                 [&](const KeyMapping& key) { return key.usage == usage; });
    return it == keys.end() ? nullptr : &*it;
}

std::optional<KeyMapping> PlayerConfig::fnKey() const {
    const auto* alt = findCode("AltRight");
    const auto* ctrl = findCode("ControlRight");
    if (!alt || !ctrl) return std::nullopt;
    // Fn sits between the two calibrated keys and has no Keyboard/Keypad usage.
    return KeyMapping{"Fn", 0, (alt->x + ctrl->x) * .5f, (alt->y + ctrl->y) * .5f};
}

} // namespace action75
