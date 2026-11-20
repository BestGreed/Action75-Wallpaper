# 夜站拾光

将 `pack` 作为壁纸文件夹导入支持原生壁纸 ABI v1 的 Windows x64 播放器。左右方向键控制移动，松开即停。空格起跳，提前松开可缩短上升距离。Enter 暂停或继续。收集金色方块得一分，碰到红色障碍会回到起点。左上角两个数字分别显示得分和碰撞次数。面板可分别修改站台背景和游戏物件的风格、亮度、色相、饱和度与 RGB 颜色。背景包含天空、楼群及站台；游戏物件包含角色、金币、障碍与记分。高级选项保留原来的琥珀灯开关，旧设置继续影响灯带和角色。

进入系统暂停时会清除按键状态，恢复后从冻结的位置继续。角色静止、没有无敌效果且没有移动输入时，模块会请求宿主休眠，直至下一次事件。所有图形通过程序绘制，无外部素材。

## 独立构建

使用 Visual Studio 2022 的 C++ 桌面组件和 Windows SDK。在本目录运行以下命令。若单独复制示例，可通过 CMake 变量 `ACTION75_SDK_INCLUDE` 指向公开头文件 `Action75Wallpaper.h` 所在目录。JSON 头文件来自同一 SDK 的 vendor-source/windows-native，可用 ACTION75_SDK_VENDOR 覆盖。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix ./pack
```

`game-simulation` 验证输入、碰撞、分数和暂停。 `game-abi-warp` 动态加载 DLL，以 D3D11 WARP 离屏渲染，分别从正式 RGBA8 目标及 BGRA8 纹理回读各通道，核对琥珀灯 RGB(255, 187, 89)，检查移动、得分、碰撞、暂停、主题切换和空闲调度。

支持 D3D11.1 ClearView 的设备直接在 GPU 目标上绘制矩形。其他设备按实际目标格式打包颜色并上传像素。

外观变化只在接收设置时重算两组共 14 个颜色。默认值保留原像素配色，角色运动、得分和暂停调度沿用原逻辑。game-abi-warp 在 RGBA8 和 BGRA8 上分别测试 10 项外观参数，并确认改动只出现在目标层，恢复默认可还原像素。
