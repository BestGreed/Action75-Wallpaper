# 黑洞 · 原生 D3D12 壁纸

`pack` 是可导入的 Windows x64 包，ID 为 `event-horizon-native`。随安装器的同款使用 `black-hole`，两处使用同一份 DLL。黑洞与吸积盘、按键恒星和远景天空均提供独立的不透明度，默认 100%。桌面透明合成需要支持此功能的播放器。

阴影外的按键点亮恒星，重复按键会积累热量，带来直接光像和延时的次像。拖动黑洞阴影可以移动中心，释放后保存到 `centerX`、`centerY`，精度为 0.1%。天空提供星云、虚空、星空和纯色四种样式。

DLL 通过公开 ABI 2 记录 D3D12 命令。星云资产由 WIC 从包内读取，星云和星场的 mip 链由 GPU 在第一帧烘焙。之后每帧按需绘制恒星和透镜发光图，合成吸积盘、HDR 光晕和色调映射。CPU 模拟与历史像素回归共用，着色器在构建阶段嵌入。MSVC 运行时静态链接。

## 构建和导入

构建依赖 Visual Studio 2022 x64、Windows SDK 和 CMake。工作目录为当前示例目录。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target wallpaper
cmake --install build --config Release --prefix "$PWD/out/event-horizon-native"
```

独立复制源码后，配置时传入 `-DACTION75_SDK=...` 指向含 `Action75Wallpaper12.h` 与 `Action75Wallpaper.h` 的公开 SDK 目录，传入 `-DJSON_INCLUDE=...` 指向含 nlohmann `json.hpp` 的目录。构建产物目录可在支持 ABI 2 的 Action75 控制台选择「导入壁纸」导入。

## 新旧对照

```powershell
cmake --build build --config Release --target verify_native_parity
.\build\Release\verify_native_parity.exe black-hole warp <ABI1旧包目录> out/event-horizon-native out/parity
```

可将 `warp` 换成 `high` 或 `low`，按 DXGI 偏好选择硬件适配器。对照程序动态加载两份 DLL，用相同时间和坐标回读像素，写出 `parity.json` 和 PPM 原始帧。D3D11 仅用于加载旧包和旧场景的回归测试。

用 `tests/verify_player.py` 可在实际播放器的隔离运行目录验证按键、拖动提交和重启持久化。该脚本需要同目录的宿主和 Worker，以及 Python、Pillow、pywin32、psutil。用 `benchmark.py` 可在同一播放器上比较新旧包的连续背景动画和每秒 40 次按键。

```powershell
python tests/verify_player.py --player <Action75Native.exe> --host-root <仓库根目录> --scene black-hole --pack out/event-horizon-native --output out/player-test
python benchmark.py --player <Action75Native.exe> --baseline-pack <ABI1旧包目录> --candidate-pack out/event-horizon-native --gpu integrated --output out/performance
```

旧包、旧源码、实测帧和本轮验收结果见仓库的 `artifacts/dx12-all/baseline/06` 与 `artifacts/dx12-all/06`。

可用 [不透明度回放](../surface-scenes/README.md) 对比两个包 ID 的默认画面，并验证 0%、50% 和 100% 的原始 RGBA 帧。
