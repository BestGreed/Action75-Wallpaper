#pragma once
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
  void stop();
  ~IpcServer();

private:
  void run();
  std::wstring name_;
  Handler handler_;
  HANDLE stopped_ = nullptr;
  std::thread worker_;
};
nlohmann::json sendCommand(const std::wstring &pipeName,
                           const nlohmann::json &request, DWORD expectedPipePid = 0);
} // namespace action75
