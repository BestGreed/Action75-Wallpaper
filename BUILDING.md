# Action75 Wallpaper

面向 Action75 键盘屏的 Windows 互动壁纸软件。Rust/egui 管理界面与 C++17/D3D11 播放器，本地渲染，支持按键交互。

## 使用

运行下载的 Windows x64 安装包，选择安装位置。首次使用先按 Win + P 选择「扩展」，再选择键盘屏和键盘。拔插或换接口后，可在首次设置或「设备与输出」刷新设备。1920 × 720 的副屏会标为键盘屏候选；选择其他分辨率时可以确认继续。
壁纸库单击即可切换。按住卡片拖到目标卡片左侧或右侧，看到插入线后松手即可排序。顺序在重启后保留，拖动期间壁纸继续播放。右侧前景和背景参数独立调整，高级参数默认折叠。关闭控制面板后壁纸继续运行；托盘右键可暂停、设置开机运行或完全退出。需要立即停止壁纸并释放鼠标时，按 Ctrl + Alt + Shift + Q 即可退出本次运行的控制台和播放器。

校准页提供拟合键位：确认后预览，可撤销，保存后生效。默认校准使用当前实机的 83 键参数。
设置可修改壁纸和日志目录，自动复制、校验并迁移；冲突不覆盖。迁移只处理壁纸包与本软件日志，其它文件保留。
日志默认在安装目录的 `logs`，每份最多 10 MB，共保留 10 份，不记录真实逐键输入。

## 软件更新

使用安装版打开控制台时会在后台检查正式发行版，持续打开时每 24 小时复查，也可在设置中手动检查。发现新版后点击“立即更新”，下载和校验期间可继续播放。安装交接时短暂停播，完成后重新打开控制台并恢复原壁纸。便携目录与其他安装实例独立。

更新源为 `blackwhite-poi/Action75-Wallpaper` 的 GitHub Releases。发布时让标签 `vX.Y.Z`、控制台 Cargo 版本、安装器版本和附件名 `Action75-Wallpaper-Setup-X.Y.Z-x64.exe` 一致。先在草稿中传完整安装包，再发布为 latest 正式 Release。客户端通过 HTTPS 获取 GitHub 提供的 SHA-256 和长度，校验通过才执行安装。草稿和预发行版不参与检查。

## 构建

需要 Windows、Visual Studio 2022 C++ 工具与 Windows SDK、Rust MSVC 工具链、CMake 3.24+。测试与打包脚本使用 Python 3.11+。

```powershell
python tools/restore-wallpaper-assets.py
./tools/build-manager.ps1 -Test
./tools/build-player.ps1 -Test
./tools/package.ps1 -Compiler 'C:/path/to/InnoSetup/ISCC.exe'
```

本地打包会把正式安装器写入 `release/`。

首次构建需下载 Cargo 锁定依赖、nlohmann JSON 3.12.0 和 Effekseer 1.80.8；原生依赖下载有固定 SHA-256 校验。打包使用 Inno Setup 的命令行编译器 ISCC。可通过上面的 `-Compiler` 指定安装路径。
内置壁纸均为 Windows x64 原生壁纸包。将 `wallpapers-native/<壁纸 ID>` 整个文件夹交给其他用户，即可通过控制台导入；设置、配色和图像资源随包交付。旧版用户保留同一壁纸 ID 和参数配置。

改动迁移包的 C++ 或着色器后，在已配置的原生构建目录运行 `cmake --build native/windows-player/build --config Release --target action75_migrated_packs`，更新 13 个发行文件夹的 DLL。黑洞沿用[独立工程](examples/native-wallpapers/black-hole/README.md)，构建后将其 `pack/wallpaper.dll` 同步到 `wallpapers-native/black-hole`。历史格式的作者包继续使用原有加载路径。

水彩材质的可编辑 HLSL 位于 `native/windows-player/materials/paintball`，修改后运行 `python tools/compile-paintball-materials.py` 更新壁纸包。
`tools/release-assets.json` 定义安装包和源码归档共同分发的文件。新增内置壁纸或资源时更新这份清单。

`python tools/verify-release.py --compiler '<ISCC.exe 路径>'` 构建带独立身份的测试安装器，检查安装、重复安装、卸载和正式安装登记。完整验收还会在隔离桌面操作控制台的「导入壁纸」，验证参数、目录重载和逐款壁纸的硬件离屏负载。仅检查安装生命周期时可传入 `--install-only`。

断屏重连回归包含在 `./tools/build-player.ps1 -Test` 中。升级验收使用独立安装目录与本机测试更新源。安装 Python 依赖 `psutil` 和 `pywin32` 后，先运行下列命令生成两个测试安装包，再验证播放中升级和暂停时升级。将旧版播放器与同目录下的 Worker 一起保留供对照。

```powershell
python tools/verify-installed-update.py build --work artifacts/update-qa --baseline-player '<旧版 Action75Native.exe 路径>'
python tools/verify-installed-update.py run --work artifacts/update-qa --playing
python tools/verify-installed-update.py run --work artifacts/update-qa
```

## 开发资料

- [当前项目架构图](docs/项目架构.svg)
- [壁纸制作 Agent 指南](docs/wallpaper-authoring/README.md)
- [产品约定](docs/product-decisions.md)
- [架构与证据](docs/architecture-baseline.md)
- [架构决策](docs/adr/README.md)
- [0.3.1 历史验收记录](docs/release-verification.md)

自有代码按 [Apache-2.0](LICENSE) 分发。字体、第三方组件与壁纸素材的授权见[许可范围](LICENSING.md)。
