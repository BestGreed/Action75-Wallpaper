# 四个作者示例的 D3D12 源码

这里的 C++ 工程只使用公开 `Action75Wallpaper12.h` / ABI 2。包侧创建自己的 D3D12 资源，将命令追加到 Worker 借出的命令列表；关闭、提交和帧同步由 Worker 负责。运行 DLL 不链接播放器私有模块，不含 D3D11、D3D11On12 或按作品 ID 选择实现的分支。

| 工程 | 安装结果 | 第三方构建依赖 |
|---|---|---|
| [soundings-echo](soundings-echo/README.md) | 单个 `soundings-echo` 包 | 随源码的公开 SDK 与 JSON 单头文件 |
| [night-orbit-trio](night-orbit-trio/README.md) | 单个 `night-orbit-trio` 包 | 随源码的公开 SDK 与 JSON 单头文件 |
| [particles](particles/README.md) | `lantern-almanac` 和 `tidemark-bearing` 两个包 | 公开 SDK、随源码 JSON、公开 EffekseerForCpp 1.80.8 源码 SDK |

均需要 Windows x64、Visual Studio 2022 C++ 工具、Windows SDK 和 CMake。每个工程的 README 给出独立构建命令；输出目录可放在仓库外。粒子工程静态链接 Effekseer CPU 模拟、LLGI 与 D3D12 Renderer，成品无需安装编辑器，也不加载独立 Effekseer DLL。其共享代码属于作者包，不要求宿主内置效果类型。

原始 `.png`、`.efk` 和 `settings.json` 按字节保留。美术源工程仍在相邻旧格式目录中；重新导出美术后，更新对应源码工程的资源副本并重新构建安装，保留原生清单与 DLL。

测试中的 D3D11 代码只用于冻结旧实现的画面对照，不链接运行 DLL。验收包含独立复制构建、实际 D3D12 帧回读、暂停恢复与空闲，以及通过生产导入器在临时库导入并重新冷加载。详见仓库 [验收记录](../../../../artifacts/dx12-all/08/README.md)。
