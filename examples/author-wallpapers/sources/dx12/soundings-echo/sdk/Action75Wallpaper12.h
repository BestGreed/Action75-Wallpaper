#pragma once
/* Windows x64, D3D12, C ABI v2. A75Event and event IDs are shared with ABI v1.
 * All callbacks run serially in the isolated worker. Never throw across this ABI. */
#include "Action75Wallpaper.h"
#include <d3d12.h>

#define ACTION75_WALLPAPER_ABI12 2u

typedef struct A75Create12 {
  uint32_t size, width, height;
  ID3D12Device *device; /* Borrowed until destroy; same adapter as the player. */
  const wchar_t *pack_dir; /* Read-only package directory, NUL-terminated UTF-16. */
  const char *settings_json; /* Borrowed for create only: NUL-terminated UTF-8 object. */
  void *host;
  /* Available only during on_event. patch_json is a NUL-terminated UTF-8 object. */
  void (__cdecl *commit_settings)(void *host, const char *patch_json);
} A75Create12;

typedef struct A75Frame12 {
  uint32_t size, width, height;
  double seconds;
  /* Borrowed for this call. Append commands; the worker closes and submits. */
  ID3D12GraphicsCommandList *commands;
  /* Single-sample RGBA8 UNORM, RENDER_TARGET on entry and return. */
  ID3D12Resource *target;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
} A75Frame12;

typedef struct A75Wallpaper12 {
  uint32_t size, abi;
  void *(__cdecl *create)(const A75Create12 *);
  void (__cdecl *on_event)(void *, const A75Event *);
  /* The previous frame's GPU commands are complete before render/on_event.
   * The worker binds rtv, viewport and scissor. The author binds its PSOs,
   * heaps, root signatures etc. The author must fully draw/clear the frame.
   * Keep referenced resources alive until the next
   * callback or destroy. -1 sleeps until an event, 0 schedules the next frame,
   * positive values delay in ms, values below -1 fail the frame. */
  int32_t (__cdecl *render)(void *, const A75Frame12 *);
  void (__cdecl *destroy)(void *);
  int32_t (__cdecl *hit_test)(void *, float, float, uint32_t, uint32_t); /* Optional. */
} A75Wallpaper12;
typedef const A75Wallpaper12 *(__cdecl *A75GetWallpaper12)(uint32_t abi);
#ifdef __cplusplus
extern "C" {
#endif
#ifdef ACTION75_WALLPAPER12_EXPORTS
__declspec(dllexport)
#endif
const A75Wallpaper12 *__cdecl Action75GetWallpaper12(uint32_t abi);
#ifdef __cplusplus
}
#endif
