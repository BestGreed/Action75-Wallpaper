# 连续场与键帽壁纸

这里构建四个使用 Action75 公开 ABI2 的 D3D12 壁纸包。三个场主题在各自 DLL 编译时选定，包名和 ID 用于识别和保存设置。

| 作品 | 包 ID | 按键效果 |
| --- | --- | --- |
| 潮汐绢墨 | `silk-nebula` | 按当前键位扰动绢墨流场 |
| 鎏金脉络 | `gilded-synapse` | 在触点与邻近键中心之间产生脉络 |
| 声纹织物 | `harmonic-textile` | 编织条纹受到按键波场扰动 |
| 键影 | `keycap-glow` | 用物理键码定位键帽并在轮廓内点亮 |

原有 `settings.json`、`layout.json` 和封面随包交付。三个场保留质量档位决定的模拟和着色分辨率、32×12 触点分块以及四分之一分辨率的辉光。辉光设为零时回收对应的两张纹理并跳过模糊绘制。键影以 84 个四边形实例绘制，保留原有圆角遮罩、流动设置和单键衰减。

## 独立构建和导入

需要 Windows x64、Visual Studio 2022 的 C++ 桌面组件、含 `fxc` 的 Windows SDK，以及 CMake 3.24+。从本目录运行以下命令。

```powershell
$work = Join-Path $env:TEMP ('action75-field-' + [guid]::NewGuid().ToString('N'))
cmake -S . -B "$work/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$work/build" --config Release --parallel 2
cmake --install "$work/build" --config Release --prefix "$work/packs"
```

在支持 ABI2 的控制台选择「导入壁纸」，选中 `$work/packs` 下含 `manifest.json` 的任意作品目录。每个 DLL 静态包含该作品需要的渲染代码和编译后的 HLSL。

在仓库之外构建时，把本目录复制到独立位置，配置时分别设置 `ACTION75_SDK_INCLUDE`（公开 SDK 的两个头文件）、`ACTION75_SDK_VENDOR`（包含 `nlohmann/json.hpp` 的上级目录）和 `ACTION75_FIELD_PACKS`（含四个作品资源目录的根）。渲染器和 HLSL 均在本目录中。

## 像素对照

配置时加入 `-DACTION75_FIELD_COMPARE=ON`，再构建 `action75_field_compare`。对照程序接受旧包、新包和证据输出目录，默认使用 WARP。传入 `--hardware` 可在当前选到的低功耗适配器上对比 1920×720 与 1600×600。程序给两种 ABI 发出同一组时钟、按键和设置，输出每帧的 RGBA8 差值与成对截图。

```powershell
& "$work/build/Release/action75_field_compare.exe" $oldPack $newPack "$work/comparison"
```

`tests/verify_runtime.py` 在临时库运行配套播放器和控制台，在未激活的桌面上调用标准导入流程。它另外核对旧参数读取、短时同负载帧率、播放中缩放和空闲休眠。
