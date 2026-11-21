# 原生 D3D12 互动壁纸

这里的三款壁纸使用 Action75 公开原生插件 ABI 2。每套作品的交互、渲染、着色器都编入各自的 `wallpaper.dll`。设置面板和预览素材来自 `wallpapers-native` 下的同名包。

| 包 | 进入壁纸后的操作 | 调整位置 |
| --- | --- | --- |
| `stillwater` 水波 | 按 Action75 键或点击副屏，涟漪从触点扩散并叠加 | 「涟漪」控制力度区间、波速、衰减和波峰。「水面」控制背景、纹理、焦散及环境运动。 |
| `night-fireworks` 烟火 | 按键按规则推进花型，点击副屏可直接放一次 | 「播放花型」选择十种花型、组合轮播、随机及按键间隔。「烟花」和「夜景」控制粒子、配色、辉光和倒影。 |
| `glass-shatter` 碎光玻璃 | 按键在键位处产生裂纹，重复敲击会继续延展原裂纹 | 「碎裂」调整随机力度上下限、整体扩展、中心破碎范围、分叉上限和持续时间。「玻璃材质」调整裂缝、反光、折射和碎片。 |

## 独立构建与导入

使用 Windows x64、Visual Studio 2022 C++ 工具链、带 `fxc.exe` 的 Windows SDK 和 CMake 3.24 或更新版本。在仓库根目录运行以下 PowerShell 命令。

```powershell
$build = Join-Path $env:TEMP 'action75-surface-author'
$packs = Join-Path $build 'packs'
$id = 'glass-shatter'
cmake -S examples/native-wallpapers/surface-scenes -B $build -G 'Visual Studio 17 2022' -A x64
cmake --build $build --config Release --target "wallpaper_$id"
cmake --install $build --config Release --prefix $packs --component $id
```

将 `$id` 换成 `stillwater` 或 `night-fireworks` 即可单独构建对应作品。每次生成可导入的 `$packs/$id` 文件夹，内含清单、面板、预览和 DLL。使用支持 ABI 2 的播放器，在「导入壁纸」中选择这个文件夹。CMake 可通过 `ACTION75_SURFACE_SDK`、`ACTION75_SURFACE_JSON`、`ACTION75_SURFACE_PACKS` 指定公开 SDK、nlohmann/json 头文件和包素材的位置。

## 验证

使用作者验收脚本在独立测试库导入作品并保存真实帧。将 `$player` 设为播放器可执行文件的路径。[preview.py](../../../skills/action75-wallpaper-author/scripts/preview.py)

```powershell
python skills/action75-wallpaper-author/scripts/preview.py --player $player --pack (Join-Path $packs $id) --frame (Join-Path $build "$id.png")
```

有 ABI 1 版成品目录时，可以用相同的键位、设置和时间比较新旧帧。以下测试需 Python 和 Pillow，`$baseline` 指向包含三款 ABI 1 作品目录及 `reference` 源码快照的备份目录。先按上节命令把三款 ABI 2 作品装入 `$packs`。

```powershell
cmake -S examples/native-wallpapers/surface-scenes -B $build -DACTION75_SURFACE_REPLAY=ON
cmake --build $build --config Release --target surface_replay
$replay = Join-Path $build 'Release/surface_replay.exe'
python examples/native-wallpapers/surface-scenes/tests/compare.py --replay $replay --baseline $baseline --packs $packs --output (Join-Path $build 'comparison')
```

回放程序只在本地离屏运行两个 ABI。它读取实际渲染帧，在可用时开启 D3D12 调试层，并检查按键节奏、空闲、调整尺寸和像素差。`compare.py` 会为每款作品保存时间线、读回帧和指标。碎光玻璃每次创建时会选用独立的随机种子，正式 DLL 的活动帧检查力度差异、中心尺寸和材质响应。固定种子的活动帧对照使用同一份备份和临时编译目录。

```powershell
$fxc = (Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') -Pattern '^ACTION75_SURFACE_FXC:FILEPATH=').Line.Split('=', 2)[1]
python examples/native-wallpapers/surface-scenes/tests/fixed_glass_seed.py --fxc $fxc --replay $replay --baseline $baseline --packs $packs --output (Join-Path $build 'fixed-seed')
```

作者接口契约和宿主运行方式见 [Action75 原生插件制作指南](../../../skills/action75-wallpaper-author/references/native.md)。
