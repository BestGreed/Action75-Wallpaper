# Action75 原生 DX12 图集与水彩壁纸

六款壁纸通过公开 `Action75Wallpaper12.h` 接口运行。包从自己的 `scene.json` 和 `settings.json` 读取资源和参数。

| 壁纸 | 独立构建目标 | 渲染实现 |
|---|---|---|
| 余像 · Afterimage | `stage_atlas_afterimage` | 2D 图集相邻帧插值和加色混合 |
| 载波 · Carrier | `stage_atlas_carrier` | 2D 图集相邻帧插值和加色混合 |
| 朱砂拓印 · Folio | `stage_atlas_folio` | 2D 图集相邻帧插值和乘色混合 |
| 潮汐玻璃 · Tidal Glass | `stage_atlas_tidal-glass` | 2D 图集相邻帧插值和加色混合 |
| 日蚀经线 · Meridian | `stage_atlas_meridian` | 连续时间的椭圆轨道、流光和淡出 |
| 水彩喷溅 · Paintball | `stage_atlas_paintball-native` | Effekseer DX12、九种原有粒子树和画布快照 |

四款图集共用一份 DLL，图集资源、构图和配色来自各自的包。Paintball 的画布以原有 1920 × 720 场景模拟三帧后烘焙。画布可见或材质需要采样时才上传。互动贴图随包保存同分辨率、完整 mip 链的 RGBA8 缓存，由同一块 1 MiB 暂存区经 COPY 队列分批上传。需要实时烘焙画布或生成 mip 时，SDK 按需创建 Direct 队列。渲染器和九棵粒子树随壁纸初始化，首键可以直接使用。包装脚本从原有 `.efkmat` 生成 DX12 材质片段，并保留同源缓存中的其他平台片段。ABI 2 运行时加载 DirectX12 片段。

继续编辑 Paintball 的原始场景或贴图时，可从 `scene.json` 移除 `canvasImage` 和 `bakedTextureDirectory`，直接预览更新后的 `.efk` 和 PNG。交付时重新生成对应缓存。

## 构建与导入

在 Windows x64 上使用 Visual Studio 2022、Windows SDK 和 CMake 3.20 或更新版本。从仓库根目录执行以下命令。

```powershell
$source = 'examples/native-wallpapers/atlas-packs'
$build = 'artifacts/atlas-dx12-build'
cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64
cmake --build $build --config Release --target action75_atlas_packs --parallel 2
```

`$build/packs/<id>` 是完整壁纸文件夹。在支持 ABI 2 的控制台里选择「导入壁纸」，指向含 `manifest.json` 的该目录。控制台对同 ID 的内置壁纸保留内置条目。若需在已有内置条目的库中并排试用，可在副本的清单中指定新的 ID。

单包构建时，把 `--target` 改成表中对应的目标。构建图集和 Meridian 时，可用 `-DACTION75_ATLAS_PAINTBALL=OFF` 跳过 Effekseer。完整构建使用固定的 Effekseer C++ 1.80.8，优先选择已解压源码，其次读取仓库附带的同版本 ZIP。构建时需要 Git 将 COPY 队列补丁应用到构建目录中的 SDK 副本。自定义路径可通过 `ACTION75_SDK_INCLUDE`、`ACTION75_SDK_VENDOR`、`ACTION75_PACK_ASSETS` 和 `ACTION75_EFFEKSEER_ROOT` 指定。独立构建只需公开 ABI 头、nlohmann/json、对应包资产，以及 Paintball 所需的 Effekseer 源码。

## 离屏对照

```powershell
cmake --build $build --config Release --target verify_atlas_dx12
python -B "$source/tests/compare.py" --probe "$build/Release/verify_atlas_dx12.exe" `
  --old 'artifacts/dx12-all/baseline/02-atlas' --new "$build/packs" `
  --output 'artifacts/atlas-dx12-comparison' --paintball-variants
```

对照程序需要 Python 的 NumPy 和 Pillow。它用冻结的 ABI 1 包和新包逐帧读回 RGBA，保存同一事件序列的截图、像素差和帧调度记录。九色水彩对照在临时副本中固定同一棵粒子树。
