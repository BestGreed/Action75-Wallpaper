# 壁纸参数模板

用 JSON 组装现有的参数组，再展开为独立的 `settings.json`。成品包交付 `manifest.json`、展开的 `settings.json` 和素材。Python 只用于作者制作阶段。

## 生成一份参数文件

在仓库根目录执行。示例为可直接导入的 `spriteBurst` 作者包生成参数。它沿用通用图集面板，修改亮度单位、大小范围、动画时长映射和背景色。

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

`library.json` 提供图集和 Effekseer 作者可组合的参数。上例的 `foreground-color` 管理特效配色和饱和度，`image-background` 管理背景亮度、饱和度、色相和颜色。图集或粒子作品通过 `binding.target` 指定播放器应调的属性。[通用作者模板](library.json)

`native-library.json` 收录当前内置原生包共享的参数。条件背景用 `visibleWhen.id` 和 `equals` 对应模式选项。力度上下限可组合 `impact-min` 和 `impact-max`，再用顶层 `orderedRange` 声明联动顺序。宿主把校验后的控件值交给 DLL，背景渲染和力度取样由 DLL 实现。[原生模板](native-library.json) · [上下限与背景实现](../implementation.md#impact-and-background)

## 14 款内置壁纸的配方

`builtins.json#/<壁纸ID>` 中每项都能展开为当前对应包的设置。14 款内置壁纸均以 `kind: native`、`abi: 1` 发布。原生作者可复用面板和控件 ID，并在自己的 DLL 中读取这些值。

| 壁纸 ID | 可复用参数 | DLL 内的资源或算法 |
| --- | --- | --- |
| `afterimage` 余像 | 原生 `atlas` 基础面板 | 按键动画图集与底图，默认加色 |
| `carrier` 载波 | `atlas`，覆盖亮度、大小、时长和配色 | 载波图集与加色混合 |
| `folio` 朱砂拓印 | `atlas`，覆盖强度、大小、时长和配色 | 纸面图集与正片叠底 |
| `tidal-glass` 潮汐玻璃 | `atlas`，覆盖默认值、时长范围和配色 | 图集呈现的玻璃光斑 |
| `meridian` 日蚀经线 | 图集的四个基础组，加轨道组 | 程序化轨道 |
| `gilded-synapse` 鎏金脉络 | 原生 `field` 基础面板 | 场主题 1 和布局资源 |
| `harmonic-textile` 声纹织物 | `field`，覆盖配色、曝光和余辉 | 场主题 3 和布局资源 |
| `silk-nebula` 潮汐绢墨 | `field`，覆盖配色、曝光和余辉 | 场主题 0 和布局资源 |
| `paintball-native` 水彩喷溅 | 前景亮度、大小、配色和图片背景 | Effekseer 效果与画布材质 |
| `black-hole` 黑洞 | 背景选择器及三套条件亮度、速度与色调 | 吸积盘、透镜和按键恒星 |
| `glass-shatter` 碎光玻璃 | `orderedRange`、背景选择和亮度 | 裂纹、折射和碎片 |
| `stillwater` 水波 | `orderedRange`、背景选择、亮度和底色 | 波场传播、消散与水面光纹 |
| `keycap-glow` 键影 | 共用的性能与待机组 | 键帽轮廓、光效节奏与布局参数 |
| `night-fireworks` 烟火 | 引用完整配置，包括 `variant-playback` | 花型、拖尾、夜景和触发节奏 |

烟火整份引用会原样展开 `playback`、花型顺序和控制项的关联。可用 `$overrides` 调整 `keyInterval` 默认值，并用对应 DLL 试播确认响应。[14 款配方](builtins.json) · [烟火参数](../../../wallpapers-native/night-fireworks/settings.json)

## 校验与对照

展开工具检查引用、循环、覆盖目标和输出结构。用 `--kind` 传入清单的 `kind`，让校验器按相同的宿主规则解析。例如 `native` 允许省略 `binding`，检查作者声明的控件和值。图集、Effekseer 和烟火的渲染绑定及播放声明会交给播放器现有解析器校验。校验全程使用临时状态目录。

```powershell
$templates = 'docs/wallpaper-authoring/templates'
$build = Join-Path (Get-Location) 'artifacts/wallpaper-schema-check'
cmake -S native/windows-player -B $build -G 'Visual Studio 17 2022' -A x64 "-DACTION75_DIST_DIR=$build/native-output"
cmake --build $build --config Release --target action75_pack_settings_tests
$checker = Join-Path $build 'Release/action75_pack_settings_tests.exe'

python "$templates/expand.py" "$templates/builtins.json#/carrier" --compare 'wallpapers-native/carrier/settings.json' --native-check $checker --kind native
python "$templates/expand.py" "$templates/builtins.json#/night-fireworks" --compare 'wallpapers-native/night-fireworks/settings.json' --native-check $checker --kind native
python "$templates/expand.py" "$templates/amber-atlas.json" --native-check $checker --kind spriteBurst
```

`--compare` 核对 JSON 值和数组顺序。加 `-o <路径>` 可保存文件。已有普通的 `settings.json` 时，也可直接执行下面的校验命令。需要 `--kind` 时，请先按上面的命令重建校验器。

```powershell
& $checker --check-settings --kind native 'examples/native-wallpapers/tidal-signal/pack/settings.json'
```

跑全量回归时，将校验器路径传给测试脚本。

```powershell
$env:ACTION75_SCHEMA_CHECKER = $checker
python -B "$templates/test_expand.py"
```

参数通过后，按现有流程核对资源、材质 uniform 和实际画面。[模板回归](test_expand.py) · [壁纸验收](../verification.md)
