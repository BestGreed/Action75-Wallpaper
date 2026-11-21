# 第一款壁纸：按键光环

目标是按一次 G，在对应键位看到一圈青色光环向外扩散，约 0.8 秒后只剩底板。控制台可分别调整光环和底板。这个练习只需已安装的软件及作者侧 Python，无需主程序源码。

## 从空目录生成成品

在 Windows PowerShell 7 中，将当前目录切到本 skill 根目录。先设置安装目录和输出目录。下面的生成器使用 Pillow 绘制 PNG，仅作者机器需要安装。输出包必须是新目录，避免覆盖已有作品。

```powershell
$software = 'D:\Apps\Action75-Wallpaper'  # 替换为含 dist、assets、existing-resources 的目录
$player = Join-Path $software 'dist/Action75Native/Action75Native.exe'
$work = Join-Path $env:TEMP ('action75-ring-' + [guid]::NewGuid().ToString('N'))
$pack = Join-Path $work 'mint-ring'
python -m pip install Pillow
python scripts/create_ring.py $pack --id mint-ring --name '薄荷光环'
python scripts/preview.py --player $player --pack $pack --frame "$work/trial.png"
```

试播成功会输出包 ID、`triggered: true` 和渲染帧数。原生作品的触发字段记录输入已送达。打开生成的 PNG 检查键位附近的画面。

| 文件 | 改它会发生什么 |
|---|---|
| `manifest.json` | 选择 `spriteBurst` 后端，连接背景、图集、封面和参数。此例是 4 列 × 2 行，8 帧从左到右逐行播放。 |
| `ring.png` | 决定光环各时刻的形状。换成自己绘制的 8 个等大透明帧，即可改变整个按键动画。改变格数时同步修改清单。 |
| `base.png` | 无按键时看到的原作底图。 |
| `cover.png` | 壁纸库缩略图。 |
| `settings.json` | “光环”组的 `glow` 绑定亮度，`reach` 绑定范围。“底板”组提供背景选择与调色。这份文件可直接作为分组样板。 |

先试一个有意义的修改。在生成器副本里将 `ellipse` 改成 `rectangle`，用自己的 ID 生成到新目录，再试播。应看到向外扩散的方框；面板的范围控制仍然有效。需要松手中止、持续跟随另一按键或与上一颗粒子碰撞时，改用能保存状态的原生路线。

## 验证参数确实驱动画面

沿用上面的 PowerShell 变量。相同键位触发两次独立会话，每次设置不同的光环范围。

```powershell
foreach ($scale in '0.5','1.4') {
    $patch = '{"reach":' + $scale + '}'
    python scripts/trace.py --player $player --host-root $software --pack $pack --output "$work/reach-$scale" --code KeyG --values $patch --frames-ms 120 300 1100
}
```

对比两份目录中的 `frame-00300.png`，较大范围的光环应更宽，中心仍在同一键位。到 `frame-01100.png` 应都恢复为底板。用 `trace.json` 核对实际采样时间、回执和坐标。对原生作品，回执统计送达，实际显示以关键位置的像素回读为准。想测亮度时，只变更 `glow`。查看实体分组、折叠、实际点击和保存恢复时，再在独立控制台导入包验收。

## 遇到问题

| 现象 | 从哪里查 |
|---|---|
| 导入找不到清单或某张图 | 选直接包含 `manifest.json` 的文件夹。把成品单独复制到空目录，按清单逐项核对文件名及相对路径，尤其检查美术文件是否还在作者工程外。 |
| 能显示底板，看不到触发动画 | 检查试播回执和较早的采样帧。图集检查透明通道、分格和时长；粒子检查发射器、导出的纹理及路径；原生插件检查事件类型、归一化键位与帧调度返回值。 |
| 滑块可拖，画面不变 | 对照控件 ID、默认值和两个极值。声明式后端检查 `binding.target`；原生插件检查创建和设置更新时是否读取同一个 ID，以及新值是否传给绘制代码。用相同按键与时刻重拍。 |
| 参数跑错分区或消失 | 查组上的 `entity`、控件上的 `appearance` 和 `visibleWhen` 引用的 ID 与值类型。播放规则所属对象写在播放组上。 |
| 原生包在开发机能跑，换目录后失败 | 核对 Windows x64、ABI、唯一入口 DLL、随包着色器和资源；播放器旁应有同版本 Worker。先用同环境运行已验证的示例，再检查作品特有依赖。 |
| 空闲时 GPU 仍忙 | 确认场景是否真的停止变化。原生作品动画结束后可返回 −1 等待事件；如果需要持续漂移，可降低更新频率。用硬件播放器在相同负载下复测，WARP 读回用于验证画面。 |
