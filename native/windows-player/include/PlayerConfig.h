#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace action75 {

struct KeyMapping {
    std::string code;
    std::uint16_t usage = 0;
    float x = 0;
    float y = 0;
};

// Coordinates are already normalized to the final screen. Do not apply the
// legacy Studio area transform a second time. This reader never writes config.
struct PlayerConfig {
    std::vector<KeyMapping> keys;
    std::string revision;
    std::string calibrationError;
    std::string deviceGroup = "VID_1CA6&PID_1A0B";

    static PlayerConfig load(const std::filesystem::path& projectRoot);
    static PlayerConfig fromDocuments(const nlohmann::json& defaults,
                                     const nlohmann::json& calibration);
    bool tryReload(const std::filesystem::path& projectRoot, std::string& error) noexcept;
    const KeyMapping* findCode(std::string_view code) const noexcept;
    const KeyMapping* findUsage(std::uint16_t usage) const noexcept;
    std::optional<KeyMapping> fnKey() const;
};

} // namespace action75
