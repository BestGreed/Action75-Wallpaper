# 独立作者壁纸 · 原生 D3D12

从控制台的「导入壁纸」选择 `packs` 下的一款作品文件夹。四个正式成品均为 Windows x64 原生 **ABI 2 / D3D12** 包，需要支持该接口的播放器和配套 Worker。各包包含自己的 DLL、设置声明与运行资源；效果和播放规则在包内执行，宿主仅提供公开事件、参数和 D3D12 命令列表。

| 成品 | 包内实现 | 保留的交互 | 独立源码 |
|---|---|---|---|
| [海图回声](packs/soundings-echo) | 原始 24 帧 PNG 图集与 D3D12 着色器 | 按键处展开声波环，原尺寸、插值、并发与背景设置 | [soundings-echo](sources/dx12/soundings-echo) |
| [夜航三景](packs/night-orbit-trio) | 原烟火模拟、HDR、辉光、烟雾与倒影的 D3D12 管线 | 默认每两键按土星、垂柳、棕榈轮放；原多选、随机与节奏 | [night-orbit-trio](sources/dx12/night-orbit-trio) |
| [灯历 · 八式焰花](packs/lantern-almanac) | 原八份 `.efk`、真实 Effekseer CPU 模拟与 D3D12 Renderer | 八种真实花型、拖尾、余烬；原多选、随机、节奏和外观参数 | [particles](sources/dx12/particles) |
| [潮标 · 偏岸](packs/tidemark-bearing) | 原 `.efk`、真实动态输入与 Effekseer D3D12 Renderer | `bearing` 改变橙色浮标位置，青色刻线固定 | [particles](sources/dx12/particles) |

包 ID、控件 ID、设置范围与默认值沿用原作品，旧参数值仍有相同含义。两个粒子包共享同一份包侧运行库，通过各自资源、参数声明与 `particles.json` 定义作品；没有按壁纸 ID 分派的宿主特例，也没有 D3D11On12。完整构建方法见 [D3D12 源码说明](sources/dx12/README.md)。用户只需导入对应的成品文件夹。

四款均提供独立的背景与前景不透明度，范围 0–100，默认 100。背景为 0 时空白区域输出透明像素，前景仍响应按键；前景为 0 时不遮住背景。烟花的光晕、烟与倒影随烟花前景调整，粒子资源中的各节点随同一次前景绘制调整。清单通过 `desktopOpacityControl` 引用背景滑块，需使用支持桌面透明输出的宿主；这里不采集或模拟桌面图像。旧配置缺少新值时按 100 处理。

`sources/atlas`、`sources/particle` 中保留原始图集生成器和 Effekseer 编辑工程，供修改美术与了解旧格式使用。它们生成的旧清单不是此处默认交付的原生成品；编辑资源后应按 D3D12 源码说明重新安装包。[作者技能](../../skills/action75-wallpaper-author/SKILL.md)

迁移前的完整包与 SHA-256 清单保存在 `artifacts/dx12-all/baseline/08`；实测报告、逐帧对照和隔离导入回执见 [验收报告](../../artifacts/dx12-all/08/README.md)。验收不操作正在使用的安装或实体副屏。

不透明度升级前的原生包、本次 0/50/100 像素验证与默认画面对照见 [透明叠加验收](../../artifacts/opacity-authors/08/README.md)。正式包已回写，运行安装由主控统一验收后切换。
