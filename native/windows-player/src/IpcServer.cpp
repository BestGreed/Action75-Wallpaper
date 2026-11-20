#include "IpcServer.h"
#include <stdexcept>
#include <vector>

namespace action75 {
using json = nlohmann::json;
constexpr size_t MaxMessage = 65536;
static std::wstring pipePath(const std::wstring &name) {
  if (name.empty() || name.size() > 100 ||
      name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"
                             L"WXYZ0123456789-_") != std::wstring::npos)
    throw std::runtime_error("Invalid local pipe name");
  return L"\\\\.\\pipe\\" + name;
}
static bool transfer(HANDLE pipe, HANDLE stopped, bool writing, void *bytes,
                     DWORD size, DWORD &transferred) {
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent)
    return false;
  BOOL ok = writing ? WriteFile(pipe, bytes, size, &transferred, &operation)
                    : ReadFile(pipe, bytes, size, &transferred, &operation);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    HANDLE waits[] = {operation.hEvent, stopped};
    DWORD result = WaitForMultipleObjects(stopped ? 2 : 1, waits, FALSE, 3000);
    if (result == WAIT_OBJECT_0)
      ok = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    else {
      CancelIoEx(pipe, &operation);
      WaitForSingleObject(operation.hEvent, INFINITE);
      ok = FALSE;
    }
  }
  CloseHandle(operation.hEvent);
  return ok != FALSE;
}
bool IpcServer::start(const std::wstring &name, Handler handler) {
  if (worker_.joinable())
    return false;
  name_ = pipePath(name);
  handler_ = std::move(handler);
  stopped_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopped_)
    return false;
  worker_ = std::thread([this] { run(); });
  return true;
}
void IpcServer::requestStop() {
  if (stopped_)
    SetEvent(stopped_);
}
void IpcServer::stop() {
  requestStop();
  if (worker_.joinable())
    worker_.join();
  if (stopped_) {
    CloseHandle(stopped_);
    stopped_ = nullptr;
  }
}
IpcServer::~IpcServer() { stop(); }
void IpcServer::run() {
  while (WaitForSingleObject(stopped_, 0) != WAIT_OBJECT_0) {
    HANDLE pipe = CreateNamedPipeW(name_.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                       PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
      return;
    OVERLAPPED connection{};
    connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ConnectNamedPipe(pipe, &connection);
    DWORD connectionError = connected ? ERROR_SUCCESS : GetLastError();
    if (connectionError == ERROR_PIPE_CONNECTED)
      SetEvent(connection.hEvent);
    if (connectionError == ERROR_IO_PENDING ||
        connectionError == ERROR_PIPE_CONNECTED) {
      HANDLE waits[] = {connection.hEvent, stopped_};
      DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (result != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &connection);
        WaitForSingleObject(connection.hEvent, INFINITE);
        CloseHandle(connection.hEvent);
        CloseHandle(pipe);
        break;
      }
      connected = TRUE;
    }
    CloseHandle(connection.hEvent);
    if (connected) {
      std::string message;
      char buffer[4096];
      bool complete = false;
      while (message.size() < MaxMessage) {
        DWORD count = 0;
        if (!transfer(pipe, stopped_, false, buffer, sizeof(buffer), count) ||
            !count)
          break;
        message.append(buffer, count);
        size_t newline = message.find('\n');
        if (newline != std::string::npos) {
          message.resize(newline);
          complete = true;
          break;
        }
      }
      if (complete && message.size() < MaxMessage &&
          WaitForSingleObject(stopped_, 0) != WAIT_OBJECT_0) {
        json reply;
        try {
          const auto request = json::parse(message);
          if (!request.is_object())
            throw std::runtime_error("Expected JSON object");
          reply = handler_(request);
        } catch (const std::exception &e) {
          reply = {{"ok", false}, {"error", e.what()}};
        }
        std::string line = reply.dump() + "\n";
        DWORD sent = 0;
        size_t offset = 0;
        while (offset < line.size() &&
               transfer(pipe, stopped_, true, line.data() + offset,
                        static_cast<DWORD>(line.size() - offset), sent) &&
               sent)
          offset += sent;
        // The client acknowledges only after reading the complete reply.
        // DisconnectNamedPipe otherwise discards unread outbound bytes.
        if (offset == line.size()) {
          char acknowledged = 0;
          DWORD count = 0;
          transfer(pipe, stopped_, false, &acknowledged, 1, count);
        }
      }
      DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
  }
}
json sendCommand(const std::wstring &name, const json &request, DWORD expectedPipePid) {
  std::wstring path = pipePath(name);
  HANDLE pipe = INVALID_HANDLE_VALUE;
  const ULONGLONG deadline = GetTickCount64() + 2000;
  while (GetTickCount64() < deadline) {
    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
      break;
    const DWORD failure = GetLastError();
    if (failure != ERROR_PIPE_BUSY && failure != ERROR_FILE_NOT_FOUND)
      break;
    if (!WaitNamedPipeW(path.c_str(), 50) &&
        GetLastError() == ERROR_FILE_NOT_FOUND)
      Sleep(10);
  }
  if (pipe == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot connect to native player");
  try {
    if (expectedPipePid) {
      ULONG pid = 0;
      if (!GetNamedPipeServerProcessId(pipe, &pid) || pid != expectedPipePid)
        throw std::runtime_error("Player identity changed");
    }
    std::string line = request.dump() + "\n";
    if (line.size() >= MaxMessage)
      throw std::runtime_error("Command exceeds size limit");
    DWORD count = 0;
    size_t offset = 0;
    while (offset < line.size()) {
      if (!transfer(pipe, nullptr, true, line.data() + offset,
                    static_cast<DWORD>(line.size() - offset), count) ||
          !count)
        throw std::runtime_error("Cannot send command");
      offset += count;
    }
    std::string reply;
    char bytes[4096];
    while (reply.size() < MaxMessage) {
      if (!transfer(pipe, nullptr, false, bytes, sizeof(bytes), count) ||
          !count)
        throw std::runtime_error("Cannot read native player reply");
      reply.append(bytes, count);
      size_t newline = reply.find('\n');
      if (newline != std::string::npos) {
        reply.resize(newline);
        auto parsed = json::parse(reply);
        char acknowledge = '\n';
        DWORD sent = 0;
        transfer(pipe, nullptr, true, &acknowledge, 1, sent);
        CloseHandle(pipe);
        return parsed;
      }
    }
    throw std::runtime_error("Reply exceeds size limit");
  } catch (...) {
    CloseHandle(pipe);
    throw;
  }
}
} // namespace action75
