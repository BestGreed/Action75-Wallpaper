# 壁纸参数模板

用 JSON 组装现有的参数组，再展开为独立的 `settings.json`。成品包交付 `manifest.json`、展开的 `settings.json` 和素材。Python 只用于作者制作阶段。

## 生成一份参数文件

在仓库根目录执行。示例沿用图集面板，修改亮度单位、大小范围、动画时长映射和背景色。

```powershell
$templates = 'docs/wallpaper-authoring/templates'
python "$templates/expand.py" "$templates/amber-atlas.json" -o 'artifacts/amber-atlas/settings.json'
```

将生成的文件放进新壁纸目录，并在 `manifest.json` 写入 `"settings": "settings.json"`。图集行列、混合模式和素材路径仍写在对应后端的清单里。[可运行的琥珀余波配方](amber-atlas.json) · [图集包制作](../implementation.md#first-pack)

## 组合与覆盖

- `"$include": "library.json#/panels/atlas"` 引用一个面板。`#/groups/…` 或 `#/controls/…` 可引用组或控件。路径相对于引用所在的 JSON 文件，纯 `#/…` 指向同一文件。数组从下标 `0` 开始。
- 引用对象后可在同层覆盖字段。对象和数组字段整体替换。改 `binding` 时，写明该控件需要的完整目标和映射。新增组或控件时，在 `groups` 或 `controls` 数组中按面板顺序组合。
- 在组或面板里用 `"$overrides": {"控件ID": {...}}` 修改已存在控件的 `default`、`min`、`max`、`step`、`unit`、`binding` 或显示字段。覆盖保留控件 ID。为新控件取其他 ID 时，组合单个控件并在引用的同层写 `id`。

只需前景配色和背景色时，可以组合两个现成的组。

```json
{
  "version": 1,
  "presentation": "layered",
  "groups": [
    {
      "$include": "library.json#/groups/foreground-color",
      "$overrides": {"saturation": {"default": 65}}
    },
    {"$include": "library.json#/groups/image-background"}
  ]
}
```

`foreground-color` 管理特效配色和饱和度，`image-background` 管理背景亮度、饱和度、色相和颜色。条件背景可组合 `background-choice` 和 `conditional-brightness`、`conditional-speed`、`conditional-tint`。把各控件的 `visibleWhen.id` 和 `equals` 对齐到对应的选择器和档位，再使用该后端已有的绑定。场景按实际绑定切换背景，`visibleWhen` 控制面板显隐。[模板库](library.json) · [黑洞示例](builtins.json)

力度上下限可以组合 `impact-min`、`impact-max`，再覆盖范围和后端绑定。水波和碎光玻璃现有的上下限联动按壁纸 ID 处理，两者的场景各自取样。其他场景使用这对控件时，需在其原生逻辑中接入联动与取样。[上下限与背景实现](../implementation.md#impact-and-background)

## 14 款内置壁纸的配方

`builtins.json#/<壁纸ID>` 中每项都能直接展开。可从用途相近的配方起步，也可在自己的配方中引用完整的现有 `settings.json`。表中的后端和资源来自各包的清单。

| 壁纸 ID | 后端 | 可复用参数 | 该壁纸的资源或算法 |
| --- | --- | --- | --- |
| `afterimage` 余像 | `spriteBurst` | `atlas` 基础面板 | 按键动画图集与底图，默认加色 |
| `carrier` 载波 | `spriteBurst` | `atlas`，覆盖亮度、大小、时长和配色 | 载波图集与加色混合 |
| `folio` 朱砂拓印 | `spriteBurst` | `atlas`，覆盖强度、大小、时长和配色 | 纸面图集与正片叠底 |
| `tidal-glass` 潮汐玻璃 | `spriteBurst` | `atlas`，覆盖默认值、时长范围和配色 | 图集呈现的玻璃光斑 |
| `meridian` 日蚀经线 | `proceduralBurst` | 图集的四个基础组，加轨道组 | 程序化轨道，消费 `field.arc*` |
| `gilded-synapse` 鎏金脉络 | `field` | `field` 基础面板 | 场主题 1 和布局资源 |
| `harmonic-textile` 声纹织物 | `field` | `field`，覆盖配色、曝光、余辉等默认值 | 场主题 3 和布局资源 |
| `silk-nebula` 潮汐绢墨 | `field` | `field`，覆盖配色、曝光、余辉等默认值 | 场主题 0 和布局资源 |
| `paintball-native` 水彩喷溅 | `effekseer` | 前景亮度、大小、配色和图片背景 | 效果与画布材质，中心大小和颜料停留、淡出使用材质 uniform |
| `black-hole` 黑洞 | `blackHole` | 背景选择器及三套条件亮度、速度与色调 | 吸积盘、透镜和按键恒星 |
| `glass-shatter` 碎光玻璃 | `glassShatter` | 力度范围 0–3.2、背景选择和亮度 | 裂纹、折射和碎片 |
| `stillwater` 水波 | `waterSurface` | 力度范围 0–2.4、背景选择、亮度和底色 | 波场传播、消散与水面光纹 |
| `keycap-glow` 键影 | `keycap` | 共用的性能与待机组 | 键帽轮廓、光效节奏与布局参数 |
| `night-fireworks` 烟火 | `fireworks` | 引用完整配置，包括 `variant-playback` | 花型、拖尾、夜景和触发节奏 |

例如烟火整份引用会原样展开 `playback`、花型顺序和控制项的关联。也可以用 `$overrides` 调整已存在的 `keyInterval` 默认值。原生校验器负责检查花型、勾选项和间隔的关联及值域。[14 款配方](builtins.json) · [烟火参数](../../../wallpapers-native/night-fireworks/settings.json) · [原生解析](../../../native/windows-player/src/PackSettings.cpp)

## 校验与对照

展开工具会检查引用、循环、覆盖目标和输出结构。原生校验可复用现有测试程序，检查 schema、绑定、值域、花型协议并计算默认值，全程使用临时状态目录。

```powershell
$templates = 'docs/wallpaper-authoring/templates'
$build = Join-Path (Get-Location) 'artifacts/wallpaper-schema-check'
cmake -S native/windows-player -B $build -G 'Visual Studio 17 2022' -A x64 "-DACTION75_DIST_DIR=$build/native-output"
cmake --build $build --config Release --target action75_pack_settings_tests
$checker = Join-Path $build 'Release/action75_pack_settings_tests.exe'

python "$templates/expand.py" "$templates/builtins.json#/carrier" --compare 'wallpapers-native/carrier/settings.json' --native-check $checker
python "$templates/expand.py" "$templates/builtins.json#/harmonic-textile" --compare 'wallpapers-native/harmonic-textile/settings.json' --native-check $checker
python "$templates/expand.py" "$templates/builtins.json#/night-fireworks" --compare 'wallpapers-native/night-fireworks/settings.json' --native-check $checker
```

`--compare` 核对 JSON 值和数组顺序。加 `-o <路径>` 可保存文件。`--native-check` 也可用于普通的 `settings.json`。跑全量回归时，将校验器路径传给测试脚本。

```powershell
$env:ACTION75_SCHEMA_CHECKER = $checker
python -B "$templates/test_expand.py"
```

参数通过后，按现有流程核对资源、材质 uniform 和实际画面。[模板回归](test_expand.py) · [壁纸验收](../verification.md)
