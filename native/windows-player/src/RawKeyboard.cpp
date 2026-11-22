#include "RawKeyboard.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <utility>

namespace action75 {
namespace {

struct ScanEntry { std::uint16_t scan; const char* code; std::uint16_t usage; };
constexpr ScanEntry normal[] = {
    {1,"Escape",41},{2,"Digit1",30},{3,"Digit2",31},{4,"Digit3",32},{5,"Digit4",33},
    {6,"Digit5",34},{7,"Digit6",35},{8,"Digit7",36},{9,"Digit8",37},{10,"Digit9",38},
    {11,"Digit0",39},{12,"Minus",45},{13,"Equal",46},{14,"Backspace",42},{15,"Tab",43},
    {16,"KeyQ",20},{17,"KeyW",26},{18,"KeyE",8},{19,"KeyR",21},{20,"KeyT",23},
    {21,"KeyY",28},{22,"KeyU",24},{23,"KeyI",12},{24,"KeyO",18},{25,"KeyP",19},
    {26,"BracketLeft",47},{27,"BracketRight",48},{28,"Enter",40},{29,"ControlLeft",224},
    {30,"KeyA",4},{31,"KeyS",22},{32,"KeyD",7},{33,"KeyF",9},{34,"KeyG",10},
    {35,"KeyH",11},{36,"KeyJ",13},{37,"KeyK",14},{38,"KeyL",15},{39,"Semicolon",51},
    {40,"Quote",52},{41,"Backquote",53},{42,"ShiftLeft",225},{43,"Backslash",49},
    {44,"KeyZ",29},{45,"KeyX",27},{46,"KeyC",6},{47,"KeyV",25},{48,"KeyB",5},
    {49,"KeyN",17},{50,"KeyM",16},{51,"Comma",54},{52,"Period",55},{53,"Slash",56},
    {54,"ShiftRight",229},{55,"NumpadMultiply",85},{56,"AltLeft",226},{57,"Space",44},
    {58,"CapsLock",57},{59,"F1",58},{60,"F2",59},{61,"F3",60},{62,"F4",61},{63,"F5",62},
    {64,"F6",63},{65,"F7",64},{66,"F8",65},{67,"F9",66},{68,"F10",67},{69,"NumLock",83},
    {70,"ScrollLock",71},{71,"Numpad7",95},{72,"Numpad8",96},{73,"Numpad9",97},
    {74,"NumpadSubtract",86},{75,"Numpad4",92},{76,"Numpad5",93},{77,"Numpad6",94},
    {78,"NumpadAdd",87},{79,"Numpad1",89},{80,"Numpad2",90},{81,"Numpad3",91},
    {82,"Numpad0",98},{83,"NumpadDecimal",99},{86,"IntlBackslash",100},{87,"F11",68},{88,"F12",69}
};
constexpr ScanEntry extended[] = {
    {28,"NumpadEnter",88},{29,"ControlRight",228},{53,"NumpadDivide",84},
    {55,"PrintScreen",70},{56,"AltRight",230},{71,"Home",74},{72,"ArrowUp",82},
    {73,"PageUp",75},{75,"ArrowLeft",80},{77,"ArrowRight",79},{79,"End",77},
    {80,"ArrowDown",81},{81,"PageDown",78},{82,"Insert",73},{83,"Delete",76},
    {91,"MetaLeft",227},{92,"MetaRight",231},{93,"ContextMenu",101}
};

template <std::size_t N>
std::optional<ScanKey> lookup(const ScanEntry (&entries)[N], std::uint16_t scan) {
    for (const auto& entry : entries)
        if (entry.scan == scan) return ScanKey{entry.code, entry.usage};
    return std::nullopt;
}

std::wstring devicePath(HANDLE handle) {
    UINT characters = 0;
    if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &characters) == UINT(-1) || !characters)
        return {};
    std::wstring path(characters, L'\0');
    if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, path.data(), &characters) == UINT(-1))
        return {};
    while (!path.empty() && path.back() == L'\0') path.pop_back();
    return path;
}

} // namespace

std::optional<ScanKey> translateScan(std::uint16_t scan, bool isExtended, bool e1,
                                    std::uint16_t virtualKey) {
    if (virtualKey == 255 || scan == 0xFF) return std::nullopt;
    if (virtualKey == VK_PAUSE) return ScanKey{"Pause",72};
    if (virtualKey == VK_SNAPSHOT) return ScanKey{"PrintScreen",70};
    // Pause's E1 sequence must not become a spurious ControlLeft event.
    if (e1) return std::nullopt;
    return isExtended ? lookup(extended, scan) : lookup(normal, scan);
}

bool matchesDeviceGroup(const std::wstring& path, const std::string& group) {
    if (group.empty()) return true;
    std::wstring folded = path;
    std::transform(folded.begin(), folded.end(), folded.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    std::wstring needle(group.begin(), group.end());
    std::transform(needle.begin(), needle.end(), needle.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    const auto found = folded.find(needle);
    if (found == std::wstring::npos) return false;
    const auto end = found + needle.size();
    return end == folded.size() || folded[end] == L'&' || folded[end] == L'#' ||
           folded[end] == L'\\';
}

bool connectedKeyboardGroup(const std::string& group, const std::vector<KeyboardDevice>& devices) {
    return !group.empty() && std::any_of(devices.begin(), devices.end(), [&](const auto& device) {
        return !device.path.empty() && matchesDeviceGroup(device.path, group);
    });
}

bool Action75FnReport::acceptsDevice(const std::wstring& path, const std::string& group) {
    return matchesDeviceGroup(path, "VID_1CA6&PID_1A0B&MI_01&COL02") && matchesDeviceGroup(path, group);
}

bool Action75FnReport::releasedFn(const std::uint8_t* report, std::size_t size) noexcept {
    if (size != ReportBytes || report[0] != 2) return false;
    const bool mediaPressed = report[1] != 0 || report[2] != 0;
    // Action75 emits a neutral Consumer Control report when Fn is released.
    // A neutral report after a nonzero media usage is that media key's release.
    const bool fn = !mediaPressed && !mediaPressed_;
    mediaPressed_ = mediaPressed;
    return fn;
}

bool KeyboardState::transition(std::uintptr_t device, std::uint16_t usage, bool down) {
    if (down) {
        if (!held_[device].insert(usage).second) return false;
        return ++counts_[usage] == 1;
    }
    const auto found = held_.find(device);
    if (found == held_.end() || !found->second.erase(usage)) return false;
    if (found->second.empty()) held_.erase(found);
    const auto count = counts_.find(usage);
    if (count == counts_.end()) return false;
    if (--count->second) return false;
    counts_.erase(count);
    return true;
}

std::vector<std::uint16_t> KeyboardState::removeDevice(std::uintptr_t device) {
    std::vector<std::uint16_t> released;
    const auto found = held_.find(device);
    if (found == held_.end()) return released;
    for (const auto usage : found->second) {
        auto count = counts_.find(usage);
        if (count != counts_.end() && --count->second == 0) {
            released.push_back(usage);
            counts_.erase(count);
        }
    }
    held_.erase(found);
    return released;
}

std::vector<std::uint16_t> KeyboardState::clear() {
    std::vector<std::uint16_t> released;
    released.reserve(counts_.size());
    for (const auto& pair : counts_) released.push_back(pair.first);
    held_.clear();
    counts_.clear();
    return released;
}

RawKeyboard::RawKeyboard(const PlayerConfig& config, Callback callback, DeviceEnumerator devices)
    : config_(config), callback_(std::move(callback)), enumerateDevices_(std::move(devices)) {}

RawKeyboard::~RawKeyboard() { detach(); }

RawKeyboard::Devices RawKeyboard::enumerate(const std::string& group) {
    // A composite keyboard can publish several collections while it connects.
    // Retry if the inventory grows between the size query and the actual read.
    std::vector<RAWINPUTDEVICELIST> raw;
    UINT actual = UINT(-1);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return std::nullopt;
        if (!count) return std::vector<KeyboardDevice>{};
        raw.resize(count);
        actual = GetRawInputDeviceList(raw.data(), &count, sizeof(RAWINPUTDEVICELIST));
        if (actual != UINT(-1)) break;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
    }
    if (actual == UINT(-1)) return std::nullopt;
    std::vector<KeyboardDevice> devices;
    for (UINT index = 0; index < actual; ++index) {
        if (raw[index].dwType != RIM_TYPEKEYBOARD) continue;
        auto path = devicePath(raw[index].hDevice);
        const bool selected = !path.empty() && matchesDeviceGroup(path, group);
        devices.push_back({raw[index].hDevice, std::move(path), selected});
    }
    return devices;
}

bool RawKeyboard::attach(HWND window) {
    if (!window) { error_ = "Raw Input requires a window"; return false; }
    detach();
    const RAWINPUTDEVICE devices[] = {
        {0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
        {0x0C, 0x01, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
    };
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) &&
        !RegisterRawInputDevices(devices, 1, sizeof(RAWINPUTDEVICE))) {
        error_ = "RegisterRawInputDevices failed: " + std::to_string(GetLastError());
        return false;
    }
    window_ = window;
    error_.clear();
    refreshDevices();
    return true;
}

void RawKeyboard::detach() {
    releaseAll();
    if (window_) {
        const RAWINPUTDEVICE devices[] = {
            {0x01, 0x06, RIDEV_REMOVE, nullptr},
            {0x0C, 0x01, RIDEV_REMOVE, nullptr},
        };
        for (const auto& device : devices) RegisterRawInputDevices(&device, 1, sizeof(device));
        window_ = nullptr;
    }
    selected_.clear();
    rejected_.clear();
    fnCollections_.clear();
}

RawKeyboard::Devices RawKeyboard::refreshDevices() {
    auto devices = enumerateDevices_(config_.deviceGroup);
    if (!devices) return std::nullopt;
    std::unordered_set<std::uintptr_t> selected, rejected;
    for (auto& device : *devices) {
        const auto handle = reinterpret_cast<std::uintptr_t>(device.handle);
        // A collection may be listed before its name is available. Keep
        // known classifications and let input retry unresolved collections.
        if (device.path.empty()) device.selected = selected_.count(handle) != 0;
        if (device.selected) selected.insert(handle);
        else if (!device.path.empty() || rejected_.count(handle)) rejected.insert(handle);
    }
    for (const auto handle : selected_)
        if (!selected.count(handle))
            for (const auto usage : state_.removeDevice(handle)) release(usage);
    selected_ = std::move(selected);
    rejected_ = std::move(rejected);
    return devices;
}

void RawKeyboard::release(std::uint16_t usage) {
    if (const auto* key = config_.findUsage(usage)) emit(*key, false);
}

void RawKeyboard::emit(const KeyMapping& key, bool down) {
    if (!callback_) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    callback_({key.code, key.usage, down, key.x, key.y, static_cast<std::uint64_t>(now.QuadPart)});
}

void RawKeyboard::releaseAll() {
    for (const auto usage : state_.clear()) release(usage);
}

void RawKeyboard::setConfig(const PlayerConfig& config) {
    releaseAll();
    if (config_.deviceGroup != config.deviceGroup) {
        selected_.clear();
        rejected_.clear();
        fnCollections_.clear();
    }
    config_ = config;
    if (window_) refreshDevices();
}

void RawKeyboard::processFnReports(HANDLE device, const std::uint8_t* payload, std::size_t size) {
    constexpr auto reportOffset = offsetof(RAWHID, bRawData);
    if (!device || size < reportOffset) return;
    DWORD layout[2];
    std::memcpy(layout, payload, sizeof(layout));
    const auto [reportSize, reportCount] = layout;
    if (reportSize != Action75FnReport::ReportBytes || !reportCount ||
        reportCount > (size - reportOffset) / reportSize)
        return;

    auto found = fnCollections_.find(device);
    if (found == fnCollections_.end()) {
        if (!Action75FnReport::acceptsDevice(devicePath(device), config_.deviceGroup)) return;
        found = fnCollections_.try_emplace(device).first;
    }
    ++diagnostics_.selectedPackets;
    auto report = payload + reportOffset;
    for (DWORD index = 0; index < reportCount; ++index, report += reportSize) {
        if (!found->second.releasedFn(report, reportSize)) continue;
        if (const auto key = config_.fnKey()) {
            ++diagnostics_.downEvents;
            emit(*key, true);
            ++diagnostics_.upEvents;
            emit(*key, false);
        }
    }
}

bool RawKeyboard::process(UINT message, WPARAM wParam, LPARAM lParam) {
    if (!window_) return false;
    if (message == WM_INPUT_DEVICE_CHANGE) {
        if (wParam == GIDC_REMOVAL) {
            const auto handle = static_cast<std::uintptr_t>(lParam);
            for (const auto usage : state_.removeDevice(handle)) release(usage);
            selected_.erase(handle);
            rejected_.erase(handle);
            fnCollections_.erase(reinterpret_cast<HANDLE>(lParam));
        } else if (wParam == GIDC_ARRIVAL) refreshDevices();
        return true;
    }
    if (message != WM_INPUT) return false;
    ++diagnostics_.packets;
    diagnostics_.lastPacketMs = GetTickCount64();
    // A single keyboard or Consumer Control report fits in the stack buffer.
    // Windows can deliver multiple HID reports in the same message.
    const auto handle = reinterpret_cast<HRAWINPUT>(lParam);
    alignas(RAWINPUT) std::array<std::uint8_t, sizeof(RAWINPUT)> packet;
    std::vector<std::uint8_t> batch;
    auto* data = packet.data();
    UINT capacity = static_cast<UINT>(packet.size());
    UINT copied = GetRawInputData(handle, RID_INPUT, data, &capacity, sizeof(RAWINPUTHEADER));
    if (copied == UINT(-1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        capacity = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &capacity, sizeof(RAWINPUTHEADER)) == 0 &&
            capacity > packet.size() && capacity <= 64 * 1024) {
            batch.resize(capacity);
            data = batch.data();
            copied = GetRawInputData(handle, RID_INPUT, data, &capacity, sizeof(RAWINPUTHEADER));
        }
    }
    constexpr auto payloadOffset = offsetof(RAWINPUT, data);
    const auto bufferSize = batch.empty() ? packet.size() : batch.size();
    if (copied == UINT(-1) || copied > bufferSize || copied < payloadOffset) {
        ++diagnostics_.readErrors;
        return true;
    }
    RAWINPUTHEADER header;
    std::memcpy(&header, data, sizeof(header));
    const auto* payload = data + payloadOffset;
    const auto payloadBytes = copied - payloadOffset;
    if (header.dwType == RIM_TYPEHID) {
        processFnReports(header.hDevice, payload, payloadBytes);
        return true;
    }
    if (header.dwType != RIM_TYPEKEYBOARD || payloadBytes < sizeof(RAWKEYBOARD)) {
        ++diagnostics_.readErrors;
        return true;
    }
    const auto device = reinterpret_cast<std::uintptr_t>(header.hDevice);
    if (!selected_.count(device)) {
        if (!device || rejected_.count(device)) return true;
        const auto path = devicePath(header.hDevice);
        if (path.empty()) return true;
        if (!matchesDeviceGroup(path, config_.deviceGroup)) {
            rejected_.insert(device);
            return true;
        }
        selected_.insert(device);
    }
    ++diagnostics_.selectedPackets;
    RAWKEYBOARD input;
    std::memcpy(&input, payload, sizeof(input));
    auto scan = input.MakeCode;
    if (!scan) scan = static_cast<USHORT>(MapVirtualKeyW(input.VKey, MAPVK_VK_TO_VSC));
    const auto flags = input.Flags;
    const auto key = translateScan(scan, (flags & RI_KEY_E0) != 0,
                                    (flags & RI_KEY_E1) != 0, input.VKey);
    const auto* mapping = key ? config_.findUsage(key->usage) : nullptr;
    if (!mapping) return true;
    const bool down = (flags & RI_KEY_BREAK) == 0;
    if (state_.transition(device, key->usage, down)) {
        if (down) ++diagnostics_.downEvents;
        else ++diagnostics_.upEvents;
        emit(*mapping, down);
    }
    return true;
}

} // namespace action75
