#include "IpcServer.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace action75 {
using json = nlohmann::json;
constexpr size_t MaxMessage = 65536;
constexpr DWORD TransferTimeout = 3000;
constexpr DWORD CommandTimeout = 6000;
constexpr DWORD ConnectionTimeout = 2000;
static DWORD remainingMilliseconds(ULONGLONG deadline) {
  const auto now = GetTickCount64();
  return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}
static std::wstring pipePath(const std::wstring &name) {
  if (name.empty() || name.size() > 100 ||
      name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"
                             L"WXYZ0123456789-_") != std::wstring::npos)
    throw std::runtime_error("Invalid local pipe name");
  return L"\\\\.\\pipe\\" + name;
}
static bool transfer(HANDLE pipe, HANDLE stopped, bool writing, void *bytes,
                     DWORD size, DWORD &transferred, DWORD timeout = TransferTimeout) {
  if (!timeout)
    return false;
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent)
    return false;
  BOOL ok = writing ? WriteFile(pipe, bytes, size, &transferred, &operation)
                    : ReadFile(pipe, bytes, size, &transferred, &operation);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    HANDLE waits[] = {operation.hEvent, stopped};
    DWORD result = WaitForMultipleObjects(stopped ? 2 : 1, waits, FALSE, timeout);
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
static void reportPipeError(const char *operation, DWORD error) noexcept {
  char message[160]{};
  std::snprintf(message, sizeof(message), "Action75 control pipe: %s (Win32 %lu)\n",
                operation, static_cast<unsigned long>(error));
  OutputDebugStringA(message);
}
bool IpcServer::start(const std::wstring &name, Handler handler) {
  if (worker_.joinable())
    return false;
  const auto path = pipePath(name);
  pipe_ = CreateNamedPipeW(path.c_str(),
                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                               FILE_FLAG_FIRST_PIPE_INSTANCE,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                               PIPE_REJECT_REMOTE_CLIENTS,
                           1, 65536, 65536, 0, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    reportPipeError("CreateNamedPipeW", GetLastError());
    return false;
  }
  handler_ = std::move(handler);
  stopped_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  connected_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopped_ || !connected_) {
    stop();
    return false;
  }
  stopAfterReply_.store(false);
  try {
    worker_ = std::thread([this] { run(); });
  } catch (const std::system_error &) {
    stop();
    return false;
  }
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
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
  if (connected_) {
    CloseHandle(connected_);
    connected_ = nullptr;
  }
  if (stopped_) {
    CloseHandle(stopped_);
    stopped_ = nullptr;
  }
}
IpcServer::~IpcServer() { stop(); }

static std::string_view encodeReply(const IpcServer::Handler &handler, const std::string &message,
                                    std::string &storage) noexcept {
  try {
    json reply;
    try {
      const auto request = json::parse(message);
      if (!request.is_object())
        throw std::runtime_error("Expected JSON object");
      reply = handler(request);
    } catch (const std::exception &e) {
      reply = {{"ok", false}, {"error", e.what()}};
    }
    storage = reply.dump(-1, ' ', false, json::error_handler_t::replace);
    if (storage.size() >= MaxMessage - 1)
      return R"({"ok":false,"error":"Player reply exceeds size limit"})" "\n";
    storage.push_back('\n');
    return storage;
  } catch (...) {
    return R"({"ok":false,"error":"Cannot process player request"})" "\n";
  }
}

void IpcServer::run() {
  while (WaitForSingleObject(stopped_, 0) != WAIT_OBJECT_0) {
    ResetEvent(connected_);
    OVERLAPPED connection{};
    connection.hEvent = connected_;
    BOOL connected = ConnectNamedPipe(pipe_, &connection);
    DWORD connectionError = connected ? ERROR_SUCCESS : GetLastError();
    if (connectionError == ERROR_PIPE_CONNECTED)
      connected = TRUE;
    else if (connectionError == ERROR_IO_PENDING) {
      HANDLE waits[] = {connected_, stopped_};
      DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (result != WAIT_OBJECT_0) {
        const DWORD waitError = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        CancelIoEx(pipe_, &connection);
        WaitForSingleObject(connected_, INFINITE);
        if (waitError) {
          reportPipeError("Wait for connection", waitError);
          if (WaitForSingleObject(stopped_, 100) == WAIT_TIMEOUT)
            continue;
        }
        return;
      }
      DWORD count = 0;
      connected = GetOverlappedResult(pipe_, &connection, &count, FALSE);
      connectionError = connected ? ERROR_SUCCESS : GetLastError();
    }
    if (!connected) {
      DisconnectNamedPipe(pipe_);
      if (connectionError != ERROR_NO_DATA && connectionError != ERROR_BROKEN_PIPE &&
          connectionError != ERROR_PIPE_NOT_CONNECTED) {
        reportPipeError("ConnectNamedPipe", connectionError);
        WaitForSingleObject(stopped_, 100);
      }
      continue;
    }
    const auto deadline = GetTickCount64() + CommandTimeout;
    const auto transferMessage = [&](bool writing, void *bytes, DWORD size, DWORD &count) {
      return transfer(pipe_, stopped_, writing, bytes, size, count,
                      std::min(TransferTimeout, remainingMilliseconds(deadline)));
    };
    try {
      std::string message;
      char buffer[4096];
      bool complete = false;
      while (message.size() < MaxMessage) {
        DWORD count = 0;
        if (!transferMessage(false, buffer, sizeof(buffer), count) ||
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
      if (complete && message.size() < MaxMessage && remainingMilliseconds(deadline) &&
          WaitForSingleObject(stopped_, 0) != WAIT_OBJECT_0) {
        std::string storage;
        const auto line = encodeReply(handler_, message, storage);
        DWORD sent = 0;
        size_t offset = 0;
        while (offset < line.size() &&
               transferMessage(true, const_cast<char *>(line.data() + offset),
                               static_cast<DWORD>(line.size() - offset), sent) &&
               sent)
          offset += sent;
        // The client acknowledges only after reading the complete reply.
        // DisconnectNamedPipe otherwise discards unread outbound bytes.
        if (offset == line.size()) {
          char acknowledged = 0;
          DWORD count = 0;
          transferMessage(false, &acknowledged, 1, count);
        }
      }
    } catch (...) {
      OutputDebugStringA("Action75 control pipe: cannot buffer a request\n");
    }
    DisconnectNamedPipe(pipe_);
    if (stopAfterReply_.load())
      requestStop();
  }
}
static json exchangeCommand(const std::wstring &name, const json &request, DWORD expectedPipePid,
                            DWORD timeout, DWORD &unconfirmedPeer) {
  std::wstring path = pipePath(name);
  HANDLE pipe = INVALID_HANDLE_VALUE;
  const auto started = GetTickCount64();
  const auto remaining = [started](DWORD budget) { return remainingMilliseconds(started + budget); };
  while (const auto left = remaining(std::min(ConnectionTimeout, timeout))) {
    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
      break;
    const DWORD failure = GetLastError();
    if (failure != ERROR_PIPE_BUSY && failure != ERROR_FILE_NOT_FOUND)
      break;
    if (!WaitNamedPipeW(path.c_str(), std::min<DWORD>(left, 50)) &&
        GetLastError() == ERROR_FILE_NOT_FOUND)
      Sleep(std::min<DWORD>(left, 10));
  }
  if (pipe == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Cannot connect to native player");
  try {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid))
      pid = 0;
    if (expectedPipePid && pid != expectedPipePid)
      throw std::runtime_error("Player identity changed");
    std::string line = request.dump() + "\n";
    if (line.size() >= MaxMessage)
      throw std::runtime_error("Command exceeds size limit");
    DWORD count = 0;
    size_t offset = 0;
    while (offset < line.size()) {
      if (!transfer(pipe, nullptr, true, line.data() + offset,
                    static_cast<DWORD>(line.size() - offset), count, remaining(timeout)) ||
          !count)
        throw std::runtime_error("Cannot send command");
      offset += count;
    }
    unconfirmedPeer = pid;
    std::string reply;
    char bytes[4096];
    while (reply.size() < MaxMessage) {
      if (!transfer(pipe, nullptr, false, bytes, sizeof(bytes), count, remaining(timeout)) ||
          !count)
        throw std::runtime_error("Native player reply unavailable. Query status before retrying.");
      reply.append(bytes, count);
      size_t newline = reply.find('\n');
      if (newline != std::string::npos) {
        if (newline >= MaxMessage)
          break;
        reply.resize(newline);
        auto parsed = json::parse(reply);
        char acknowledge = '\n';
        DWORD sent = 0;
        transfer(pipe, nullptr, true, &acknowledge, 1, sent, remaining(timeout));
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
json sendCommand(const std::wstring &name, const json &request, DWORD expectedPipePid) {
  DWORD peer = 0;
  try {
    return exchangeCommand(name, request, expectedPipePid, CommandTimeout, peer);
  } catch (const std::exception &) {
    if (peer && request.contains("action") && request["action"] == "selectPack") {
      // The selection may already have committed. Read the same player's
      // current state once, without issuing the selection again.
      try {
        json query = {{"action", "status"}, {"expectedPlayerPid", peer}};
        if (request.contains("expectedRuntimeRoot"))
          query["expectedRuntimeRoot"] = request["expectedRuntimeRoot"];
        DWORD ignored = 0;
        auto observed = exchangeCommand(name, query, peer, ConnectionTimeout, ignored);
        if (observed.value("ok", false))
          return {{"ok", false},
                  {"error", "Wallpaper switch reply unavailable. Current player status is attached."},
                  {"observedStatus", std::move(observed.at("result"))}};
      } catch (const std::exception &) {
      }
    }
    throw;
  }
}
} // namespace action75
