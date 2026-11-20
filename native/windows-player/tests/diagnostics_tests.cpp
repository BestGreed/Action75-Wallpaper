#include "Diagnostics.h"
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

static void put(const fs::path &path, const std::string &value) {
  std::ofstream output(path, std::ios::binary);
  output << value;
  require(output.good(), "Could not write test fixture");
}

static std::string read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Fixture {
  fs::path root = fs::temp_directory_path() /
                  (L"action75-diagnostics-" + std::to_wstring(GetCurrentProcessId()) +
                   L"-" + std::to_wstring(GetTickCount64()));
  Fixture() { require(fs::create_directory(root), "Fixture must be new"); }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

int main() {
  try {
    Fixture fixture;
    const auto logs = fixture.root / L"shared-logs";
    fs::create_directories(logs);
    fs::create_directories(fixture.root / L"data");
    put(fixture.root / L"data/app-settings.json",
        nlohmann::json{{"logDir", logs.u8string()}}.dump());

    const std::vector<std::wstring> unrelated{
        L"manager-sync.log", L"manager-other.log", L"playerbackup.log",
        L"player-backup.log", L"manager-.log", L"manager-12x.log",
        L"player-10.log.bak", L"Manager-123.log", L"player--10.log",
        L"player-１２.log"};
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(100);
    for (const auto &name : unrelated) {
      put(logs / name, "unrelated bytes");
      fs::last_write_time(logs / name, old);
    }
    fs::create_directory(logs / L"manager-999.log");
    put(logs / L"manager-999.log/keep.txt", "unrelated directory");
    put(logs / L"manager.log", "manager active");
    fs::last_write_time(logs / L"manager.log", old);
    put(logs / L"player.log", "");
    fs::resize_file(logs / L"player.log", 10u * 1024u * 1024u);
    std::vector<fs::path> history;
    for (int i = 0; i < 12; ++i) {
      const auto path = logs / ((i % 2 ? L"manager-" : L"player-") +
                                std::to_wstring(i) + L".log");
      put(path, "owned history");
      fs::last_write_time(path, old + std::chrono::hours(20 - i));
      history.push_back(path);
    }
    const auto outside = fixture.root / L"other-application.log";
    put(outside, "outside log bytes");
    const auto link = logs / L"manager-123456.log";
    const bool linked = CreateSymbolicLinkW(link.c_str(), outside.c_str(),
                                           SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;

    action75::Diagnostics diagnostics;
    diagnostics.initialize(fixture.root);
    require(fs::file_size(logs / L"player.log") < 1024, "Active player log did not rotate");
    require(read(logs / L"manager.log") == "manager active", "Active manager log changed");
    for (const auto &name : unrelated)
      require(read(logs / name) == "unrelated bytes", "Unrelated log was removed or changed");
    require(read(logs / L"manager-999.log/keep.txt") == "unrelated directory",
            "Log-shaped directory changed");
    for (int i = 0; i < 12; ++i)
      require(fs::exists(history[i]) == (i < 7), "Shared retention did not remove only five oldest owned files");
    std::size_t entries = 0;
    for (const auto &entry : fs::directory_iterator(logs)) {
      (void)entry;
      ++entries;
    }
    require(entries == 10 + unrelated.size() + 1 + (linked ? 1 : 0),
            "Unrelated entries affected the ten-file budget");
    require(read(outside) == "outside log bytes", "Link target changed");
    if (linked) {
      require(fs::is_symlink(fs::symlink_status(link)), "Log-shaped link was removed");
      fs::remove(logs / L"player.log");
      require(CreateSymbolicLinkW((logs / L"player.log").c_str(), outside.c_str(),
                                 SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0,
              "Could not create active-log link fixture");
      diagnostics.write("INFO", "must not append through link");
      require(read(outside) == "outside log bytes", "Active log followed a link");
      std::cout << "PASS: log-shaped reparse files and active-log links remain untouched\n";
    } else {
      std::cout << "SKIP: real symlink fixture requires Windows developer mode or symbolic-link privilege\n";
    }
    std::cout << "PASS: actual custom-directory rotation preserves unrelated files and directories; shared budget is ten\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
