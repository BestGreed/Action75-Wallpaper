# 磁流体 · 局部表面重建版

将同级 **Ferrofluid** 文件夹导入支持公开原生 ABI 2 的控制台。Source 包含 C++ 和 HLSL，Preview/interaction.webp 展示单键吸引、多键竞争、依次松开和回流。包 ID 沿用 `meniscus-ferrofluid-v1`。

磁流体与背景的不透明度可独立设置为 0–100%，初始均为 100%。背景设为 0% 时，支持桌面透明合成的播放器会在液体周围显示桌面，长按和换键仍然生效。

## 构建

需要 Windows x64、Visual Studio 2022 C++ Build Tools、Windows SDK 和 CMake。在本目录运行以下命令。脚本在临时目录生成可导入的独立包。完整源码包附有公开头文件和 nlohmann/json 3.12.0。单独复制示例时，可用 `-SdkInclude` 指向两个公开头文件所在目录，用 `-SdkVendor` 指向包含 `nlohmann/` 的目录。

~~~powershell
$work = Join-Path $env:TEMP "Action75-Ferrofluid-$([guid]::NewGuid())"
./Source/build.ps1 -BuildDir "$work/build" -Output "$work/pack"
~~~

原作者最新版工程为 action75-ferrofluid-studio，基线 DLL 的 SHA-256 为 032686139ae184b28d4df138b8d60f2dd54139602f4ebed4c465cb34b7fd704c。该作品的浅色器皿、黑色液面与反射由程序绘制，原控制项和 192×72 守恒求解器均保留。优化重复使用四行三次插值结果，只清除前一帧液体的矩形，并上传前后液体矩形的并集。绘制阶段筛掉全干的网格块，背景跳过深度写入。初始化仍会写入一次完整的 961×361 高度纹理。

运行 `$work/build/Release/visual_check.exe <包目录> <键位校准 JSON> <帧输出目录>`，按固定时间轴使用 D3D12 WARP 回读按键交互。用相同回读器运行 `python Source/test_appearance.py <visual_check.exe> <键位校准 JSON> <输出目录> --pack <包目录>`，可以复验两层共十二项外观控件。

## ABI 1 局部表面重建的历史实测

Intel UHD Graphics，1920×720，原审计 integration-build 播放器，隔离实例。按原审计顺序轮流发送 9 个键码，每秒 40 次 testTrigger，每轮 4 秒，前后采集空闲和暂停。两轮交叉顺序为旧、新、新、旧。原审计包 DLL 前缀 f6f1f9cd，局部表面重建版 DLL 前缀 ee24be33。原始逐进程和 GPU 引擎样本见 Measurements/trial-*.json。

| 指标 | 旧包两轮 | 新包两轮 |
|---|---:|---:|
| 工作进程 CPU，单逻辑核 % | 12.50、12.90 | 11.30、8.55 |
| 播放器 CPU，单逻辑核 % | 1.55、0.39 | 1.54、1.55 |
| 合计 GPU 引擎毫秒/帧 | 1.973、2.354 | 2.171、2.111 |
| FPS | 60.22、60.22 | 60.22、60.22 |
| 工作进程峰值私有内存 MiB | 70.79、70.80 | 72.10、70.52 |
| 两进程合计峰值私有内存 MiB | 119.38、120.07 | 121.41、119.85 |
| 空闲 3 秒 / 暂停 2 秒新增帧 | 0 / 0 | 0 / 0 |

两轮平均工作进程 CPU 从 12.70% 降至 9.93%，相对降低约 22%。合计 GPU 引擎平均从 2.164 降至 2.141 ms/帧，变化小于轮间波动。工作进程峰值私有内存增加约 1.3 MiB。先前记录的 27.7% 未在本次旧包复测中重现。CPU 单独测量中，缓存行与局部清除将原作者版表面重建从 1.396 降至 0.313 ms/帧（360 帧、40 次按下/秒、同时保留四磁极的合成序列）；该微基准与播放器测试有不同输入。

## 回读及复核

Windows WARP 通过 SDK ABI 在固定 60 Hz 时间轴注入 A、L、E 三个按下和反序松开。五次 1280×480 RGBA 回读（静止、单键、多键、释放最新键、全部释放）与作者最新版逐字节一致，五帧所有通道最大误差均为 0。详情见 Measurements/accuracy.json。Preview 包含对应 PNG 和短动图。物理守恒审计覆盖远距离迁移、竞争、释放和回流，记录的最大相对质量误差为 2.32×10⁻⁷，最后一条记录进入休眠。

迁移前的 ABI 1 源码及回读工具保存在 `artifacts/dx12-all/baseline/04/ferrofluid`。迁移对照使用 `artifacts/dx12-all/04/qa/compare.cpp`，历史对照数据保存在本目录的 Measurements 和 Preview。

## 前景与背景外观

液体与底板分别提供材质、亮度、色相、饱和度和 RGB 颜色。默认以黑磁液搭配青瓷，亮度和饱和度均为 1，色相为 0°，颜色为白色。旧底板材质的 tone ID 和已有取值沿用。液滴半径、磁场强度、吸附速度与尖峰高度常驻面板，黏性、张力、反光等细调在高级选项。外观更新时预计算底板色和液体彩色变换，运动帧继续使用缓存。

ABI 1 的历史外观对照使用 DLL SHA-256 8a8c6c2daa4bca304c6fefcb5299f8ce2405ed58eea9279332997188d6cfa125。WARP 对两层共十个设置逐项发出 A75_SETTINGS 并回读，包括各自的 RGB(255,68,36)。每次只有对应层的采样发生变化。默认参数生成的五张交互帧与原作者基线逐字节相同。历史逐项数据见 Measurements/appearance.json。

暂停时保留液面位置，调整材质可立即更新画面。重载校准会清除按键输入，流体继续保持暂停，恢复后再推进。可从项目根目录运行 `tools/verify-wallpaper-behavior.py` 回读暂停、调色、输入重置和恢复的画面。

在上述 1920×720、每秒 40 次 testTrigger 条件下，对外观更新前后各补测一次，工作进程 CPU 为 8.19% / 7.36% 单逻辑核，合计 GPU 引擎耗时为 2.35 / 1.93 毫秒每帧，FPS 为 60.23 / 60.22。单轮结果用于检查回退，原始数据见 Measurements/appearance-perf-*.json。
