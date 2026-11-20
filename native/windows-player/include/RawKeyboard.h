#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "PlayerConfig.h"
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace action75 {

struct KeyEvent {
    std::string code;
    std::uint16_t usage = 0;
    bool down = false;
    float x = 0;
    float y = 0;
    std::uint64_t timestamp = 0; // QueryPerformanceCounter ticks.
};

struct KeyboardDevice {
    HANDLE handle = nullptr;
    std::wstring path;
    bool selected = false;
};

struct ScanKey {
    std::string code;
    std::uint16_t usage = 0;
};

std::optional<ScanKey> translateScan(std::uint16_t scan, bool extended, bool e1,
                                    std::uint16_t virtualKey);
bool matchesDeviceGroup(const std::wstring& path, const std::string& group);
bool connectedKeyboardGroup(const std::string& group, const std::vector<KeyboardDevice>& devices);

// Only pressed USB usages are retained. No text, key history or injected events.
// Return true only when the union across selected device collections changes.
class KeyboardState {
public:
    bool transition(std::uintptr_t device, std::uint16_t usage, bool down);
    std::vector<std::uint16_t> removeDevice(std::uintptr_t device);
    std::vector<std::uint16_t> clear();
private:
    std::unordered_map<std::uintptr_t, std::unordered_set<std::uint16_t>> held_;
    std::unordered_map<std::uint16_t, unsigned> counts_;
};

// Construct, attach, process and setConfig on the window's message-loop thread.
// The owner must still call DefWindowProc for WM_INPUT foreground cleanup.
class RawKeyboard {
public:
    // Aggregate diagnostics only; no key values, text, or event history.
    struct Diagnostics {
        std::uint64_t packets = 0, selectedPackets = 0, readErrors = 0;
        std::uint64_t downEvents = 0, upEvents = 0, lastPacketMs = 0;
    };
    using Callback = std::function<void(const KeyEvent&)>;
    RawKeyboard(const PlayerConfig& config, Callback callback);
    ~RawKeyboard();
    RawKeyboard(const RawKeyboard&) = delete;
    RawKeyboard& operator=(const RawKeyboard&) = delete;

    bool attach(HWND window);
    void detach();
    bool process(UINT message, WPARAM wParam, LPARAM lParam);
    void setConfig(const PlayerConfig& config);
    const std::string& lastError() const noexcept { return error_; }
    const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    std::size_t selectedDeviceCount() const noexcept { return selected_.size(); }
    bool registered() const noexcept { return window_ != nullptr; }
    static std::vector<KeyboardDevice> enumerate(const std::string& group = "VID_1CA6&PID_1A0B");

private:
    void refreshDevices();
    void release(std::uint16_t usage);
    void emit(const KeyMapping& key, bool down);
    void releaseAll();
    PlayerConfig config_;
    Callback callback_;
    HWND window_ = nullptr;
    std::unordered_set<std::uintptr_t> selected_;
    KeyboardState state_;
    std::string error_;
    Diagnostics diagnostics_;
};

} // namespace action75
