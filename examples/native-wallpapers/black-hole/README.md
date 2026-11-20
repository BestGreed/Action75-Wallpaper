# 黑洞 · 独立 D3D11 壁纸

`pack` 是完整发行目录。它包含 x64 的 wallpaper.dll、原版星云与封面、设置清单和原始素材来源说明。包 ID 为 event-horizon-native。原有 19 个控件沿用各自的 ID、单位、范围和默认值。新增 7 个外观控件，可分别调整天空和按键恒星的亮度、色相、饱和度与颜色。拖动时仅黑洞阴影响应鼠标，释放后提交 centerX、centerY，精度 0.1%。阴影外的按键点亮恒星，重复同一键会积累热量。可选星云、虚空、星空和纯色四种天空，吸积盘另有三套配色。

渲染源码及 HLSL 源于 Action75 的 BlackHoleScene 与 BlackHole.hlsl。迁移将配置类型收窄到该作品使用的外观字段，将纹理解码、输入、拖动和参数处理放进 DLL。星体轨迹、热衰减、引力透镜、HDR 与辉光通道继续使用原实现。纹理通过包目录读取。公开 SDK、Windows SDK 和 nlohmann/json 单头文件仅为构建依赖。构建工具用 D3DCompiler 将十个着色器入口编译为 DX11 字节码，再将字节码链接进 DLL。播放时使用 D3D11 和 WIC，MSVC 运行时静态链接。

## 构建和独立验证

在此目录用 Visual Studio 2022 x64 和 CMake 运行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target verify_black_hole
.\build\Release\verify_black_hole.exe pack
```

CMake 在构建后将 wallpaper.dll 放入 pack。源码单独复制到其他工程后，可设置 ACTION75_SDK 指向含 Action75Wallpaper.h 的目录，JSON_INCLUDE 指向含 json.hpp 的目录。WARP 自测动态加载发行 DLL，用 640×240 的 BGRA8 目标读回画面，验证三种天空、按键恒星、命中区域、拖动的位置补丁和取消拖动时恢复中心。

## 硬件性能对照

`benchmark.py` 需要 Python、psutil 与 pywin32。基线为旧宿主和原生 blackHole 包，实验组为支持本机壁纸的新版宿主和此 pack。两个进程都采用私有测试配置、离屏硬件渲染、指定集成 GPU、1920×720、60 FPS。每轮先采样背景动画，再依次向九个阴影外的键位发送每秒 40 次输入。三轮交错调整先后次序。

```powershell
python benchmark.py --player <新版Action75Native.exe> --baseline-player <旧版Action75Native.exe> --baseline-pack <原版black-hole目录> --host-root <含校准和键位表的目录> --output <测试记录目录>
```

脚本用 Windows GPU Engine 计数器按适配器 LUID 和进程 PID 独立统计每个引擎的忙碌时间，然后计算每个呈现帧的合计引擎毫秒数。并行引擎的忙碌时间可重叠。报告另含玩家与 worker 的 CPU、父子进程工作集、共享及专用显存，以及父进程 GPU 时间戳。进程显存之和可能包含共享纹理的重复记账。测量避开用户会话，无需切换显示器。

## 本机核显结果

Intel UHD Graphics，1920×720，目标 60 FPS，各组三轮交错采样。表中 GPU 是按进程累加的引擎忙碌毫秒/实际帧；内存为每轮采样峰值的中位数。DLL 列包含播放器和隔离 worker 两个进程。

|负载|版本|FPS|GPU ms/帧|峰值工作集 MiB|峰值共享显存 MiB|
|---|---|---:|---:|---:|---:|
|背景动画|原版|60.00|2.74|127.9|43.8|
|背景动画|DLL|60.16|3.37|159.9|60.0|
|40 键/秒|原版|60.00|3.12|87.8|43.9|
|40 键/秒|DLL|60.00|3.81|143.8|60.1|

同一按键在第 430 毫秒的旧版与 DLL 实际回读，RGB 通道平均绝对差为 0.020/255。集成宿主离屏拖动中心从 (58%,46%) 到 (42%,66%) 后重新选包，控制台参数保留新坐标。物理副屏及实体鼠标操作未纳入测量。性能试验宿主 SHA-256 为 4b6ca28b2e3946292b11caf434994cb4bb5bf240f4b16f603858fb9cfa3edea6，worker 为 916637505157e70af064943f1c7f88f778ee127fcb2e765964b91ef07d5cf100。
