#pragma once
/* Windows x64, D3D11, C ABI v1. All pointers are borrowed for the call.
 * UTF-8 JSON strings are NUL terminated. Coordinates use normalized screen space.
 * Callbacks run serially. No exception may cross the ABI. */
#include <stdint.h>
#include <d3d11.h>

#define ACTION75_WALLPAPER_ABI 1u
enum A75EventType {
  A75_KEY_DOWN = 1, A75_KEY_UP, A75_POINTER_DOWN, A75_POINTER_MOVE,
  A75_POINTER_UP, A75_SETTINGS, A75_RESIZE, A75_PAUSE, A75_RESUME,
  A75_INPUT_RESET
};
typedef struct A75Create {
  uint32_t size, width, height;
  ID3D11Device *device;
  ID3D11DeviceContext *context;
  const wchar_t *pack_dir;
  const char *settings_json; /* object keyed by author control ID */
  void *host;
  void (__cdecl *commit_settings)(void *host, const char *patch_json);
} A75Create;
typedef struct A75Event {
  uint32_t size, type, width, height;
  double seconds; /* monotonic seconds, same clock as frames */
  float x, y;
  const char *code; /* physical key name, e.g. KeyA */
  const char *settings_json;
} A75Event;
typedef struct A75Frame {
  uint32_t size, width, height;
  double seconds;
  /* ABI v1 host target format: DXGI_FORMAT_R8G8B8A8_UNORM. */
  ID3D11RenderTargetView *target;
} A75Frame;
typedef struct A75Wallpaper {
  uint32_t size, abi;
  void *(__cdecl *create)(const A75Create *);
  void (__cdecl *on_event)(void *, const A75Event *);
  /* -1 = sleep until an event, 0 = next paced frame, positive = delay in ms.
   * Values below -1 indicate a rendering failure. */
  int32_t (__cdecl *render)(void *, const A75Frame *);
  void (__cdecl *destroy)(void *);
  /* Optional pointer capture region; normalized coordinates. */
  int32_t (__cdecl *hit_test)(void *, float, float, uint32_t, uint32_t);
} A75Wallpaper;
/* Export this symbol from the DLL (C linkage, undecorated). */
typedef const A75Wallpaper *(__cdecl *A75GetWallpaper)(uint32_t abi);
#ifdef __cplusplus
extern "C" {
#endif
#ifdef ACTION75_WALLPAPER_EXPORTS
__declspec(dllexport)
#endif
const A75Wallpaper *__cdecl Action75GetWallpaper(uint32_t abi);
#ifdef __cplusplus
}
#endif
