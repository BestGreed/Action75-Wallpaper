# 壁纸包与渲染实现

[返回制作入口](README.md) · [预览与验收](verification.md)

## 先选实现路线

| 需求 | 已有路线 | 可读样板 |
| --- | --- | --- |
| 换底图，加一段局部按键光效、波纹或纸面印染 | `spriteBurst`，以底图和 PNG 动画图集组成壁纸包 | [潮汐玻璃](../../wallpapers-native/tidal-glass/manifest.json) · [朱砂拓印](../../wallpapers-native/folio/manifest.json) |
| 需要连续计算的轨迹、位移或渐变 | `proceduralBurst`，用时间计算形态。先读当前 `proceduralStyle` 的分支 | [日蚀经线](../../wallpapers-native/meridian/manifest.json) · [ProceduralBurst.cpp](../../native/windows-player/src/ProceduralBurst.cpp) |
| 键帽内部发光、描边或局部纹路 | `keycap`，读取 `layout.json`，用物理键码定位键帽，限制影响范围 | [键影](../../wallpapers-native/keycap-glow/manifest.json) · [KeycapEffect.cpp](../../native/windows-player/src/KeycapEffect.cpp) |
| 键间传播、持续底纹、流场或全屏形变 | `field`，用 `fieldTheme` 选择已有场特效，配套 `layout.json` | [潮汐绢墨](../../wallpapers-native/silk-nebula/manifest.json) · [FieldEffect.cpp](../../native/windows-player/src/FieldEffect.cpp) |
| 编辑粒子发射器、曲线、贴图与材质 | `effekseer`，使用导出的效果及包内素材。可声明 `effect` 或 `effects`，可选 `canvasEffect` | [水彩喷溅](../../wallpapers-native/paintball-native/manifest.json) · [EffekseerBackend.h](../../native/windows-player/include/EffekseerBackend.h) |
| 全屏引力透镜、吸积盘、静默区或拖动主体 | `native`，独立 DLL 从包目录加载星云底图 | [黑洞](../../wallpapers-native/black-hole/manifest.native.json) · [BlackHoleScene.cpp](../../examples/native-wallpapers/black-hole/src/BlackHoleScene.cpp) · [BlackHole.hlsl](../../examples/native-wallpapers/black-hole/src/BlackHole.hlsl) |
| 共享水面、随机力度范围或可选光学背景 | `waterSurface`。「水波」的包 ID 仍为 `stillwater`。触发时随机取样，计算着色器推进波面，渲染着色器绘制水面与背景。清单省略 `background` | [水波清单](../../wallpapers-native/stillwater/manifest.json) · [水波参数](../../wallpapers-native/stillwater/settings.json) · [WaterSurface.cpp](../../native/windows-player/src/WaterSurface.cpp) |
| 花型组合、重力下坠、长尾和实时倒影 | `fireworks`。勾选播放花型后可固定播放或组合轮换，前景和背景分别调节。它也提供构建时编译 HLSL 的示例 | [烟火清单](../../wallpapers-native/night-fireworks/manifest.json) · [烟火参数](../../wallpapers-native/night-fireworks/settings.json) · [FireworksScene.cpp](../../native/windows-player/src/FireworksScene.cpp) |
| 碎裂、分枝与带折射的小片 | `glassShatter`。「碎光玻璃」用预生成的裂纹和碎片轮廓做实例化绘制。每次触发随机取样，背景由场景绘制 | [碎光玻璃清单](../../wallpapers-native/glass-shatter/manifest.json) · [碎光玻璃参数](../../wallpapers-native/glass-shatter/settings.json) · [GlassShatter.cpp](../../native/windows-player/src/GlassShatter.cpp) |

先复用所选路线的加载器、坐标和设置。可在现有后端内表达的差异交给壁纸包。需要新算法或新交互状态时，再改原生实现。每种花型要能固定选择或随机切换时，可以共享一个粒子后端和一套参数。`settings.json` 定义选项，由播放器实现选项对应的行为。

纯背景也可研究原生播放器已经识别的 `image` 和 `video` 清单。它们只有背景播放。要单独交付这种包时，同时核对管理器正常连接与断线时对该类型的支持。 [播放器目录扫描](../../native/windows-player/src/main.cpp) · [管理器目录回退](../../apps/manager/src/lib.rs)

<a id="first-pack"></a>

## 第一次制作练习 · 琥珀余波

目标是一张深蓝底，按下时从该键中心泛起一片琥珀色光，向外展开后消失。选 `spriteBurst` 跑通整条链路。下面的文件名与[隔离运行命令](verification.md#isolated-run)配套。

1. 在仓库的 `wallpapers-native/amber-ripple/` 准备新包。仅验证加载时，可将[余像](../../wallpapers-native/afterimage)的 `background.png` 和 `burst.png` 临时当作测试素材。制作自己的作品时，将它们换成有使用及分发权的素材。并行开发若涉及日常壁纸目录，先在独立源码副本里做练习。
2. 准备 1920×720 的 `background.png` 和 `burst.png`。下面采用 6 列×4 行，每格按从左到右、从上到下排列，共 24 帧。例如每格 128×128 像素时，图集为 768×512。各帧围绕同一中心出图，保留软边。当前后端以图集的 **RGB** 作光效或颜料遮罩，空白区的 RGB 置黑，单改 alpha 无法清除它。
3. 保存下面两份 UTF-8 JSON 到包目录，编码不带 BOM。加入 `preview.png` 时使用同一壁纸的实渲染画面，按 8∶3 构图。初次渲染前可临时省略 `preview` 键，控制台将尝试读 `background`。

`manifest.json`

```json
{
  "version": 1,
  "id": "amber-ripple",
  "name": "琥珀余波",
  "kind": "spriteBurst",
  "background": "background.png",
  "effect": "burst.png",
  "columns": 6,
  "rows": 4,
  "duration": 0.8,
  "diameter": 324,
  "composition": "add",
  "settings": "settings.json"
}
```

`settings.json`

```json
{
  "version": 1,
  "presentation": "layered",
  "groups": [
    {
      "id": "appearance",
      "label": "外观",
      "controls": [
        {
          "id": "brightness", "label": "余波亮度", "type": "slider",
          "default": 1, "min": 0, "max": 2, "step": 0.05, "unit": "×",
          "surface": "foreground", "binding": {"target": "effectBrightness"}
        },
        {
          "id": "spread", "label": "余波范围", "type": "slider",
          "default": 1, "min": 0.5, "max": 2, "step": 0.05, "unit": "×",
          "surface": "foreground", "advanced": true, "binding": {"target": "effectScale"}
        },
        {
          "id": "backgroundBrightness", "label": "背景亮度", "type": "slider",
          "default": 1, "min": 0.25, "max": 1.5, "step": 0.05, "unit": "×",
          "surface": "background", "binding": {"target": "background.brightness"}
        }
      ]
    }
  ]
}
```

4. [跑隔离验收](verification.md#isolated-run)。确认按键原点、范围和背景亮度各自有效。制作成品时，绘制深蓝底和新的琥珀色图集，截取实渲染画面作封面，再在 `manifest.json` 加 `"preview": "preview.png"`。保留测试素材的来源记录和制作用的源文件。

## 壁纸包约定

- **目录和 ID** · 默认壁纸目录 `wallpapers-native/<id>/`，也可以是用户设置里的壁纸目录。播放器另扫描运行根目录下的 `data/native-wallpapers`。目录名与 `id` 一致，使用稳定的 ASCII 小写字母、数字和连字符，控制在 64 字符内，让本地和在线壁纸库一致。新作品用新 ID，已有作品沿用原 ID 和控件 ID。播放器会排除 Windows 保留设备名及大小写冲突。
- **清单和资源** · `manifest.json` 声明 `version: 1`、`id`、`name`、`kind` 与该后端读的资源字段。素材路径相对包目录并留在其中。`spriteBurst` 图集为最大 2048×2048 的 PNG，等大的方格至少 16 像素，行列各 1–12 且总格数 2–96，`duration` 为 0.12–3 秒，`diameter` 为 48–440 基准像素。`add` 适合发光，`multiply` 适合纸面颜料。
- **缩略图** · 优先显式指向 `preview.png`。管理器会按可见行加载，将画面等比缩至 480×180 内，并生成小尺寸的选中框模糊纹理。准备能在小卡片上辨认核心效果的实际画面。要从 `listPacks` 核对封面，先取得返回的 `path`，再从该目录的 `manifest.json` 读取 `preview`。出现「有卡片但无法播放」时，核对 IPC 的 `supported`、`schemaAvailable` 和 `schemaError`，继续验证 `selectPack`。断线时控制台能从本地清单绘制卡片。 [管理器扫描](../../apps/manager/src/lib.rs) · [缩略图加载](../../apps/manager/src/gallery_previews.rs)
- **设置 schema** · `settings.json` 声明 `version: 1`、`groups[].controls[]`。组和控件都需要稳定 `id` 与 `label`。控件声明 `type`、`default`，普通渲染参数还声明 `binding.target`。最多 16 组、64 个控件、32 KiB。`presentation: "layered"` 以 `surface: "foreground"`/`"background"` 分前景和背景，`advanced: true` 收进高级区，`visibleWhen` 可按另一控件的值显示。
- **控件与绑定** · `slider` 写明 `min/max/step`，`toggle` 用布尔值，`select` 提供数字的 `options[].value` 与显示名 `label`。颜色由三个 0–255 的 RGB 整数组成，配色可用 `palette` 声明包内 `palettes`、`maxColors` 和默认 `preset/colors`。滑块精度、默认值、映射后的值域都要通过原生校验。`binding.mapping` 支持 `identity`、`linear`、`power`、`piecewise`。`material.uniform` 绑定实际 Effekseer 材质的统一变量和分量，涉及多变体时逐一核对。参数声明处与消费处同时阅读。 [PackSettings.cpp](../../native/windows-player/src/PackSettings.cpp) · [pack_ui.rs](../../apps/manager/src/pack_ui.rs)
- **默认值与用户值** · 包内 `settings.json` 是模板。每款的用户参数保存在运行状态目录的 `wallpaper-settings/<id>.json`，含 `revision`。调参经 `previewPackSettings` 预览，`setPackSettings` 提交后持久化，`resetPackSettings` 恢复。修改已有控件的 ID、类型或范围时验证旧值迁移，直接改模板的默认值通常只影响新 profile 或重置。 [PackSettings.cpp](../../native/windows-player/src/PackSettings.cpp) · [pack_model.rs](../../apps/manager/src/pack_model.rs)
- **烟火播放规则** · 一个组可声明 `presentation: "variant-playback"`。组内 `playback.selection` 指向花型选择，`randomize` 指向随机开关，`interval` 指向正整数滑块。`selection` 的 `value: 0` 表示组合，其他选项通过 `value` 选用原生花型 1–8，并以 `toggleId` 指向组内的勾选控件。`playback.order` 将本包声明的每个花型列一次，可以只选用部分花型。播放控件由组引用关联，可使用包内 ID 并省略 `binding`。内置烟火继续提供旧绑定供老播放器读取。副屏点按立即绽放，击键按设置次数计数。 [烟火参数](../../wallpapers-native/night-fireworks/settings.json)
- **自制粒子花型** · 从 Effekseer 导出兼容 1.80.8 的 CPU 效果文件，随包附上相对路径引用的素材。清单用 `"kind": "effekseer"` 和 `"effects": ["effects/heart.efkefc", "effects/palm.efkefc"]` 声明资源。沿用 `variant-playback` 组，`value: 1` 和 `value: 2` 对应清单中的第一和第二个文件，`playback.order` 决定轮换顺序。试播从屏幕中央立即触发，正常播放按击键间隔触发，点按副屏直接播放。控件可通过 `"binding": {"target": "effect.dynamicInput", "index": 0}` 写入 Effekseer 的动态输入 0–3，再由作者的动态方程决定怎样改变粒子。未绑定的槽继续使用每个效果文件的默认值。 [粒子加载](../../native/windows-player/src/EffekseerBackend.cpp) · [试玩与击键入口](../../native/windows-player/src/main.cpp)

<a id="impact-and-background"></a>

## 随机范围与可选背景

1. **先定义单次触发。** 确定力度会影响哪些可见量，例如碎裂半径、主裂纹长度、碎片初速度和散开幅度。触发时在有效区间取样一次，把该次力度留在对应实例里。测试可用固定种子重放。上下限相等时得到固定力度。
2. **约定两个滑块如何联动。** 单个 slider 的范围校验只覆盖自身。目前 `normalizeImpactRange` 只处理 `stillwater` 和 `glass-shatter` 的 `impactMin`、`impactMax`。拖过另一端时同步移动对端，批量提交倒序值时归整。给新包增加上下限，需要显式接入归整逻辑，并检查读取旧值、预览和提交得到一致的范围。场景从 `PackAppearance` 读取最终值。 [水波参数](../../wallpapers-native/stillwater/settings.json) · [玻璃参数](../../wallpapers-native/glass-shatter/settings.json) · [原生校验和参数应用](../../native/windows-player/src/PackSettings.cpp)
3. **背景选择也要接到画面。** 用 `select` 的数值选项定义可保存的背景模式，在 `PackAppearance`、场景 `configure` 和着色器中依次接通。用 `visibleWhen` 显示只对特定背景有效的控件。固定击键位置、力度和时间，只改变背景做对比，再切回并重启核对保存。 [水波参数](../../wallpapers-native/stillwater/settings.json) · [水面背景实现](../../native/windows-player/materials/WaterSurface.hlsl)

## 新增原生场景时检查这些入口

| 入口 | 要接通的行为 | 源码 |
| --- | --- | --- |
| 清单扫描和控制台列表 | 校验新 `kind`、资源及可选项。确认在线列表、离线卡片、选择失败保留原壁纸 | `packs`、`Player::listPacks`。[播放器入口](../../native/windows-player/src/main.cpp) · `local_packs`。[管理器 lib.rs](../../apps/manager/src/lib.rs) |
| 资源准备与提交 | 用已有 D3D11 设备准备候选资源，校验成功后替换活动对象。失败时恢复当前背景、释放候选。切换时复用已加载资源，评估编译与读盘带来的首帧等待 | `Player::selectPack`。[播放器入口](../../native/windows-player/src/main.cpp) · [NativeRenderer.h](../../native/windows-player/include/NativeRenderer.h) |
| 输入和画面生命周期 | 复用 `trigger`、`stopEffects`、`hasMotion`、`render`。检查按键唤醒、暂停清退、恢复、隐藏以及尺寸变化。需要鼠标交互时将客户区像素转换为归一化坐标，同时遵循独占和校准流程 | [播放器入口](../../native/windows-player/src/main.cpp) · [RawKeyboard.cpp](../../native/windows-player/src/RawKeyboard.cpp) |
| 参数 | 若有新控制，连通数值范围、`PackAppearance`、`applyAppearance` 和具体场景，验证下限、默认值与上限。已有控件由 schema 生成 | [PackSettings.h](../../native/windows-player/include/PackSettings.h) · [PackSettings.cpp](../../native/windows-player/src/PackSettings.cpp) |
| 构建 | 普通播放 `Action75Native`、硬件验证用 `Action75Perf`，以及探针或测试目标要链接同一实现。新 HLSL 可沿用现有嵌入规则，按现有目标组织源码 | [CMakeLists.txt](../../native/windows-player/CMakeLists.txt) · [build-player.ps1](../../tools/build-player.ps1) |

### 输入与坐标

`RawKeyboard` 按设备和物理扫描码处理，按住时过滤系统重复的 key-down。正常播放器在按下时调用 `trigger`。新效果需要持续按住或释放时，补齐对应状态和松开、断开、清理测试。

`PlayerConfig` 给出的 `x, y` 是以副屏左上角为原点的归一化坐标，像素点为 `(x × width, y × height)`。作圆形、传播或距离判断时，要按实际渲染宽高修正比例。校准以 `data/fm-calibration.json` 的已保存点位为准，新环境从仓库的默认校准开始。键帽几何来自场景的 `layout.json`，使用前应对照它的拟合依据及实机布局。校准保存更新中心点，依赖拟合键帽的包仍要核对其自身几何。先用中心点验证触发源，再检查键帽轮廓、边缘裁切、黑边和侧边长键。使用测试副本试验拟合，用户确认保存后才应用。 [PlayerConfig.cpp](../../native/windows-player/src/PlayerConfig.cpp) · [默认校准](../../assets/defaults/fm-calibration.json) · [校准拟合](../../apps/manager/src/calibration_fit.rs)

### 动画与 GPU 资源

让动画取自单调时间或固定模拟步长，控制每帧最多补算多少步，测重载后是否追帧。保持 60 FPS 的呈现节奏，内部物理步长可以更细。预分配有界的事件池、实例和 GPU 缓冲，在加载阶段创建资源，重复使用。在渲染线程提交 D3D11 命令，避免每帧读盘、编译 shader 或 GPU 回读。优先合批局部特效，测量后再增加全屏 pass 或改变中间纹理精度。

### 着色器怎样验证

先分清自己沿用的构建方式。`embed_hlsl` 只把 HLSL 源码嵌入程序，实际编译发生在加载场景时。若增加或修改了入口，在 VS 开发者终端用 Windows SDK 的 `fxc.exe` 按实际入口名分别编译，并使用对应的 `vs_5_0`、`ps_5_0` 或 `cs_5_0`。例如可用 `/T cs_5_0 /E StepWave` 检查水面模拟入口。接着运行播放器 `selectPack` 和渲染探针，验证设备资源、常量布局与实际画面。若切换首帧耗时明显，参考「烟火」的构建期编译方式，把着色器的编译成本放到构建阶段。 [CMake 着色器入口](../../native/windows-player/CMakeLists.txt) · [烟火着色器编译器](../../native/windows-player/cmake/CompileFireworksShaders.cpp)
