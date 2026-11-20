#pragma once
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <windows.h>

namespace action75 {
// Use the same Windows directory identity as the manager. A path alias still
// refers to the same installation, while a copied directory gets its own pipe.
inline std::wstring installationEndpoint(const std::filesystem::path &root) {
  const auto attributes = GetFileAttributesW(root.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    throw std::runtime_error("Invalid player runtime directory");
  const auto directory = CreateFileW(root.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (directory == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot open player runtime directory");
  FILE_ID_INFO id{};
  BY_HANDLE_FILE_INFORMATION legacy{};
  const bool identified = GetFileInformationByHandleEx(directory, FileIdInfo, &id, sizeof(id)) != FALSE;
  const bool legacyIdentified = !identified && GetFileInformationByHandle(directory, &legacy) != FALSE;
  CloseHandle(directory);

  std::wostringstream name;
  name.imbue(std::locale::classic());
  name << std::hex << std::setfill(L'0');
  if (identified) {
    name << L"Action75Root-" << std::setw(16) << id.VolumeSerialNumber << L'-';
    for (const auto byte : id.FileId.Identifier)
      name << std::setw(2) << static_cast<unsigned>(byte);
  } else if (legacyIdentified) {
    name << L"Action75Root-legacy-" << std::setw(8) << legacy.dwVolumeSerialNumber << L'-'
         << std::setw(8) << legacy.nFileIndexHigh << std::setw(8) << legacy.nFileIndexLow;
  } else {
    throw std::runtime_error("Cannot identify player runtime directory");
  }
  return name.str();
}

inline bool matchesRuntimeRoot(const std::filesystem::path &root,
                               const std::wstring &endpoint) {
  try {
    return installationEndpoint(root) == endpoint;
  } catch (const std::exception &) {
    return false;
  }
}
} // namespace action75
