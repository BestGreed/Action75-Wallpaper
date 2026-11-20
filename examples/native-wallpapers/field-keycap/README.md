# 连续场与键帽壁纸

此目录是四个独立 D3D11 壁纸 DLL 的可编辑源码。发行目录位于 wallpapers-native 下，分别是 silk-nebula、gilded-synapse、harmonic-textile 和 keycap-glow。前三款沿用原版场模拟、着色和 HDR 辉光，最后一款按物理键码绘制键帽。三个场主题在各自 DLL 编译时选定，修改花名、文件夹名或清单 ID 后仍可独立导入。

设置由各包 settings.json 声明。场主题保留原版 13 个控件、预设与自定义配色，键帽主题保留 21 个控件。键盘按下事件携带宿主校准后的归一化位置和物理键码。场效果在事件坐标发射，鎏金脉络从包内 84 个键中心选择邻接目标。键帽使用物理键码匹配 layout.json 中的中心和尺寸，再应用作者公开的位置、缩放参数。

场背景持续动画，暂停后休眠。键帽在环境流动启用或键光仍在衰减时绘制。关闭流动后，最后一次键光衰减完毕便休眠，下一次输入唤醒绘制。

从本目录独立构建：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 4
```

四个 DLL 分别出现在 build/native-wallpapers/对应包名/wallpaper.dll。每个 DLL 已静态包含所用的渲染器和着色器，可替换待发布包里的同名文件。此目录的 Wallpaper.cpp 负责输入与作者参数映射。[场渲染器](../../../native/windows-player/src/FieldEffect.cpp)和[键帽渲染器](../../../native/windows-player/src/KeycapEffect.cpp)与传统宿主和像素测试共用。
