# 潮汐信标

按下任意键，其校准位置会出现信标光核和一道扩散波纹。约两秒后光核消失，作品请求休眠。“信标”组中的发光颜色和亮度直接控制光核。成品目录为 `pack`；着色器源码在 `src/tide.hlsl`，构建时将顶点与像素入口分别编译为可随包分发的 CSO。背景的潮线同样由着色器绘制。

## 构建

使用 Windows x64、Visual Studio 2022 C++ 桌面组件和 Windows SDK。完整源码包提供 nlohmann/json 3.12.0，并可直接执行下列构建命令。

```powershell
$example = "examples/native-wallpapers/tidal-signal"
$build = "artifacts/dx12-chain/author/build"
cmake -S $example -B $build -G "Visual Studio 17 2022" -A x64
cmake --build $build --config Release
ctest --test-dir $build -C Release --output-on-failure
cmake --install $build --config Release --prefix "$example/pack"
```

单独复制工程时可设置 `ACTION75_SDK_INCLUDE` 和 `ACTION75_SDK_VENDOR`，分别指向两份公共 ABI 头文件所在目录和包含 nlohmann/json.hpp 的目录。控制台导入时选择生成的 `pack`。作者使用编译工具，成品只需 DLL、manifest、参数和两个 CSO。

## 渲染接口

本例使用公开的 ABI 2。初始化时从包内加载着色器并创建管线状态和根签名。每帧向 worker 提供的命令列表写 10 个 32 位常量，再绘制覆盖目标的三角形。画面使用单采样 RGBA8 UNORM。GPU 上无需全屏上传缓冲或作者私建的 D3D11 设备。睡眠、键位、暂停和设置事件共用 ABI 1 的事件结构。

## 独立验收

CTest 组装临时包，再动态加载发行 DLL，创建真实 D3D12 WARP 设备并从目标纹理读回像素。默认光核 RGB 为 (63,190,161)；将颜色设为 [240,60,20]、亮度设为 0.5 得到 (120,30,10)。测试同时检查远离键位的背景、输入重置、淡出与休眠返回值。

`verify_player.py` 通过私有命名管道启动离屏播放器，需要 Python 3、Pillow 和 psutil。硬件模式另需 pywin32。请将下方 `$player` 设为 ABI 2 测试版 Action75Native.exe 的完整路径，其旁应有同一构建的 Action75WallpaperWorker.exe。

```powershell
python "$example/verify_player.py" --player "$player" --output artifacts/dx12-chain/author/warp
python "$example/verify_player.py" --player "$player" --hardware --output artifacts/dx12-chain/author/hardware
```

脚本检查 ABI 2 键位像素和调色，切换到 ABI 1 的“夜站拾光”并核对琥珀色，在切回后模拟副屏离线、等待 worker 退出，再验证重连首键和保存的设置。输出包含 PNG、独立进程的私有内存和工作集，以及硬件模式下按 PID 与适配器 LUID 匹配的专用和共享显存。跨进程共享的分配可能计入两个进程的 GPU 计数。
