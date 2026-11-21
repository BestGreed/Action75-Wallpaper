# 作者接口

清单的公共字段包括 version（整数 1）、id（建议小写 ASCII 字母、数字、连字符或下划线，最多 64 字节）、name、kind、settings 和 preview。通常用文件夹名作为 id。例：

```json
{"version":1,"id":"mint-pulse","name":"薄荷脉冲","kind":"spriteBurst","settings":"settings.json","preview":"cover.png","background":"sky.png","effect":"pulse.png","columns":4,"rows":2,"duration":0.8,"diameter":200}
```

* 图集。透明 PNG 中按行放置等大的方格帧；每格至少 16 像素，图集最大 2048×2048；行列各 1–12，共 2–96 帧。播放时长 0.12–3 秒，直径 48–440 基准像素。发光适合 add，着色印迹可用 multiply。背景 PNG 单独提供。目标画幅 1920×720，小背景可铺展。
* CPU 粒子。清单使用 kind 为 effekseer，单个资源写 effect，多变体按顺序列在 effects 数组；可用正数 scale 设作品基准比例。用 Effekseer 1.80.8 的 CPU 发射器和内置材质创作。每份导出的 .efk 或 .efkefc 使用自己的局部坐标原点；以包内相对路径保留实际引用的纹理、曲线和模型。此 Effekseer 路线接受 CPU 发射器和内置材质。可从空白工程制作，分层设置粒子寿命、发射方向/位置、速度、大小、渐隐和色彩。中文路径下注意导出成功后检查文件和读回图。编辑器命令行可用 `Effekseer.exe -cui -in 源.efkproj -e 成品.efk`。使用 [1.8 工具手册](https://effekseer.github.io/Helps/18x/Tool/en/ToolReference/index.html)。
* 烟火。声明 kind 为 fireworks，使用 settings 和可选 preview。当前版形状 1–10 依次为垂柳、菊、牡丹、棕榈、万华镜、花束、土星、心形、金丝风车和星瓣大丽花。要自己绘制一款复杂烟花，使用上述通用粒子或自绘图集。
* 能力范围。原生 SDK 版还可以导入 [Windows 原生插件](native.md)；此前版本开放图集、兼容 CPU 粒子和烟火。播放器还带有黑洞、水面、玻璃、键帽、连续场和程序化爆发后端，供已有内容使用，复制这类工程需核对对应版本的目录加载能力；不能承诺正式导入成功。纯 image/video 是背景播放，没有按键粒子。每包最高 128 MiB、512 个文件与目录；控制台会在独立试播后发布。

## 参数

```json
{"version":1,"presentation":"layered","groups":[{"id":"look","label":"外观","controls":[{"id":"glow","label":"亮度","appearance":"brightness","type":"slider","default":1,"min":0.5,"max":2,"step":0.05,"unit":"×","binding":{"target":"effectBrightness"}}]}]}
```

五种基础控件是 slider、toggle、select、color、palette。区分作者自己的稳定 id 和运行时的 binding.target。通用效果可从 effectScale、effectBrightness 等当前后端支持的绑定起步；material.uniform 等需要效果资源本身实现对应输入。改变名字或创建一个 JSON 滑块并不会新增渲染能力。条件显示用 visibleWhen。升级沿用相同 ID 以保留用户设置。同 ID 的不同内容经用户确认更新，独立作品用独立 ID。预览只临时生效，正式提交才写用户配置。

## 参数分组

在 `settings.json` 顶层设置 `"presentation": "layered"`。先判断用户想调画面里的哪个对象，再决定控件放在基础还是高级。

- **对象**。在 `groups[]` 的组上声明 `"entity": "恒星"`。名称直接显示给用户，同名组会合并。自定义对象按首次出现顺序排列，主要对象可放在前面。简单的双层作品可以省略 `entity`，普通控件通过 `"surface": "background"` 归入背景，其余默认归入前景。
- **基础设置**。在控件上用 `appearance` 标明角色。面板依次显示样式或背景选择（`style`）、亮度（`brightness`）、不透明度（`opacity`）、色相（`hue`）、饱和度（`saturation`）、颜色或配色（`color`）。为可调背景配齐这些能力，包内渲染须实际响应。仅在特定背景下生效的颜色可用 `visibleWhen` 条件显示。
- **高级设置**。运动、物理、几何和交互控件省略 `appearance`，自动进入所属对象的高级设置，默认折叠。花型多选、播放顺序和击键间隔可在同一个播放组声明 `entity`。空分区自动隐藏。

例如，“恒星亮度”用 `entity` 为“恒星”的组和 `appearance` 为 `brightness` 的控件；“吸积盘亮度”用相同的外观角色，放在“黑洞与吸积盘”组。“引力透镜强度”也放在后者，省略外观角色。三者各自沿用稳定的参数 ID。

验收时打开控制台，核对控件只在所属对象下出现，并能独立展开高级设置。固定场景、按键与采样时刻，对比默认值和改动后的画面。例如调恒星色相时，对照按键触发的恒星与远景，确认调色范围符合设计。

## 粒子参数的像素验收

CPU 粒子的外观由作者工程决定。effectScale 会写入实例根节点；一级发射器需要继承根缩放，拖尾子节点按作品需要继承或在出生时取样。只看参数回执无法发现轨迹仍保持原尺寸的问题。用 trace.py 固定花型、按键和采样时刻，比较缩放两端的实际非背景包围盒。简单材质的亮度调节使用实例颜色乘数，实测可用的亮度区间为 0–1。速度改变时间推进，可从相同时间点的形状或淡出时刻验证。

制作新的复杂形状时，直接改变作者工程里的发射器、轨迹、子发射器和颜色，逐份导出资源。可在成品旁交付 .efkproj 或生成器供后续编辑。使用变体时，播放顺序字段属于作者，用户在控制台决定勾选哪些候选及是否随机。工具回执 effectVariantIndex 从 0 计，播放声明的候选编号从 1 计。

需要让作者自己的变量决定行为时，可把数值控件绑定到 effect.dynamicInput，并用 index 选择 0–3 号槽。在 Effekseer 的作者工程中，用对应输入驱动动态方程，例如改变某个发射器的初速度。每个槽的意义由作品定义。同一作品使用相同的槽分工，未绑定槽沿用资源默认值。支持这项能力的宿主需要匹配版本。验证时为同一个资源传两组参数，在相同按键和采样时刻对照像素，并确认结果符合方程预期。
