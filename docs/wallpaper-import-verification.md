# Action75 Wallpaper 壁纸文件夹导入

## 使用方法

打开壁纸库的「导入壁纸」，选择直接包含 `manifest.json` 的作者文件夹。试播通过后，壁纸库会显示新包，点卡片即可播放。再次导入相同内容会定位到现有条目。同 ID 的导入包内容发生变化时，需要确认更新。正在播放的版本继续使用，重新选择卡片或重启后应用新版本，兼容的作者参数沿用原值。[图集示例](wallpaper-import-evidence/author-glow) · [导入界面](../apps/manager/src/wallpaper_import_ui.rs)

支持图集、使用内置材质的 Effekseer CPU 粒子，以及使用已有花型的烟火包。烟火作者包可以带参数定义和封面，选择、顺序和击键间隔由参数组声明。[烟火包制作说明](wallpaper-authoring/implementation.md)

`manifest.json` 中的直接引用与粒子效果实际引用的纹理、模型、曲线都必须随包提供。作者可以用 `preview` 声明封面，图集的底图也可以作为封面。应用会指出缺少的文件、无效图片、路径越界和暂未开放的后端或能力。[入库校验](../apps/manager/src/wallpaper_import.rs) · [粒子依赖检查](../native/windows-player/src/EffekseerBackend.cpp)

## 试播与发布

- 在壁纸库所在卷暂存一份资源快照，检查打开的文件是否位于包目录，拒绝重解析点和硬链接。每包最多 128 MiB、512 个文件与目录，图片最多 16,777,216 总像素。
- 用普通权限令牌、独立的 Windows 桌面和 Job Object 运行候选。进程在得到限额后才运行，只试播该包。候选用 D3D11 WARP 绘制并回读画面，超时 8 秒后终止并等待退出。[试播进程](../apps/manager/src/wallpaper_import/trial.rs) · [原生试播](../native/windows-player/src/main.cpp) · [Windows 受限令牌](https://learn.microsoft.com/en-us/windows/win32/secauthz/restricted-tokens) · [Windows Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- 每次提交发出完整资源版本。新包通过目录重命名入库，更新在资源准备好后原子替换清单。活动播放器持有已加载的资源与参数定义，用户参数由宿主按壁纸 ID 独立保存。试播失败将清理暂存内容。上次意外退出遗留的暂存目录在下一次导入时回收。[发布与清理](../apps/manager/src/wallpaper_import.rs) · [播放版本切换](../native/windows-player/src/main.cpp)

## 验收证据

自动化从隐藏桌面的壁纸库点击「导入壁纸」，操作 Windows 文件夹选择器，并用另一个隔离的原生播放器记录当前壁纸进程 ID、帧与按键响应。超时对照让隔离运行根内的候选进程刻意阻塞，走正式的 Job 终止和清理路径。[复验脚本](../artifacts/wallpaper-import-qa/verify.py) · [可机读结果](../artifacts/wallpaper-import-qa/acceptance.json)

图集、CPU 粒子分别完成 6 帧绘制和回读，试播进程的完整性级别实测为普通用户。超时对照从选择文件夹到完成记录历时 9.125 秒，原播放实例推进了 548 帧，并返回 85 次按键触发和状态采样。候选以退出码 124 结束，暂存目录和独立桌面均已释放。更新后重启恢复 `0.65×` 的光纹强度和 `0.75×` 的底图亮度，新加的展开速度采用作者默认的 `1.00×`。[可机读结果](../artifacts/wallpaper-import-qa/acceptance.json)

| 检查项 | 证据 |
| --- | --- |
| 新 ID、重复导入、续播 | [导入后壁纸库](../artifacts/wallpaper-import-qa/gallery-imported.png) |
| 同 ID 更新、取消、参数保留和重启 | [更新确认](../artifacts/wallpaper-import-qa/update-confirm.png) · [更新后壁纸库](../artifacts/wallpaper-import-qa/gallery-updated.png) |
| 缺图集、资源越界、缺粒子依赖 | [缺图集](../artifacts/wallpaper-import-qa/import-missing.png) · [路径越界](../artifacts/wallpaper-import-qa/import-boundary.png) · [粒子依赖](../artifacts/wallpaper-import-qa/effect-dependency.png) |
| 超时、播放器连续性、进程和桌面释放 | [超时反馈](../artifacts/wallpaper-import-qa/import-timeout.png) · [验收日志](../artifacts/wallpaper-import-qa/acceptance-run.log) |
| 窄窗口和既有标题栏 | [1080×680 石墨灰壁纸库](../artifacts/wallpaper-import-qa/gallery-compact.png) |

## 构建和复验

Release 构建通过 120 项控制台测试和 18 项原生播放器测试。[控制台日志](../artifacts/wallpaper-import-qa/manager-test.log) · [原生播放器日志](../artifacts/wallpaper-import-qa/native-test.log) · [验收构建产物](../artifacts/wallpaper-import-qa/dist)

Windows x64，使用项目要求的 Visual Studio 2022、CMake、Rust 工具链及带 Pillow 的 Python。以下命令从项目根目录运行，会把原生输出放在独立的验收目录。

```powershell
cmake -S native/windows-player -B artifacts/wallpaper-import-qa/build -G "Visual Studio 17 2022" -A x64 "-DACTION75_DIST_DIR=$PWD/artifacts/wallpaper-import-qa/dist"
cmake --build artifacts/wallpaper-import-qa/build --config Release
ctest --test-dir artifacts/wallpaper-import-qa/build -C Release --output-on-failure
cargo build --release --manifest-path apps/manager/Cargo.toml
cargo test --release --manifest-path apps/manager/Cargo.toml
```

复验脚本读取上述二进制、项目自带的 Effekseer SDK 归档和 `artifacts/wallpaper-import-qa/stalled-trial.exe`。后者可在 VS 开发者 PowerShell 中从同目录的 `stalled-trial.cpp` 编译。脚本的坐标对应 125% 缩放下的 1380×880 逻辑窗口。其他 DPI 可通过 `--click`、`--confirm` 指定按钮的客户区像素坐标。

```powershell
cl /nologo /MT artifacts/wallpaper-import-qa/stalled-trial.cpp /Fe:artifacts/wallpaper-import-qa/stalled-trial.exe /Fo:artifacts/wallpaper-import-qa/stalled-trial.obj /link /SUBSYSTEM:WINDOWS
python artifacts/wallpaper-import-qa/verify.py --suite --confirm 1034 640
```
