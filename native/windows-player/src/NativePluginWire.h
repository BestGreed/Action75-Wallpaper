#pragma once
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>

namespace action75::worker {
using Microsoft::WRL::ComPtr;
enum class Operation : unsigned { Initialize, Event, Render, HitTest, Exit };
// Fixed-size mailbox. No address in this mapping is ever dereferenced by the host.
struct Mailbox {
  Operation operation;
  LUID adapter;
  unsigned width, height, type;
  double seconds;
  float x, y;
  char code[128];
  char settings[128 * 1024];
  char patch[16 * 1024 + 1];
  int32_t hit;
  int32_t delay;
  LONG status;
  char diagnostic[192];
};
class Handle {
public:
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() { reset(); }
  void reset(HANDLE handle = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = handle;
  }
  HANDLE value = nullptr;
};
} // namespace action75::worker
