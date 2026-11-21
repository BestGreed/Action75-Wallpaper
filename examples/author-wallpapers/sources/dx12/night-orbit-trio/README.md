# 夜航三景 · 原生 D3D12

此目录可单独复制到仓库外构建。`sdk` 是公开 ABI 2 头文件，`vendor` 是固定的 nlohmann/json 单头文件及许可；不读取播放器私有头文件或链接播放器库。成品清单、控件 ID、设置默认值位于 `pack`。运行包仅需要 DLL、两个 JSON 和六份预编译着色器。

## 构建可导入包

需要 Windows x64、Visual Studio 2022 C++ 工具、Windows SDK 与 CMake 3.24 以上。在此目录运行：

```powershell
cmake -S . -B "$env:TEMP/night-orbit-build" -G "Visual Studio 17 2022" -A x64
cmake --build "$env:TEMP/night-orbit-build" --config Release --parallel 2
ctest --test-dir "$env:TEMP/night-orbit-build" -C Release --output-on-failure
cmake --install "$env:TEMP/night-orbit-build" --config Release --prefix "$env:TEMP/night-orbit-trio"
```

将最后生成的 `night-orbit-trio` 文件夹导入支持 ABI 2 的 Action75 Wallpaper。只构建作品时可加 `-DBUILD_TESTING=OFF`；验证器中的 D3D11 代码仅用于与旧画面对照，不会链接进 `wallpaper.dll`。如需替换公开 SDK 或 JSON 单头文件，配置 `ACTION75_SDK_INCLUDE` 和 `ACTION75_SDK_VENDOR`。

## 效果与规则

- 保留原始 FireworksScene 的确定性花型种子、各粒子运动、寿命、拖尾、余烬、HDR 叠加、三次低分辨率辉光过滤、烟雾、夜空和水面倒影。`PatternData.inc`、`Simulation.inc` 与 `Fireworks.hlsl` 来自冻结的真实原场景；GPU 资源和提交改用 D3D12。
- 默认每两次物理按键按 **土星 7 → 垂柳 1 → 棕榈 4** 循环。沿用 `sky_program`、三个候选开关、`mix_stars`、`tap_cadence`（1–99）及全部背景控件。单选与多选、空多选、随机和指针触发均按原规则执行。
- 暂停冻结作品时钟；暂停中可以改变背景外观，烟花年龄不推进。恢复扣除暂停墙钟，输入重置清除当前烟花。无活动烟花时返回 `-1`，下一次事件再唤醒。
- 宿主只传公开事件、用户参数和 D3D12 命令列表；作品没有按壁纸 ID 分支、ABI 1 导出、D3D11On12 或隐藏的 D3D11 渲染路径。静态链接 MSVC 运行库。

## 对照验证

`tests/reference` 冻结了迁移前的真实 D3D11 FireworksScene，仅把其外观类型换成本目录中相同的数据结构。该验证器不访问桌面、键盘、副屏或正在运行的播放器。

验证在独立 WARP D3D11 与 D3D12 设备上逐帧回读 **1920×720 RGBA8**。35 个样本覆盖三种花型的六个时间点、默认轮放、99 键间隔、指针、子集与随机、背景调色、暂停中改设置、恢复、重置及消散后的休眠。`build/evidence/metrics.csv` 给出逐通道误差，并保存主要样本的新旧 PNG。正式迁移验收的全部 35 样本误差为 0；该结果限于本机 WARP，实体副屏和长时硬件负载需另验。
