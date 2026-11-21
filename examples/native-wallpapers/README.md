# Action75 原生壁纸 SDK

此 SDK 面向 Windows x64，支持 Direct3D 11（ABI 1）与 Direct3D 12（ABI 2）。开发时安装 CMake、Visual Studio C++ 构建工具和 Windows SDK。用户从支持原生插件的控制台导入某个作品的 `pack` 文件夹。

| 作品 | 开发与验证 |
| --- | --- |
| [潮汐信标](tidal-signal/README.md) | ABI 2 原生 D3D12 着色器、按键位置、可调光色、休眠以及跨 ABI 重连验收。 |
| [夜站拾光](key-game/README.md) | 只用公共接口实现角色移动、跳跃、碰撞、计分和休眠。可作为新作品的起点。 |
| [黑洞](black-hole/README.md) | 多阶段着色器、星云纹理、鼠标命中区域、拖动以及设置写回。 |

[D3D12 ABI 头文件](../../skills/action75-wallpaper-author/sdk/Action75Wallpaper12.h) · [D3D11 ABI 头文件](../../skills/action75-wallpaper-author/sdk/Action75Wallpaper.h) · [输入、绘制和设置约定](../../skills/action75-wallpaper-author/references/native.md)

从发行源码包构建时，在仓库根目录用 PowerShell 运行。将 `$archive` 设为要验收的源码 ZIP。需要单独构建一组时，使用 `-Project` 指定工程名。

```powershell
.\examples\native-wallpapers\verify-sdk.ps1 -Archive $archive
```

脚本将作者工程、公开头文件和固定版本依赖复制到独立目录。它执行工程自带的测试，生成可导入包及 PE 检查 JSON。水彩和 Effekseer 作者示例可使用 `-WallpaperAssets` 与 `-EffekseerArchive` 提供已恢复的素材和校验过的公开依赖，便于离线复建。

项目名包括 `atlas-packs`、`surface-scenes`、`field-keycap`、`black-hole`、`ferrofluid`、`key-game`、`dx12-probe`、`tidal-signal`、`supernova-horizon`、`night-orbit-trio`、`soundings-echo` 与 `particles`。兼容性构建 `key-game-abi1` 使用原 ABI1 夹具。
