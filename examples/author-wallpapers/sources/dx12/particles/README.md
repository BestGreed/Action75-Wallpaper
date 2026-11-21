# Real CPU particles on the public D3D12 ABI

This directory independently builds the `lantern-almanac` and `tidemark-bearing`
packages. `assets/` preserves the original exported `.efk`, textures and covers
byte for byte. Its settings retain every original control and add independent
background/foreground opacity. The editable Effekseer projects and generator remain in
the original `sources/particle` and `sources/particle/dynamic-proof` author directories.

The runtime does not distinguish wallpaper IDs. `particles.json` declares the
ordered resource paths and base scale, while the original settings schema declares
playback roles and parameter bindings. The native manifest contains only public
importer fields. Both packages statically compile the same package-side library.

## Build independently

Requires Windows x64, Visual Studio 2022 Desktop C++ and a Windows SDK, CMake 3.24+,
the **public** Action75 SDK headers (`Action75Wallpaper.h`,
`Action75Wallpaper12.h`), and the public Effekseer C++ 1.80.8 source SDK. No
Action75 player source, private header, private library, executable or installed
runtime is required. The public nlohmann/json single header is included here.

Obtain the upstream C++ SDK from the [official download page](https://effekseer.github.io/en/download.html)
or its [pinned release archive](https://github.com/effekseer/Effekseer/releases/download/1808/EffekseerForCpp1.80.8.zip).
The archive SHA-256 is:

```text
4206c2b3704fee9f84331e18c7f80d061f7989910b1fada82c7857b2b5efc035
```

Copy this entire directory anywhere, extract that SDK and run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DACTION75_SDK_INCLUDE=C:/public-action75-sdk `
  -DEFFEKSEER_ROOT=C:/deps/EffekseerForCpp1.80.8
cmake --build build --config Release --target author_particles12 --parallel 2
cmake --install build --config Release --prefix C:/output/particle-packs
```

The two output directories are complete importable packs. The DLL uses `/MT` and
links Effekseer CPU, EffekseerRendererDX12 and LLGI statically. It does not link
D3D11, D3D11On12, a host library, editor, audio runtime or another rendering API.
Third-party notices are copied into each pack's `licenses` directory.

The library initializes on the borrowed ABI2 device. Its private DIRECT queue is
used by the unmodified SDK's synchronous resource uploader during asset loading.
For frames, `EffekseerRendererDX12::BeginCommandList` wraps the **borrowed host
command list**; `EndCommandList` releases that wrapper without closing or
submitting the host list. The host owns submission, synchronization, target and
presentation. The package clears the target, updates the original CPU particle
trees, and issues the real SDK renderer's sprite/track/ribbon geometry. No
trajectory, child emitter, trail or ember is substituted with a similar animation.

No effect asset or filesystem operation is performed in the frame loop. The
runtime retains the original 8192-instance manager capacity and 64-live-effect
limit, prunes expired handles, and returns the ABI sleep result after effects
expire. Pause freezes simulation and excludes the paused wall-clock interval on
resume. Input reset stops all live effects and rewinds variant order while
retaining keyboard cadence, matching legacy `stopEffects`. The public reset
event carries no reason: the host also sends it when pointer capture is cancelled,
where the legacy built-in Effekseer backend did not clear particles. This one
capture-cancellation distinction cannot be preserved through the public ABI.
Resource paths use the supplied package directory directly, avoiding
filesystem canonicalization queries that fail inside the host AppContainer.

## Independent object opacity

`backgroundOpacity` and `foregroundOpacity` are percentage sliders (0–100,
default 100). The manifest's public `desktopOpacityControl` points to the
background control. Old profiles that omit either value retain 100%. Neither
control changes the exported effects, original appearance controls, keyboard
cadence, selected variants or the dynamic bearing input.

100/100 retains the original direct-render path, including all stock Effekseer
blend modes. When either opacity is reduced, the complete real Effekseer draw
is rendered into one persistent RGBA8 texture. Its RGB is already the result of
the original blend operations over transparent black. Since stock additive
nodes can accumulate RGB above their Max alpha, the layer's coverage is resolved
as `max(stockAlpha, R, G, B)`. RGB and coverage are then scaled together by
foreground opacity and composed with the independently premultiplied background
using standard source-over. This guarantees `RGB <= alpha` and transparent empty
pixels of `(0,0,0,0)` for the public ABI's premultiplied output.

The transparent branch therefore has ordinary coverage over a light desktop;
it cannot reproduce unlimited additive light against every possible external
desktop color through a single bounded premultiplied layer. Default 100/100
keeps the original rendering exactly. Opacity fades the completed foreground as
one object rather than changing every particle's alpha before overlapping.

The texture, descriptors and composite pipeline are allocated lazily and reused;
they are recreated only for a size change. Foreground 0 skips particle GPU
drawing while continuing the actual CPU simulation with a 100 ms delayed wake.
Expired effects and static backgrounds return `-1`; pause also returns `-1`.
Restoring opacity retains the correct live age and never revives expired effects.

The additional verifier loads both actual DLLs and compares 100/100 against a
frozen DX12 baseline, then checks the nine 0/50/100 combinations, white-background
removal, bounded premultiplication, whole-layer scaling and hidden-effect age:

```powershell
cmake --build build --config Release --target verify_particle_opacity --parallel 2
build/Release/verify_particle_opacity.exe C:/output/particle-packs C:/frozen-dx12-packs C:/evidence
```

The optional `warp` fourth argument selects WARP. The PNG writer checks WIC's
negotiated channel order so BGRA negotiation cannot swap the evidence colors.
Raw `*-premul.png` captures store the actual premultiplied GPU bytes; the
`*-over-dark.png` and `*-over-light.png` files are explicit offline source-over
previews, not an actual desktop/host integration test.

## Independent real-renderer comparison

The optional verifier alone links a DX11 reference renderer. Enabling it does not
link DX11 into `wallpaper.dll`:

```powershell
cmake -S . -B build -DA75_PARTICLE_TESTS=ON
cmake --build build --config Release --target verify_particles --parallel 2
build/Release/verify_particles.exe C:/output/particle-packs C:/frozen-original-packs C:/evidence
```

The second argument must contain the untouched original `lantern-almanac` and
`tidemark-bearing` directories. The verifier loads those original `.efk` files
through the real DX11 renderer, and the candidate through the actual package
runtime and borrowed D3D12 list. It uses a private fixed-seed test seam, matching
1/60-second steps and cameras. It checks real CPU particle counts every frame,
captures full 1920x720 PNGs and records pixel differences for eight flowers over
their lifetime, size limits, brightness, speed limits and all three bearing
positions. Nonblack union-support MAE and support overlap are checked, preventing
the black canvas from diluting image differences. It also loads the actual final
DLL and checks its public ABI, rendered pixels, pause/resume, reset and settings.
Playback tests exercise selected masks, empty sets, order loops, shuffle filtering,
fixed-choice overrides, cursor/cadence retention and the original instance cap.
Live size/brightness/dynamic-input bindings are checked on real Effekseer state
or pixels. An optional fourth argument
`warp` selects software rendering for both APIs. No user-facing debug setting is
added to the schema.

This test is independent reference evidence; actual sandbox/import/host trials
remain separate integration checks.

Pixel equality across the two stock SDK renderers is not assumed. Their linear
samplers differ: the DX11 renderer uses linear min/mag filtering with point mip
selection, while LLGI's DX12 renderer uses linear min/mag/mip filtering. Small
spark and trail sprites can therefore have different brightness at gradients as
they minify. The verifier records support-region MAE, support overlap and maximum
channel error alongside the real CPU instance counts, instead of hiding these
differences in the black canvas.
