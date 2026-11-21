# Windows 原生交互壁纸

使用原生 SDK 版本的 Windows x64 播放器。宿主从包中读取清单和参数，在独立工作进程中运行作者提供的 DLL。本 skill 的 [sdk 目录](../sdk/) 提供两个公开头文件。D3D11 作品包含 `Action75Wallpaper.h`，D3D12 作品包含 `Action75Wallpaper12.h`，两个头文件放在同一目录。构建需要 Windows SDK 和 C++ 编译器。用户导入成品文件夹即可。

```json
{"version":1,"id":"my-stage","name":"舞台","kind":"native","abi":1,"entry":"wallpaper.dll","settings":"settings.json"}
```

`entry` 可以写包内相对路径，例如 `bin/sparks.dll`。导入时只允许这一份 DLL，发布和更新后保留其子目录结构。下面的清单和回调说明介绍 ABI 1；当前夜站拾光示例使用 ABI 2，见[ABI 2 制作方法](#d3d12-与-abi-2)。需要复验旧接口时使用源码包中的 `tools/fixtures/key-game-abi1`。

给参数声明稳定的控件 ID。ABI 1 的四个回调通过一个导出函数 `Action75GetWallpaper` 返回；返回表要在 DLL 生命周期内有效。可用模块定义文件导出此名称，或在包含头文件前定义 `ACTION75_WALLPAPER_EXPORTS`。ABI 1 代码参考源码包中的 `tools/fixtures/key-game-abi1`；当前 ABI 2 示例位于 `examples/native-wallpapers/key-game`。两者只依赖公共 SDK，包含独立构建及绘制测试。

- `create` 接收尺寸、D3D11 设备、上下文、只读资源目录和按控件 ID 索引的 JSON 值。返回作品实例。材质、几何、纹理在初始化时准备。建议把作者着色器预编译后随作品发布。
- `on_event` 接收按下、松开、指针、参数变化和挂起恢复。键码为物理键名，指针和键位坐标按宽高归一化。按住状态和游戏分数保存在作品实例中。收到输入重置时清除尚未释放的键。用户拖动改变作者参数时，可在输入回调内用 `commit_settings` 提交已声明控件的 JSON 对象。
- `render` 绘制到当次回调提供的 RTV。作品负责绑定目标、视口和所需流水线状态。按帧使用单调时钟。返回 0 请求下一帧，正整数表示等待毫秒数，−1 表示静止至下次事件。尺寸或设备变更时宿主可以重建实例。
- `destroy` 释放资源。可选的 `hit_test` 声明作品接收鼠标的区域，返回零的区域可透传。键盘副屏保护等设备策略由宿主处理。

将 DLL、相对路径资源和两个 JSON 放在同一个作品目录，用同版本播放器离屏试播；播放器旁边需有配套的 `Action75WallpaperWorker.exe`。验证按下与松开、暂停恢复、静止时帧计数、设置变动和重新载入。记录核显资源消耗时同时测播放器和绘制作品的工作进程。

需要关联最小值和最大值时，可以在参数 JSON 顶层声明 `"orderedRange": ["impactMin", "impactMax"]`。这两个 ID 必须指向取值范围相同的滑块。宿主在首次载入和恢复默认时按从小到大排列两个值。单独拖动一端越过另一端时，两端会一起移动。

原生作品按 ID 读取作者值，普通控件可以省略 `binding`。使用源码中的参数校验器时，传入 `--check-settings --kind native <settings.json>`，以同样的规则验证 ABI 1 和 ABI 2。

## 从源码构建一份可导入的原生包

作者机需要 Windows x64、Visual Studio 2022 的 C++ 桌面组件、Windows SDK 和 CMake。解压完整源码包，确认其中有 `vendor-source/windows-native/nlohmann/json.hpp`（3.12.0）和 `Licenses/nlohmann-json-LICENSE.txt`。两个 ABI 示例都使用该头文件。以下命令在源码根目录的 PowerShell 中运行，构建当前 ABI 2 夜站拾光。构建 ABI 1 兼容夹具时，把 `-S` 后的目录改为 `tools/fixtures/key-game-abi1`。

```powershell
$work = Join-Path $env:TEMP ('action75-native-' + [guid]::NewGuid().ToString('N'))
cmake -S examples/native-wallpapers/key-game -B "$work/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$work/build" --config Release
ctest --test-dir "$work/build" -C Release --output-on-failure
cmake --install "$work/build" --config Release --prefix "$work/pack"
```

安装步骤把 DLL、清单和参数放在 `$work/pack`。在 skill 根目录使用 `scripts/preview.py` 对这个成品试播。单独复制示例时，配置 CMake 的 `ACTION75_SDK_INCLUDE` 指向公开头文件目录，`ACTION75_SDK_VENDOR` 指向包含 `nlohmann/` 的目录。MSVC 示例静态链接 Visual C++ 运行库，成品按单入口 DLL 交付。

从发行源码 ZIP 验证两个 ABI 时，可在源码根目录运行 `examples/native-wallpapers/verify-sdk.ps1 -Archive <ZIP 路径>`。它在独立目录解压、构建、安装作者包，跑 WARP 读回并检查成品 DLL 的运行库依赖。

制作自己的新作品时，先设置独立的包 ID。可保留示例的导出入口和构建目标，用自己的场景接替 `source/game.hpp` 里的模拟，接着在 `source/wallpaper.cpp` 中连接输入、参数和绘制。先验证一个按键动作，再加第二个对象或着色器。把两组测试分别保留：模拟测试检查状态变化，WARP 绘制测试检查目标像素和空闲调度。若资源由着色器读取，将编译产物按作品路径随包安装，同时在清空构建目录后重新试播成品。

## D3D12 与 ABI 2

面向支持 D3D12 作者接口的播放器，在包清单声明 `"kind":"native","abi":2,"renderer":"d3d12"`，保留 `entry` 和 `settings`。仓库中的 `examples/native-wallpapers/tidal-signal` 提供可直接构建的作品、HLSL 和独立像素测试，`pack` 是可导入成品。

1. 包含 `Action75Wallpaper12.h`，导出 `Action75GetWallpaper12(2)` 并返回 `A75Wallpaper12` 回调表。Windows x64 构建时可定义 `ACTION75_WALLPAPER12_EXPORTS`。两个 ABI 共用 `A75Event` 和作者的控件 ID。
2. 在 `create` 中使用 worker 交付的 D3D12 设备建立根签名、管线状态和持久资源。示例在作者机编译 HLSL，运行时从 `pack_dir/shaders` 读取 VS 和 PS 字节码。
3. 在 `render` 中将绘制命令追加到借用的 graphics command list。worker 已绑定 RTV、视口和裁剪区；作品绑定需要的根签名、PSO 和描述符。目标格式为单采样 RGBA8 UNORM，返回前须处于 RENDER_TARGET。命令列表的关闭、提交和同步归 worker；作者持有的 GPU 资源须至少存活到下一次回调。
4. 调色或调亮度时，从同一键位回读精确 RGB，同时核对另一处背景。用 `verify_player.py` 检查 ABI 1/2 切换、断开并重新连接后的首键，以及实际 worker 的独立内存和显存读数。详细命令在作品 README。
