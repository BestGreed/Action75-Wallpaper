#include "NativePluginSession.h"
#include "NativePluginWire.h"
#include <userenv.h>
#include <sddl.h>
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <new>
#include <sstream>
#include <string_view>
#include <vector>
#include <cstring>
#include <cwctype>
#include <stdexcept>

namespace action75 {
namespace fs = std::filesystem;
using namespace worker;
namespace {
void check(bool ok, const char *operation) {
  if (!ok)
    throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(GetLastError()) +
                             ")");
}
void checkHr(HRESULT hr, const char *operation) {
  if (FAILED(hr))
    throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(hr) + ")");
}
std::wstring hostFile() {
  std::wstring path(32768, 0);
  auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  check(length && length < path.size(), "Locate player");
  path.resize(length);
  return path;
}
struct LocalMemory {
  HLOCAL value = nullptr;
  ~LocalMemory() {
    if (value)
      LocalFree(value);
  }
};
struct Attributes {
  LPPROC_THREAD_ATTRIBUTE_LIST list;
  std::vector<char> storage;
  Attributes() {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    storage.resize(size);
    list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(list, 2, 0, &size), "Allocate worker attributes");
  }
  ~Attributes() { DeleteProcThreadAttributeList(list); }
};
void textField(char *dest, size_t size, const char *source) {
  size_t n = source ? strnlen_s(source, size) : 0;
  if (n >= size)
    throw std::runtime_error("Wallpaper IPC string exceeds its limit");
  if (n)
    memcpy(dest, source, n);
  dest[n] = 0;
}
// A versioned import can exceed MAX_PATH even when the author's original
// folder is short. Use one path representation for enumeration and file opens.
fs::path extendedPath(const fs::path &path) {
  auto name = fs::absolute(path).lexically_normal().make_preferred().wstring();
  if (name.rfind(LR"(\\?\)", 0) == 0) return name;
  if (name.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + name.substr(2);
  return LR"(\\?\)" + name;
}
bool ordinaryPath(const fs::path &path) {
  DWORD flags = GetFileAttributesW(path.c_str());
  return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
}
void copyNewFile(const fs::path &source, const fs::path &dest) {
  Handle in(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  check(in.value != INVALID_HANDLE_VALUE, "Open staged source");
  BY_HANDLE_FILE_INFORMATION info{};
  check(GetFileInformationByHandle(in.value, &info), "Inspect staged source");
  if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
    throw std::runtime_error("Unsupported staged file");
  Handle out(CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                         nullptr));
  check(out.value != INVALID_HANDLE_VALUE, "Create staged file");
  char buffer[65536];
  DWORD n = 0, written = 0;
  for (;;) {
    check(ReadFile(in.value, buffer, sizeof(buffer), &n, nullptr), "Read staged source");
    if (!n)
      break;
    check(WriteFile(out.value, buffer, n, &written, nullptr) && written == n, "Write staged file");
  }
}
void copyPack(const fs::path &root, const fs::path &destination) {
  check(ordinaryPath(root), "Invalid wallpaper directory");
  fs::create_directory(destination);
  size_t count = 0;
  uintmax_t bytes = 0;
  for (const auto &item : fs::recursive_directory_iterator(root)) {
    if (++count > 512 || !ordinaryPath(item.path()))
      throw std::runtime_error("Unsupported wallpaper tree");
    auto target = destination / item.path().lexically_relative(root);
    if (item.is_directory())
      fs::create_directory(target);
    else {
      if (!item.is_regular_file() || (bytes += item.file_size()) > (128ull << 20))
        throw std::runtime_error("Unsupported wallpaper file");
      // A new file inherits the private staging ACL. Never preserve source permissions.
      copyNewFile(item.path(), target);
    }
  }
}
std::wstring idString() {
  GUID guid{};
  checkHr(CoCreateGuid(&guid), "Allocate worker identity");
  wchar_t buffer[64]{};
  StringFromGUID2(guid, buffer, 64);
  return std::wstring(buffer + 1, 36);
}
constexpr std::wstring_view StagePrefix = L"Action75-worker-";
constexpr std::wstring_view CleanupSuffix = L".cleanup";
constexpr char CleanupRecord[] = "Action75WorkerCleanupV1";
constexpr char PendingExitTag[] = "Action75WorkerCleanupV2";

struct PendingExitRecord {
  char tag[sizeof(PendingExitTag)];
  DWORD pid;
  FILETIME created;
};

void cleanupDiagnostic(const char *operation, unsigned long error) noexcept {
  char text[160]{};
  std::snprintf(text, sizeof(text), "Action75 Worker cleanup: %s (0x%08lX)\n", operation, error);
  OutputDebugStringA(text);
}

DWORD removeStage(const fs::path &stage) {
  if (stage.empty())
    return ERROR_SUCCESS;
  const auto path = extendedPath(stage);
  const auto flags = GetFileAttributesW(path.c_str());
  if (flags == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_SUCCESS : error;
  }
  if (!(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT))
    return ERROR_INVALID_NAME;
  std::error_code error;
  fs::remove_all(path, error);
  return static_cast<DWORD>(error.value());
}

std::wstring_view cleanupIdentity(std::wstring_view fileName) noexcept {
  constexpr size_t GuidSize = 36;
  if (fileName.size() != StagePrefix.size() + GuidSize + CleanupSuffix.size() ||
      fileName.substr(0, StagePrefix.size()) != StagePrefix ||
      fileName.substr(StagePrefix.size() + GuidSize) != CleanupSuffix)
    return {};
  const auto identity = fileName.substr(StagePrefix.size(), GuidSize);
  for (size_t i = 0; i < identity.size(); ++i) {
    const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
    if (separator ? identity[i] != L'-' : !std::iswxdigit(identity[i]))
      return {};
  }
  return identity;
}

bool cleanupReady(const char *bytes, DWORD size) noexcept {
  if (size == sizeof(CleanupRecord) - 1)
    return std::memcmp(bytes, CleanupRecord, size) == 0;
  PendingExitRecord record{};
  if (size != sizeof(record))
    return false;
  std::memcpy(&record, bytes, sizeof(record));
  if (std::memcmp(record.tag, PendingExitTag, sizeof(PendingExitTag)) != 0 || !record.pid)
    return false;
  Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, record.pid));
  if (!process.value)
    return GetLastError() == ERROR_INVALID_PARAMETER;
  if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
    return true;
  // A recycled PID does not identify the Worker that owned this stage.
  FILETIME created{}, exit{}, kernel{}, user{};
  return GetProcessTimes(process.value, &created, &exit, &kernel, &user) &&
         CompareFileTime(&created, &record.created) != 0;
}

bool profileRemoved(HRESULT result) noexcept {
  return SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
         result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

// Unmarked directories may still be in use by another player instance.
void retryRecordedCleanup(const fs::path &temp) {
  std::error_code ec;
  unsigned attempted = 0;
  for (fs::directory_iterator it(temp, ec), end; !ec && it != end && attempted < 16;
       it.increment(ec)) {
    const auto name = it->path().filename().wstring();
    const auto identity = cleanupIdentity(name);
    if (identity.empty())
      continue;
    auto path = extendedPath(it->path());
    Handle marker(CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (marker.value == INVALID_HANDLE_VALUE)
      continue;
    BY_HANDLE_FILE_INFORMATION attributes{};
    char record[sizeof(PendingExitRecord) + 1]{};
    DWORD bytes = 0;
    if (!GetFileInformationByHandle(marker.value, &attributes) ||
        (attributes.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !ReadFile(marker.value, record, sizeof(record), &bytes, nullptr) ||
        !cleanupReady(record, bytes))
      continue;
    ++attempted;
    const auto failure = removeStage(temp / (std::wstring(StagePrefix) + std::wstring(identity)));
    const auto profile = L"Action75.Wallpaper." + std::wstring(identity);
    const auto result = DeleteAppContainerProfile(profile.c_str());
    if (!failure && profileRemoved(result)) {
      FILE_DISPOSITION_INFO disposition{TRUE};
      if (!SetFileInformationByHandle(marker.value, FileDispositionInfo, &disposition,
                                      sizeof(disposition)))
        cleanupDiagnostic("remove pending record", GetLastError());
    } else {
      cleanupDiagnostic("retry pending cleanup", failure ? failure : static_cast<DWORD>(result));
    }
  }
}

void recordPendingCleanup(const fs::path &stage, HANDLE awaitingExit = nullptr) {
  if (stage.empty())
    return;
  const void *record = CleanupRecord;
  DWORD size = sizeof(CleanupRecord) - 1;
  PendingExitRecord pending{};
  if (awaitingExit) {
    pending.pid = GetProcessId(awaitingExit);
    FILETIME exit{}, kernel{}, user{};
    if (!pending.pid || !GetProcessTimes(awaitingExit, &pending.created, &exit, &kernel, &user)) {
      cleanupDiagnostic("identify pending Worker", GetLastError());
      return;
    }
    std::memcpy(pending.tag, PendingExitTag, sizeof(PendingExitTag));
    record = &pending;
    size = sizeof(pending);
  }
  auto path = extendedPath(stage);
  path += CleanupSuffix;
  Handle marker(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (marker.value == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error != ERROR_FILE_EXISTS)
      cleanupDiagnostic("record pending cleanup", error);
    return;
  }
  DWORD bytes = 0;
  if (!WriteFile(marker.value, record, size, &bytes, nullptr) || bytes != size)
    cleanupDiagnostic("record pending cleanup", GetLastError());
}

bool cleanExitedWorker(const fs::path &stage, const wchar_t *profile) noexcept {
  try {
    DWORD removal = removeStage(stage);
    constexpr DWORD delays[]{20, 50, 100};
    for (const auto delay : delays) {
      if (removal != ERROR_SHARING_VIOLATION && removal != ERROR_LOCK_VIOLATION &&
          removal != ERROR_ACCESS_DENIED && removal != ERROR_DIR_NOT_EMPTY)
        break;
      Sleep(delay);
      removal = removeStage(stage);
    }
    const auto result = profile ? DeleteAppContainerProfile(profile) : S_OK;
    if (!removal && profileRemoved(result))
      return true;
    cleanupDiagnostic("clear exited Worker", removal ? removal : static_cast<DWORD>(result));
    recordPendingCleanup(stage);
  } catch (...) {
    OutputDebugStringA("Action75 Worker cleanup: could not prepare cleanup paths\n");
  }
  return false;
}

struct DeferredWorkerCleanup {
  fs::path stage;
  std::wstring profile;
  Handle process;

  static void CALLBACK finish(PTP_CALLBACK_INSTANCE callback, void *context,
                              PTP_WAIT wait, TP_WAIT_RESULT result) noexcept {
    std::unique_ptr<DeferredWorkerCleanup> cleanup(static_cast<DeferredWorkerCleanup *>(context));
    SetThreadpoolWait(wait, nullptr, nullptr);
    CloseThreadpoolWait(wait);
    if (result != WAIT_OBJECT_0)
      return;
    CallbackMayRunLong(callback);
    const bool cleaned = cleanExitedWorker(cleanup->stage, cleanup->profile.empty() ? nullptr : cleanup->profile.c_str());
    if (cleaned && !cleanup->stage.empty()) {
      try {
        auto marker = extendedPath(cleanup->stage);
        marker += CleanupSuffix;
        std::error_code error;
        fs::remove(marker, error);
        if (error)
          cleanupDiagnostic("remove completed cleanup record", error.value());
      } catch (...) {
        OutputDebugStringA("Action75 Worker cleanup: could not remove completed cleanup record\n");
      }
    }
  }
};

void deferWorkerCleanup(Handle &process, fs::path &stage, std::wstring &profile) noexcept {
  try {
    // The next player can retry if this one exits before the callback runs.
    recordPendingCleanup(stage, process.value);
  } catch (...) {
    OutputDebugStringA("Action75 Worker cleanup: could not record running Worker\n");
  }
  std::unique_ptr<DeferredWorkerCleanup> cleanup(new (std::nothrow) DeferredWorkerCleanup);
  if (!cleanup)
    return;
  const auto wait = CreateThreadpoolWait(DeferredWorkerCleanup::finish, cleanup.get(), nullptr);
  if (!wait) {
    cleanupDiagnostic("schedule Worker cleanup", GetLastError());
    return;
  }
  cleanup->stage = std::move(stage);
  cleanup->profile = std::move(profile);
  cleanup->process.value = std::exchange(process.value, nullptr);
  const auto handle = cleanup.release()->process.value;
  SetThreadpoolWait(wait, handle, nullptr);
}
} // namespace
struct NativePluginSession::Impl {
  fs::path stage;
  std::wstring profile;
  PSID sid = nullptr;
  HDESK desktop = nullptr;
  Handle process, job, request, response, mapping, shared, sharedFence;
  bool composition = false;
  ComPtr<ID3D12Resource> surface12;
  ComPtr<ID3D12Fence> fence12;
  UINT64 handoff=0;
  Mailbox *msg = nullptr;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Texture2D> surface;
  ComPtr<IDXGIKeyedMutex> mutex;
  unsigned width = 0, height = 0;
  bool hasHitTest = true;
  std::string patch;
  ~Impl() {
    if (process.value) {
      if (msg && request.value) {
        msg->operation = Operation::Exit;
        SetEvent(request.value);
      }
      // A healthy D3D12 Worker can spend longer in driver/DLL teardown than
      // it spends in a normal event callback.
      if (WaitForSingleObject(process.value, 250) != WAIT_OBJECT_0)
        TerminateProcess(process.value, 1);
    }
    job.reset();
    const bool exited = !process.value || WaitForSingleObject(process.value, 300) == WAIT_OBJECT_0;
    if (desktop)
      CloseDesktop(desktop);
    if (msg)
      UnmapViewOfFile(msg);
    if (exited)
      cleanExitedWorker(stage, sid ? profile.c_str() : nullptr);
    else
      deferWorkerCleanup(process, stage, profile);
    if (sid)
      FreeSid(sid);
  }
  void call(Operation operation, DWORD timeout) {
    if (composition && (operation == Operation::Event || operation == Operation::HitTest ||
                        operation == Operation::Render)) {
      // A callback first waits for the prior submission. Validation also waits for its new frame.
      timeout += FrameCompletionTimeoutMs;
      if (operation == Operation::Render && msg->waitForFrame)
        timeout += FrameCompletionTimeoutMs;
    }
    msg->operation = operation;
    msg->diagnostic[0] = 0;
    msg->status = E_PENDING;
    check(SetEvent(request.value), "Signal wallpaper worker");
    HANDLE wait[] = {response.value, process.value};
    DWORD result = WaitForMultipleObjects(2, wait, FALSE, timeout);
    if (result == WAIT_TIMEOUT)
      throw std::runtime_error("Wallpaper worker timed out");
    if (result != WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(process.value, &code);
      throw std::runtime_error("Wallpaper worker exited (" + std::to_string(code) + ")");
    }
    if (FAILED(msg->status))
      throw std::runtime_error(
          std::string(msg->diagnostic, strnlen_s(msg->diagnostic, sizeof(msg->diagnostic))));
  }
  void launch(ID3D11Device *device, ID3D12Device *device12, const fs::path &packPath, const fs::path &module,
              unsigned width, unsigned height, const std::string &json,
              LUID compositionAdapter = {}, HANDLE compositionSurface = nullptr, bool premultipliedAlpha = false) {
    const fs::path root = fs::absolute(packPath).lexically_normal();
    const fs::path relative =
        (module.is_absolute() ? module.lexically_relative(root) : module).lexically_normal();
    if (relative.empty() || relative.has_root_path() || *relative.begin() == L".." ||
        relative.native().find(L':') != std::wstring::npos ||
        _wcsicmp(relative.extension().c_str(), L".dll") != 0)
      throw std::runtime_error("Invalid wallpaper DLL path");
    if ((!device && !device12 && !compositionSurface) || width == 0 || height == 0 ||
        width > 16384 || height > 16384)
      throw std::runtime_error("Invalid wallpaper render target");
    const auto temp = fs::temp_directory_path();
    static std::once_flag cleanup;
    std::call_once(cleanup, [&] {
      try {
        retryRecordedCleanup(temp);
      } catch (...) {
        OutputDebugStringA("Action75 Worker cleanup: could not scan pending records\n");
      }
    });
    auto identity = idString();
    profile = L"Action75.Wallpaper." + identity;
    checkHr(CreateAppContainerProfile(profile.c_str(), L"Action75 Wallpaper",
                                      L"Isolated wallpaper session", nullptr, 0, &sid),
            "Create wallpaper AppContainer");
    LocalMemory sidText, descriptor;
    check(ConvertSidToStringSidW(sid, reinterpret_cast<LPWSTR *>(&sidText.value)),
          "Read worker SID");
    std::wstring acl = L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;GRGX;;;" +
                       std::wstring(static_cast<wchar_t *>(sidText.value)) + L")";
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              acl.c_str(), SDDL_REVISION_1,
              reinterpret_cast<PSECURITY_DESCRIPTOR *>(&descriptor.value), nullptr),
          "Create worker ACL");
    SECURITY_ATTRIBUTES stageAccess{sizeof(stageAccess), descriptor.value, FALSE};
    auto newStage = temp / (std::wstring(StagePrefix) + identity);
    check(CreateDirectoryW(newStage.c_str(), &stageAccess), "Create private worker directory");
    stage = newStage;
    const auto pack = stage / L"pack";
    copyPack(extendedPath(root), extendedPath(pack));
    const auto exe = stage / L"Action75WallpaperWorker.exe";
    copyNewFile(fs::path(hostFile()).parent_path() / exe.filename(), exe);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    request.reset(CreateEventW(&inherit, FALSE, FALSE, nullptr));
    response.reset(CreateEventW(&inherit, FALSE, FALSE, nullptr));
    mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0,
                                     sizeof(Mailbox), nullptr));
    check(request.value && response.value && mapping.value, "Allocate worker IPC");
    msg = static_cast<Mailbox *>(
        MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Mailbox)));
    check(msg != nullptr, "Map worker IPC");
    composition = compositionSurface != nullptr;
    msg->abi = device12 || composition ? 2 : 1;
    msg->composition = !composition ? CompositionMode::Off :
        premultipliedAlpha ? CompositionMode::Premultiplied : CompositionMode::Opaque;
    if (composition) {
      msg->adapter = compositionAdapter;
      // This surface is created for this session only. The worker receives no
      // host window, desktop, target or composition-device handle.
      check(DuplicateHandle(GetCurrentProcess(), compositionSurface, GetCurrentProcess(),
                            &shared.value, 0, TRUE, DUPLICATE_SAME_ACCESS),
            "Duplicate wallpaper composition surface");
    } else if (device12) {
      msg->adapter = device12->GetAdapterLuid();
      D3D12_RESOURCE_DESC d{};
      d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      d.Width = width;
      d.Height = height;
      d.MipLevels = d.DepthOrArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES memory{};
      memory.Type = D3D12_HEAP_TYPE_DEFAULT;
      memory.CreationNodeMask = memory.VisibleNodeMask = 1;
      checkHr(device12->CreateCommittedResource(&memory, D3D12_HEAP_FLAG_SHARED, &d,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&surface12)),
              "Create shared D3D12 texture");
      checkHr(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)),
              "Create wallpaper handoff fence");
      checkHr(device12->CreateSharedHandle(surface12.Get(), &inherit, GENERIC_ALL, nullptr,
                                           &shared.value),
              "Share D3D12 texture");
      checkHr(device12->CreateSharedHandle(fence12.Get(), &inherit, GENERIC_ALL, nullptr,
                                           &sharedFence.value),
              "Share D3D12 fence");
    } else {
      ComPtr<IDXGIDevice> dxgi;
      ComPtr<IDXGIAdapter> adapter;
      checkHr(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Get graphics device");
      checkHr(dxgi->GetAdapter(&adapter), "Get graphics adapter");
      DXGI_ADAPTER_DESC desc{};
      checkHr(adapter->GetDesc(&desc), "Get adapter identity");
      msg->adapter = desc.AdapterLuid;
      D3D11_TEXTURE2D_DESC texture{};
      texture.Width = width;
      texture.Height = height;
      texture.MipLevels = 1;
      texture.ArraySize = 1;
      texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      texture.SampleDesc.Count = 1;
      texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      texture.MiscFlags =
          D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
      checkHr(device->CreateTexture2D(&texture, nullptr, &surface),
              "Create shared wallpaper texture");
      ComPtr<IDXGIResource1> resource;
      checkHr(surface.As(&resource), "Get shared wallpaper resource");
      checkHr(surface.As(&mutex), "Get wallpaper texture mutex");
      checkHr(resource->CreateSharedHandle(&inherit,
                                           DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &shared.value),
              "Share wallpaper texture");
      device->GetImmediateContext(&context);
    }
    job.reset(CreateJobObjectW(nullptr, nullptr));
    check(job.value != nullptr, "Create worker job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
                                              JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 768ull << 20;
    check(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits)),
          "Limit worker job");
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_HANDLES |
                             JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
                             JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                             JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS;
    check(SetInformationJobObject(job.value, JobObjectBasicUIRestrictions, &ui, sizeof(ui)),
          "Limit worker desktop access");
    // A private, never-switched desktop also keeps user input and visible windows
    // away from the plugin while allowing graphics drivers to create hidden windows.
    std::wstring desktopName = L"Action75Worker-" + identity;
    std::wstring desktopAcl = L"D:P(A;;GA;;;OW)(A;;GA;;;SY)(A;;0xC7;;;" +
                              std::wstring(static_cast<wchar_t *>(sidText.value)) + L")";
    LocalMemory desktopDescriptor;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              desktopAcl.c_str(), SDDL_REVISION_1,
              reinterpret_cast<PSECURITY_DESCRIPTOR *>(&desktopDescriptor.value), nullptr),
          "Create desktop ACL");
    SECURITY_ATTRIBUTES desktopAccess{sizeof(desktopAccess), desktopDescriptor.value, FALSE};
    desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, &desktopAccess);
    check(desktop != nullptr, "Create private worker desktop");
    Attributes attributes;
    SECURITY_CAPABILITIES capabilities{sid, nullptr, 0, 0};
    check(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                    &capabilities, sizeof(capabilities), nullptr, nullptr),
          "Configure worker AppContainer");
    std::vector<HANDLE> handles{mapping.value, request.value, response.value, shared.value};
    if (sharedFence.value) handles.push_back(sharedFence.value);
    check(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                    handles.size()*sizeof(HANDLE), nullptr, nullptr),
          "Limit inherited handles");
    std::wostringstream cmd;
    cmd << L'"' << exe.native() << L'"';
    for (const auto h : {mapping.value, request.value, response.value, shared.value})
      cmd << L' ' << reinterpret_cast<uintptr_t>(h);
    cmd << L" \"" << pack.native() << L"\" \"" << extendedPath(pack / relative).native() << L"\"";
    if (sharedFence.value) cmd << L' ' << reinterpret_cast<uintptr_t>(sharedFence.value);
    auto command = cmd.str();
    // AppContainer setup needs the profile paths. Never pass user-defined
    // environment entries (API tokens, agent sockets, etc.) into an author DLL.
    std::wstring env;
    for (const auto name :
         {L"ALLUSERSPROFILE", L"APPDATA", L"LOCALAPPDATA", L"ProgramData", L"SystemDrive",
          L"SystemRoot", L"TEMP", L"TMP", L"USERPROFILE", L"windir"}) {
      DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
      if (!needed)
        continue;
      std::wstring value(needed, 0);
      DWORD n = GetEnvironmentVariableW(name, value.data(), needed);
      if (!n || n >= needed)
        continue;
      env.append(name).append(L"=").append(value.data(), n).push_back(0);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.lpDesktop = desktopName.data();
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION info{};
    check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW |
                             CREATE_UNICODE_ENVIRONMENT,
                         env.data(), stage.c_str(), &startup.StartupInfo, &info),
          "Launch wallpaper AppContainer");
    process.reset(info.hProcess);
    Handle thread(info.hThread);
    for (auto handle : handles)
      check(SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0), "Seal inherited handle");
    check(AssignProcessToJobObject(job.value, process.value), "Confine wallpaper to job");
    msg->width = width;
    msg->height = height;
    textField(msg->settings, sizeof(msg->settings), json.c_str());
    check(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Start wallpaper worker");
    call(Operation::Initialize, 5000);
    this->width = width;
    this->height = height;
    hasHitTest = msg->hit >= 0;
  }
};
NativePluginSession::NativePluginSession() = default;
NativePluginSession::~NativePluginSession() = default;
bool NativePluginSession::active() const noexcept {
  return impl_ && WaitForSingleObject(impl_->process.value, 0) == WAIT_TIMEOUT;
}
unsigned NativePluginSession::renderAbi() const noexcept {
  return active() ? (impl_->composition || impl_->surface12 ? 2u : 1u) : 0u;
}
void NativePluginSession::stop() noexcept {
  // Recovery still needs the Worker's device-loss report after the mailbox is unmapped.
  if (impl_)
    stoppedWithCompositionDeviceLoss_ = compositionDeviceLost();
  impl_.reset();
}
DWORD NativePluginSession::workerPid() const noexcept {
  return impl_ ? GetProcessId(impl_->process.value) : 0;
}
HANDLE NativePluginSession::exitHandle() const noexcept {
    return impl_ ? impl_->process.value : nullptr;
}
bool NativePluginSession::start(ID3D11Device *device, const fs::path &root, const fs::path &module,
                                unsigned width, unsigned height, const std::string &json,
                                std::string &error) {
  try {
    auto candidate = std::make_unique<Impl>();
    candidate->launch(device, nullptr, root, module, width, height, json);
    impl_ = std::move(candidate);
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
bool NativePluginSession::start12(ID3D12Device *device,const fs::path &root,const fs::path &module,
                                  unsigned width,unsigned height,const std::string &json,std::string &error) {
  try {
    auto candidate=std::make_unique<Impl>();
    candidate->launch(nullptr,device,root,module,width,height,json);
    impl_=std::move(candidate);
    return true;
  } catch(const std::exception &e) { error=e.what(); return false; }
}
bool NativePluginSession::startComposition(LUID adapter, HANDLE surface, const fs::path &root,
                                            const fs::path &module, unsigned width, unsigned height,
                                            const std::string &json, std::string &error, bool premultipliedAlpha) {
  try {
    auto candidate = std::make_unique<Impl>();
    candidate->launch(nullptr, nullptr, root, module, width, height, json, adapter, surface, premultipliedAlpha);
    impl_ = std::move(candidate);
    return true;
  } catch (const std::exception &failure) {
    error = failure.what();
    return false;
  }
}
bool NativePluginSession::renderComposition(double seconds, int32_t &delay, bool validate,
                                             HRESULT &present, std::string &error) {
  if (!impl_ || !impl_->composition) {
    error = "No active D3D12 composition worker";
    return false;
  }
  try {
    impl_->msg->seconds = seconds;
    impl_->msg->waitForFrame = validate;
    impl_->call(Operation::Render, 500);
    delay = impl_->msg->delay;
    present = impl_->msg->presentResult;
    if (delay < -1 || (FAILED(present) && present != DXGI_ERROR_WAS_STILL_DRAWING))
      throw std::runtime_error("Invalid D3D12 composition result");
    return true;
  } catch (const std::exception &failure) {
    error = failure.what();
    return false;
  }
}
bool NativePluginSession::compositionDeviceLost() const noexcept {
  if (!impl_)
    return stoppedWithCompositionDeviceLoss_;
  return impl_->composition &&
         InterlockedCompareExchange(&impl_->msg->compositionDeviceRemoved, 0, 0) != 0;
}
bool NativePluginSession::render12(double seconds,int32_t &nextDelay,const Copy12 &copy,std::string &error) {
  if(!impl_ || !impl_->surface12) { error="No active D3D12 wallpaper"; return false; }
  try {
    const auto workerDone=impl_->handoff+1;
    impl_->msg->waitValue=impl_->handoff;
    impl_->msg->signalValue = workerDone;
    impl_->msg->seconds = seconds;
    impl_->call(Operation::Render, 500);
    if (!copy(impl_->surface12.Get(), impl_->fence12.Get(), workerDone, error)) {
      stop();
      return false;
    }
    impl_->handoff = workerDone + 1;
    nextDelay = impl_->msg->delay;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    stop();
    return false;
  }
}
bool NativePluginSession::event(const A75Event &event, std::string &error) {
  if (!impl_) {
    error = "Wallpaper worker is inactive";
    return false;
  }
  try {
    auto msg = impl_->msg;
    msg->type = event.type;
    msg->width = event.width;
    msg->height = event.height;
    msg->seconds = event.seconds;
    msg->x = event.x;
    msg->y = event.y;
    textField(msg->code, sizeof(msg->code), event.code);
    textField(msg->settings, sizeof(msg->settings), event.settings_json);
    impl_->call(Operation::Event, 250);
    impl_->patch.assign(msg->patch, strnlen_s(msg->patch, sizeof(msg->patch) - 1));
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    stop();
    return false;
  }
}
std::string NativePluginSession::takeSettingsPatch() {
  return impl_ ? std::exchange(impl_->patch, {}) : std::string{};
}
bool NativePluginSession::hitTest(float x, float y, bool &hit, std::string &error) {
  if (!impl_) {
    error = "Wallpaper worker is inactive";
    hit = false;
    return false;
  }
  try {
    if (!impl_->hasHitTest) {
      hit = false;
      return true;
    }
    impl_->msg->x = x;
    impl_->msg->y = y;
    impl_->call(Operation::HitTest, 250);
    hit = impl_->msg->hit != 0;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    hit = false;
    stop();
    return false;
  }
}
bool NativePluginSession::render(ID3D11Texture2D *target, double seconds, int32_t &nextDelay, std::string &error) {
  if (!impl_) {
    error = "Wallpaper worker is inactive";
    return false;
  }
  try {
    if (!target)
      throw std::runtime_error("No active host render target");
    D3D11_TEXTURE2D_DESC host{};
    target->GetDesc(&host);
    if (host.Width != impl_->width || host.Height != impl_->height ||
        host.Format != DXGI_FORMAT_R8G8B8A8_UNORM || host.SampleDesc.Count != 1 ||
        host.ArraySize != 1 || host.MipLevels != 1)
      throw std::runtime_error("Host output size or format changed");
    impl_->msg->seconds = seconds;
    impl_->call(Operation::Render, 500);
    if (impl_->mutex->AcquireSync(1, 150) != S_OK)
      throw std::runtime_error("Wallpaper shared texture timed out");
    impl_->context->CopyResource(target, impl_->surface.Get());
    impl_->context->Flush();
    checkHr(impl_->mutex->ReleaseSync(0), "Release shared texture");
    nextDelay = impl_->msg->delay;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    stop();
    return false;
  }
}
} // namespace action75
