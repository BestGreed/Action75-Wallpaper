# 奇点余烬

铜金吸积盘围绕黑洞形成弯曲光环。每次敲键都在对应键位点亮一颗超新星，冷白闪光扩展成蓝紫冲击波，随后抛出的火星逐渐熄灭。「深空」下可以在星云和虚空之间切换。

## 导入

在支持 D3D12 原生 ABI 2 的 Windows x64 版 Action75 Wallpaper 控制台中，选择「导入」并指向 [pack](pack) 文件夹。导入后选择「奇点余烬」，按任一已校准键位即可触发爆发。

| 对象 | 可调整参数 |
| --- | --- |
| 超新星 | 亮度、色彩滤镜、爆发范围、持续时间 |
| 黑洞与吸积盘 | 吸积盘亮度 |
| 深空 | 星云／虚空、亮度、色相、饱和度、色彩滤镜 |

连按时场景最多同时保留 8 次爆发，每次爆发由一片冲击波和 36 条火星构成。第 9 次同时爆发会接替最早的一次。黑洞与天空在独立纹理中缓存，1920×720 输出时缓存为 1280×480 RGBA8，像素数据约 2.34 MiB。空闲时回到事件驱动调度。

## 构建

需要 Visual Studio 2022 的 C++ 桌面开发组件、Windows SDK 和 CMake 3.24 或更新版本。公开 ABI 头文件已包含在 `sdk`。构建使用 Windows SDK 的 `fxc.exe`，可用 `-DFXC=<绝对路径>` 指定编译器。以下命令从本目录的 PowerShell 运行，产物放在独立临时目录。

```powershell
$buildRoot = Join-Path $env:TEMP ('supernova-horizon-' + [guid]::NewGuid().ToString('N'))
cmake -S . -B "$buildRoot/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$buildRoot/build" --config Release --parallel
cmake --install "$buildRoot/build" --config Release --prefix "$buildRoot/pack"
```

`source/wallpaper.cpp` 实现输入、设置和 D3D12 绘制。`shaders/sky.hlsl` 控制黑洞与背景，`shaders/supernova.hlsl` 控制按键效果。`metadata` 保存包清单、参数和实际读回帧制成的封面。

## 试播与验收

将 `$hostRoot` 替换为匹配版本的播放器目录。目录需含播放器、同版 Worker、默认校准数据和键位映射。将 `$skill` 指向 Action75 壁纸作者技能目录。下面从本目录执行，使用另建的临时库和命名管道。运行 `verify.py` 前在作者机的 Python 环境安装 Pillow。

```powershell
$hostRoot = 'D:/Path/To/Action75'
$skill = 'C:/Path/To/action75-wallpaper-author'
$player = Join-Path $hostRoot 'dist/Action75Native/Action75Native.exe'
$trial = Join-Path $env:TEMP ('supernova-trial-' + [guid]::NewGuid().ToString('N'))

python "$skill/scripts/preview.py" --player $player --pack ./pack --frame "$trial/import.png"
python tools/verify.py --skill $skill --player $player --host-root $hostRoot --pack ./pack --output "$trial/pixels"
```

`verify.py` 保存像素读回和 `report.json`。要使用播放器选择的硬件适配器，在最后一行添加 `--hardware`。

### 实测

- Windows SDK 10.0.26100.0 和 MSVC 19.44 从空构建目录生成的包与随附成品都通过播放器离屏试播。RTX 4060 的导入试播报告为 D3D12、触发成功、6 帧。
- 1920×720 WARP 读回中，T / M 键位附近的峰值通道增量为 240 / 254，星云与虚空的全画面最大通道差为 208。恢复默认设置后，静态画面与首次读回逐像素一致。爆发半径取 0.55 / 1.45 时，读回的冲击波差异范围宽为 62 / 171 像素。爆发淡出后两次空闲采样之间无新增帧。
- 1920×720 Intel UHD 硬件离屏测试中，星云和虚空两种背景各连续触发 8 秒、192 次。两轮平均均为约 59.99 FPS，空闲 1 秒均无新增帧。

试播宿主的 SHA-256 为 `C1804518C5511A1601A02D6E1FEA4472762132DE88A96FD591AF7C97E9A88949`，Worker 为 `7C135B87740C97A1149E5B950FFCAC1EF41BBB0F8968EB67C5AD832F15A0B17A`。
