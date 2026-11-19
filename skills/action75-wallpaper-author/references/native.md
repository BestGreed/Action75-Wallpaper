# Windows 原生交互壁纸

使用原生 SDK 版本的 Windows x64 播放器。宿主从包中读取清单和参数，在独立工作进程中运行作者提供的 DLL。开发时使用 SDK 的 `Action75Wallpaper.h`、Windows SDK 和 C++ 编译器。用户导入成品文件夹即可。

```json
{"version":1,"id":"my-stage","name":"舞台","kind":"native","abi":1,"entry":"wallpaper.dll","settings":"settings.json"}
```

`entry` 可以写包内相对路径，例如 `bin/sparks.dll`。导入时只允许这一份 DLL，发布和更新后保留其子目录结构。给参数声明稳定的控件 ID。四个回调通过一个导出函数 `Action75GetWallpaper` 返回；返回表要在 DLL 生命周期内有效。可用模块定义文件导出此名称，或在包含头文件前定义 `ACTION75_WALLPAPER_EXPORTS`。下载仓库后，将 [公共头文件](../sdk/Action75Wallpaper.h) 所在目录加入作者工程的包含路径。

- `create` 接收尺寸、D3D11 设备、上下文、只读资源目录和按控件 ID 索引的 JSON 值。返回作品实例。材质、几何、纹理在初始化时准备。建议把作者着色器预编译后随作品发布。
- `on_event` 接收按下、松开、指针、参数变化和挂起恢复。键码为物理键名，指针和键位坐标按宽高归一化。按住状态和游戏分数保存在作品实例中。收到输入重置时清除尚未释放的键。用户拖动改变作者参数时，可在输入回调内用 `commit_settings` 提交已声明控件的 JSON 对象。
- `render` 绘制到当次回调提供的 RTV。作品负责绑定目标、视口和所需流水线状态。按帧使用单调时钟。返回 0 请求下一帧，正整数表示等待毫秒数，−1 表示静止至下次事件。尺寸或设备变更时宿主可以重建实例。
- `destroy` 释放资源。可选的 `hit_test` 声明作品接收鼠标的区域，返回零的区域可透传。键盘副屏保护等设备策略由宿主处理。

将 DLL、相对路径资源和两个 JSON 放在同一个作品目录，用同版本播放器离屏试播；播放器旁边需有配套的 `Action75WallpaperWorker.exe`。验证按下与松开、暂停恢复、静止时帧计数、设置变动和重新载入。记录核显资源消耗时同时测播放器和绘制作品的工作进程。

需要关联最小值和最大值时，可以在参数 JSON 顶层声明 `"orderedRange": ["impactMin", "impactMax"]`。这两个 ID 必须指向取值范围相同的滑块。单独拖动一端越过另一端时，两端会一起移动。适用于碎玻璃和水波一类的随机力度区间。

## 打包自己的原生作品

在 Windows x64 上创建 DLL 工程，启用 Visual Studio 的 C++ 桌面开发组件与 Windows SDK。实现上面的四个回调，使用 C 链接导出 `Action75GetWallpaper`。宿主请求支持的 ABI 时返回回调表，版本不兼容时返回空指针。将 x64 Release 产物放到清单的 `entry` 路径，再用[离屏试播工具](../scripts/preview.py)检查载入、输入和画面。用户收到的是包含清单、DLL、参数和素材的成品文件夹。
