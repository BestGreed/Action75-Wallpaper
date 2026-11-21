# 夜站拾光

将 `pack` 导入支持公开原生 ABI 2 的 Windows x64 播放器。包 ID 沿用 `night-platform-runner`。左右方向键控制移动，松开即停。空格起跳，提前松开可缩短上升距离。Enter 暂停或继续。收集金色方块得一分，碰到红色障碍会回到起点。左上角两个数字分别显示得分和碰撞次数。面板可分别修改站台背景和游戏物件的风格、亮度、色相、饱和度与 RGB 颜色。背景包含天空、楼群及站台。游戏物件包含角色、金币、障碍与记分。高级选项保留原来的琥珀灯开关，旧设置继续影响灯带和角色。

进入系统暂停时会清除按键状态，恢复后从冻结的位置继续。角色静止、没有无敌效果且没有移动输入时，模块会请求宿主休眠，直至下一次事件。所有图形通过程序绘制，无外部素材。

## 独立构建

使用 Visual Studio 2022 的 C++ 桌面组件和 Windows SDK。在本目录运行以下命令。完整源码包提供 nlohmann/json 3.12.0。单独复制示例时可指定 `ACTION75_SDK_INCLUDE` 为公开头文件目录，`ACTION75_SDK_VENDOR` 为包含 `nlohmann/` 的目录。MSVC 构建默认静态链接 Visual C++ 运行库。

```powershell
$work = Join-Path $env:TEMP "Action75-KeyGame-$([guid]::NewGuid())"
cmake -S . -B "$work/build" -G "Visual Studio 17 2022" -A x64
cmake --build "$work/build" --config Release
ctest --test-dir "$work/build" -C Release --output-on-failure
cmake --install "$work/build" --config Release --prefix "$work/pack"
```

`game-simulation` 验证输入、碰撞、分数和暂停。`game-abi2-warp` 加载 DLL，在 D3D12 WARP 的 RGBA8 目标上回读像素，覆盖琥珀灯 RGB(255, 187, 89)、两层各五项外观参数、移动、得分、碰撞、暂停及空闲调度。

绘制器将同色矩形分批，直接写入宿主目标。每条 D3D12 清除指令最多提交 32 个矩形，以兼容 RTX 4060 上已复现的驱动限制。外观变化只在接收设置时重算两组共 14 个颜色。
