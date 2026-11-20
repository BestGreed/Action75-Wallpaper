# Action75 原生壁纸 SDK v1

此 SDK 面向 Windows x64 和 Direct3D 11。开发时安装 CMake、Visual Studio C++ 构建工具和 Windows SDK。用户从支持原生插件的控制台导入某个作品的 `pack` 文件夹。

| 作品 | 开发与验证 |
| --- | --- |
| [夜站拾光](key-game/README.md) | 只用公共接口实现角色移动、跳跃、碰撞、计分和休眠。可作为新作品的起点。 |
| [黑洞](black-hole/README.md) | 多阶段着色器、星云纹理、鼠标命中区域、拖动以及设置写回。 |

[ABI 头文件](../../native/windows-player/include/Action75Wallpaper.h) · [输入、绘制和设置约定](../../skills/action75-wallpaper-author/references/native.md)

两个工程独立构建，可以从 SDK 目录执行以下命令验证游戏：

```powershell
cmake -S examples/native-wallpapers/key-game -B build/game -A x64
cmake --build build/game --config Release
ctest --test-dir build/game -C Release --output-on-failure
```
