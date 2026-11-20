#include "Action75Wallpaper.h"
#include <string>
#include <filesystem>
struct State {
  A75Create host;
  bool keyed = false;
  bool hang = false;
  bool exitEarly = false;
  explicit State(const A75Create &create) : host(create) {}
};
static void *__cdecl create(const A75Create *args) {
  // Check the actual effective token and the package's read-only ACL.
  HANDLE token = nullptr;
  DWORD ac = 0, bytes = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return nullptr;
  BOOL ok = GetTokenInformation(token, TokenIsAppContainer, &ac, sizeof(ac), &bytes);
  CloseHandle(token);
  if (!ok || !ac)
    return nullptr;
  HANDLE f = CreateFileW((std::filesystem::path(args->pack_dir) / L"deny-write").c_str(),
                         GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    CloseHandle(f);
    return nullptr;
  }
  auto state = new State(*args);
  state->hang = std::string(args->settings_json) == "{\"hang\":true}";
  state->exitEarly = std::string(args->settings_json) == "{\"exit\":true}";
  return state;
}
static void __cdecl event(void *p, const A75Event *e) {
  auto s = static_cast<State *>(p);
  if (e->type == A75_SETTINGS && std::string(e->settings_json) == "{\"exit\":true}")
    ExitProcess(83);
  if (e->type == A75_KEY_DOWN) {
    s->keyed = true;
    s->host.commit_settings(s->host.host, "{\"seen\":true}");
  }
}
static int32_t __cdecl render(void *p, const A75Frame *f) {
  auto s = static_cast<State *>(p);
  if (s->hang)
    Sleep(30000);
  if (s->exitEarly)
    ExitProcess(83);
  const float rgba[] = {s->keyed ? 1.f : 0.f, 0.5f, 0.f, 1.f};
  s->host.context->ClearRenderTargetView(f->target, rgba);
  return 70;
}
static void __cdecl destroy(void *p) { delete static_cast<State *>(p); }
static int32_t __cdecl hit(void *, float x, float, uint32_t, uint32_t) { return x < 0.5f; }
#pragma comment(linker, "/export:Action75GetWallpaper")
extern "C" const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi) {
  static const A75Wallpaper api{sizeof(api), ACTION75_WALLPAPER_ABI, create, event, render, destroy,
                                hit};
  return abi == ACTION75_WALLPAPER_ABI ? &api : nullptr;
}
