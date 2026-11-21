#pragma once
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <windows.h>

namespace action75 {
class IpcServer {
public:
  using Handler = std::function<nlohmann::json(const nlohmann::json &)>;
  bool start(const std::wstring &name, Handler handler);
  void requestStop();
  void stopAfterReply() noexcept { stopAfterReply_.store(true); }
  HANDLE stoppingHandle() const noexcept { return stopped_; }
  void stop();
  ~IpcServer();

private:
  void run();
  Handler handler_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE connected_ = nullptr;
  HANDLE stopped_ = nullptr;
  std::atomic<bool> stopAfterReply_{false};
  std::thread worker_;
};
nlohmann::json sendCommand(const std::wstring &pipeName,
                           const nlohmann::json &request, DWORD expectedPipePid = 0);
} // namespace action75
