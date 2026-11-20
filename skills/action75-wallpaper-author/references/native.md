# Windows 原生交互壁纸

使用原生 SDK 版本的 Windows x64 播放器。宿主从包中读取清单和参数，在独立工作进程中运行作者提供的 DLL。开发时使用 SDK 的 `Action75Wallpaper.h`、Windows SDK 和 C++ 编译器。用户导入成品文件夹即可。

```json
{"version":1,"id":"my-stage","name":"舞台","kind":"native","abi":1,"entry":"wallpaper.dll","settings":"settings.json"}
```

`entry` 可以写包内相对路径，例如 `bin/sparks.dll`。导入时只允许这一份 DLL，发布和更新后保留其子目录结构。给参数声明稳定的控件 ID。四个回调通过一个导出函数 `Action75GetWallpaper` 返回；返回表要在 DLL 生命周期内有效。可用模块定义文件导出此名称，或在包含头文件前定义 `ACTION75_WALLPAPER_EXPORTS`。代码参考源码包中的 `examples/native-wallpapers/key-game`。这份示例只依赖公共 SDK，包含独立构建及绘制测试。

- `create` 接收尺寸、D3D11 设备、上下文、只读资源目录和按控件 ID 索引的 JSON 值。返回作品实例。材质、几何、纹理在初始化时准备。建议把作者着色器预编译后随作品发布。
- `on_event` 接收按下、松开、指针、参数变化和挂起恢复。键码为物理键名，指针和键位坐标按宽高归一化。按住状态和游戏分数保存在作品实例中。收到输入重置时清除尚未释放的键。用户拖动改变作者参数时，可在输入回调内用 `commit_settings` 提交已声明控件的 JSON 对象。
- `render` 绘制到当次回调提供的 RTV。作品负责绑定目标、视口和所需流水线状态。按帧使用单调时钟。返回 0 请求下一帧，正整数表示等待毫秒数，−1 表示静止至下次事件。尺寸或设备变更时宿主可以重建实例。
- `destroy` 释放资源。可选的 `hit_test` 声明作品接收鼠标的区域，返回零的区域可透传。键盘副屏保护等设备策略由宿主处理。

将 DLL、相对路径资源和两个 JSON 放在同一个作品目录，用同版本播放器离屏试播；播放器旁边需有配套的 `Action75WallpaperWorker.exe`。验证按下与松开、暂停恢复、静止时帧计数、设置变动和重新载入。记录核显资源消耗时同时测播放器和绘制作品的工作进程。

需要关联最小值和最大值时，可以在参数 JSON 顶层声明 `"orderedRange": ["impactMin", "impactMax"]`。这两个 ID 必须指向取值范围相同的滑块。单独拖动一端越过另一端时，两端会一起移动。适用于碎玻璃和水波一类的随机力度区间。

## 从源码构建一份可导入的原生包

作者机需要 Windows x64、Visual Studio 2022 的 C++ 桌面组件、Windows SDK 和 CMake。取得随项目分发的源码或含示例的 SDK，找到 `examples/native-wallpapers/key-game`。该工程通过公开 ABI 接入播放器，测试覆盖角色移动、碰撞和设置后的像素。以下命令在源码根目录的 PowerShell 中运行，输出使用独立目录。

```powershell
$work = Join-Path $env:TEMP ('action75-native-' + [guid]::NewGuid().ToString('N'))
cmake -S examples/native-wallpapers/key-game -B "$work/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$work/build" --config Release
ctest --test-dir "$work/build" -C Release --output-on-failure
cmake --install "$work/build" --config Release --prefix "$work/pack"
```

安装步骤把 DLL、清单和参数放在 `$work/pack`。在 skill 根目录使用 `scripts/preview.py` 对这个成品试播。单独复制示例时，配置 CMake 的 `ACTION75_SDK_INCLUDE` 指向公开头文件目录、`ACTION75_SDK_VENDOR` 指向含 `nlohmann/json.hpp` 的目录。

制作自己的新作品时，先设置独立的包 ID。可保留示例的导出入口和构建目标，用自己的场景接替 `source/game.hpp` 里的模拟，接着在 `source/wallpaper.cpp` 中连接输入、参数和绘制。先验证一个按键动作，再加第二个对象或着色器。把两组测试分别保留：模拟测试检查状态变化，WARP 绘制测试检查目标像素和空闲调度。若资源由着色器读取，将编译产物按作品路径随包安装，同时在清空构建目录后重新试播成品。
