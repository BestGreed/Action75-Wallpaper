#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>
#include <windows.h>

namespace action75 {
inline std::filesystem::path
configuredDirectory(const std::filesystem::path &root, const char *key,
                    const wchar_t *fallback) {
  const auto file = root / L"data/app-settings.json";
  if (std::filesystem::exists(file)) {
    std::ifstream input(file);
    const auto value = nlohmann::json::parse(input);
    const auto path = value.value(key, std::string());
    if (!path.empty())
      return std::filesystem::u8path(path);
  }
  return root / fallback;
}
class Diagnostics {
  std::filesystem::path root_;
  bool suspended_ = false;

  static bool ownsLogName(std::wstring_view name) {
    if (name == L"manager.log" || name == L"player.log")
      return true;
    const auto prefix = name.rfind(L"manager-", 0) == 0 ? 8u
                        : name.rfind(L"player-", 0) == 0 ? 7u : 0u;
    if (!prefix || name.size() <= prefix + 4 ||
        name.substr(name.size() - 4) != L".log")
      return false;
    const auto stamp = name.substr(prefix, name.size() - prefix - 4);
    return std::all_of(stamp.begin(), stamp.end(),
                       [](wchar_t c) { return c >= L'0' && c <= L'9'; });
  }

  static bool ownsRegularLog(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return ownsLogName(path.filename().native()) &&
           attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           std::filesystem::is_regular_file(std::filesystem::symlink_status(path));
  }

public:
  void initialize(const std::filesystem::path &root) {
    root_ = root;
    write("INFO", "Player started");
  }
  void suspend(bool value) { suspended_ = value; }
  void write(const char *level, const std::string &message) noexcept {
    if (suspended_ || root_.empty())
      return;
    try {
      namespace fs = std::filesystem;
      const auto dir = configuredDirectory(root_, "logDir", L"logs");
      fs::create_directories(dir);
      const auto stamp =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      const auto record =
          std::to_string(stamp) + " " + level + " " + message + "\n";
      const auto file = dir / L"player.log";
      // A user-selected directory can contain other applications' files or links.
      // Never rotate or append through a link using our active log's name.
      if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
          !ownsRegularLog(file))
        return;
      if (fs::exists(file) &&
          fs::file_size(file) + record.size() > 10u * 1024u * 1024u)
        fs::rename(file, dir / (L"player-" + std::to_wstring(stamp) + L".log"));
      {
        std::ofstream output(file, std::ios::app);
        output << record;
      }
      std::vector<fs::directory_entry> files;
      for (const auto &entry : fs::directory_iterator(dir)) {
        if (ownsRegularLog(entry.path()))
          files.push_back(entry);
      }
      std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
        return a.last_write_time() < b.last_write_time();
      });
      auto count = files.size();
      for (const auto &entry : files) {
        if (count <= 10)
          break;
        const auto name = entry.path().filename();
        if (name != L"player.log" && name != L"manager.log" &&
            ownsRegularLog(entry.path())) {
          fs::remove(entry);
          --count;
        }
      }
    } catch (...) {
      OutputDebugStringA("ACTION75: diagnostic log could not be written\n");
    }
  }
};
} // namespace action75
