# 海图回声 · 独立 D3D12 作者包

此目录可独立复制、构建。只包含公开 ABI 头、包侧实现、原 PNG 资源和随附的 nlohmann/json 头文件；运行 DLL 不使用播放器私有头文件或库，不创建 D3D11 设备，也没有 D3D11On12 桥接。

成品保留 `soundings-echo` ID 和原 `settings.json` 的全部控件 ID、范围及默认值。`reach` 调整原有 300 像素基准直径；背景样式、亮度、色相、饱和度、颜色分别保留旧包的含义。原 `echo.png` 的 6×4 共 24 格按 0.85 秒播放，相邻格做原有三维图集插值；每次触发位于宿主交付的归一化键位。最多保留 24 次触发，超过 8 次采用相同的能量预算，超过容量替换最早一次。

暂停冻结作品时钟且不接受新按键；恢复保留剩余寿命。`A75_INPUT_RESET` 清除效果，与旧宿主停止特效时发送的重置事件一致。无存活效果返回 `-1`，等待新输入；参数变化可唤醒并重画背景。

## 独立构建及安装成品

需要 Windows x64、Visual Studio 2022 C++ 工具、Windows SDK 和 CMake。在本目录执行，输出目录自行选择：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix pack
```

把生成的 `pack` 文件夹整体导入支持 ABI2 的 Action75 Wallpaper。运行仅需要一份 `wallpaper.dll`、四份预编译 shader、原 PNG、参数、清单和许可文件。DLL 使用静态 MSVC 运行库；安装后不编译着色器。可以通过 `ACTION75_SDK_INCLUDE` 与 `ACTION75_JSON_INCLUDE` 更换公开 SDK／JSON 头目录。

## 验证和旧版对照

默认 `verify_soundings` 直接创建独立 D3D12 WARP 设备，加载已安装的 ABI2 DLL，回读 1920×720 RGBA8。检查真实图集出帧、键位局部范围、大小两端的像素宽度、32 次触发、过期空闲、按键释放、暂停时钟和输入重置。测试不创建桌面窗口，不连接正在运行的播放器。

仓库迁移验收另冻结了旧版 `SpriteBurst.cpp/.h`、状态、shader 与背景 shader。需要精确对照时，将其路径传给 `SOUNDINGS_LEGACY_REFERENCE`：

```powershell
cmake -S . -B reference-build -G "Visual Studio 17 2022" -A x64 -DSOUNDINGS_LEGACY_REFERENCE="<冻结旧实现的目录>"
cmake --build reference-build --config Release --parallel 2
ctest --test-dir reference-build -C Release -V
```

该可选参数只为验证目标链接旧 D3D11 参考实现；不改变运行 DLL 的源码、链接或 GPU 后端。参考检查覆盖九个动画时间点、大小两端、图片与纯色背景调色、并发容量、恢复后的时钟、清空和过期。JSON 与成对 PNG 输出在构建目录的 `evidence`。正式验收记录位于仓库 `artifacts/dx12-all/08/soundings`；宿主导入与硬件／实体副屏检查由总体验收补充。

公开 ABI 头和 JSON 头可从随附 `sdk`／`vendor` 离线使用。JSON 的 MIT 许可和项目 Apache-2.0 许可均随成品安装。
