# DX12 按键验证

Windows x64 原生壁纸包，ID 为 `dx12-key-probe`。按键在校准位置点亮 88×88 像素的方块，显示 450 ms。靠近屏幕边缘时方块按画布裁切。到时恢复深蓝背景，等待下一次输入。

ABI 2 版本直接记录两次 D3D12 清屏命令，使用宿主借出的命令列表和 RTV。播放器负责设备、队列和提交。

## 构建和导入

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix "$PWD/out/dx12-key-probe"
```

独立复制源码后，配置时传入 `-DACTION75_SDK=...` 指向公开 SDK 的头文件目录，并传入 `-DACTION75_SDK_VENDOR=...` 指向包含 `nlohmann/json.hpp` 的目录。用支持 ABI 2 的播放器导入 `out/dx12-key-probe`。

使用黑洞示例的 `verify_native_parity` 工具可对照保存的桥接旧包。旧包需要支持 DX11/DX12 共享资源的硬件适配器。

```powershell
verify_native_parity.exe probe high <ABI1旧包目录> out/dx12-key-probe out/parity
```
