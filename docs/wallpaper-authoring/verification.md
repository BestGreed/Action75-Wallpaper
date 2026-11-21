# 预览、验收与交付

[返回制作入口](README.md) · [壁纸包与渲染实现](implementation.md)

## 构建与取用程序

在 `Action75-OpenSource` 根目录执行。需要 Windows x64、VS 2022 C++ 工具和 Windows SDK、CMake 3.24+、仓库 `Cargo.toml` 要求的 Rust MSVC 工具链。下面的隔离示例需要 Python 3.11+。首次原生构建由仓库脚本获取并校验依赖。

```powershell
.\tools\build-player.ps1 -Test
.\tools\build-manager.ps1 -Test
```

播放器输出到 `dist/Action75Native/Action75Native.exe`，管理器输出到 `apps/manager/target/release/Action75Manager.exe`。素材和配置变更可以先用支持该 `kind` 的已构建播放器。改动渲染或注册逻辑时，使用当前源码构建。工作区的 `dist` 若正被日常实例占用，到独立源码副本构建并使用其中产物。原生测试以脚本实际选择的目标为准，改到其他探针时，按 CMake 目标另行构建和执行。 [播放器构建脚本](../../tools/build-player.ps1) · [管理器构建脚本](../../tools/build-manager.ps1) · [CMake 测试入口](../../native/windows-player/CMakeLists.txt)

### 复制当前源码并隔离构建

先检查源码副本是否带上需要的未提交改动。仓库尚无首次提交时，直接从当前工作树复制。仅为 CMake 更换 `-B` 目录时，程序仍输出到源码根目录的 `dist`。复制源码根目录才能同时隔离构建和程序输出。在仓库根目录执行下面的 PowerShell。

```powershell
$wallpaperTrialSource = @'
import shutil
import tempfile
from pathlib import Path

repo = Path.cwd()
stage = Path(tempfile.mkdtemp(prefix="action75-authoring-"))
excluded = {"build", "target", "dist", "data", "artifacts", "vendor", "__pycache__"}

def omit(parent, names):
    return [name for name in names
            if name in excluded or name.endswith(".log")
            or (name.startswith("build-") and (Path(parent) / name).is_dir())]

for name in ("apps/manager", "native/windows-player", "tools", "assets",
             "existing-resources", "wallpapers-native", "docs", "Licenses"):
    shutil.copytree(repo / name, stage / name, ignore=omit)
for name in ("README.md", "PRODUCT.md", "DESIGN.md", "GLOSSARY.md",
             "vendor-source/windows-native/nlohmann/json.hpp",
             "vendor-source/windows-native/EffekseerForCpp1.80.8.zip"):
    source = repo / name
    if source.is_file():
        target = stage / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
calibration = stage / "data/fm-calibration.json"
calibration.parent.mkdir()
shutil.copy2(stage / "assets/defaults/fm-calibration.json", calibration)
print(stage)
'@ | python -
if ($LASTEXITCODE -ne 0) { throw "Failed to copy Action75 source" }
Set-Location -LiteralPath $wallpaperTrialSource
.\tools\build-player.ps1 -Test
```

管理器依赖 Cargo。`Get-Command cargo` 找不到时，脚本按 `CARGO_HOME/bin/cargo.exe` 取工具。未设置 `CARGO_HOME` 时使用用户目录下的 `.cargo`。自定义安装 Rust 时，在当前 PowerShell 会话指定现有工具链。下面是本机的配置，其它电脑按各自安装位置填写。

```powershell
$env:CARGO_HOME = 'D:/Coding/Rust/cargo'
$env:RUSTUP_HOME = 'D:/Coding/Rust/rustup'
& (Join-Path $env:CARGO_HOME 'bin/cargo.exe') -V
.\tools\build-manager.ps1 -Test
```

`native_mapping_input` 会直接读取源码副本下的 `data/fm-calibration.json`。上面的脚本只从默认校准生成这份测试夹具。遇到该测试报告校准缺失时，先检查夹具路径。性能目标可用 `cmake --build native/windows-player/build --config Release --target Action75Perf` 单独构建。 [键位映射测试](../../native/windows-player/tests/core_tests.cpp) · [CMake 构建输出配置](../../native/windows-player/CMakeLists.txt)

### 并行 Agent 怎样合入

- 开工时列出各自新增的场景、资源和测试，再明确共用文件变更到哪个函数或目标。常见交集有播放器 `main.cpp`、`PackSettings.*`、CMake、管理器 `local_packs` 和构建与发布脚本。
- 各自用隔离源码、构建输出和运行目录推进。需要写同一处时，由负责合入的 Agent 确认上一组补丁已落盘，再基于当前文件逐块合并。有补丁文件时可用 `git apply --check` 预检上下文。管理器、播放器的共用二进制与重启时机单独交接。
- 如果隔离副本里已有壁纸测试失败，先确认该份源码、测试、清单和配置来自同一个快照。其它会话更新过测试或壁纸包时，把相关文件一起同步再归因。复制会保留文件修改时间时，MSBuild 可能继续用副本里的旧对象。确认失败目标确实重新编译，必要时在隔离工作区使用新的构建目录。遇到键位测试则先核对默认校准夹具。
- 合并后用最终源码重新构建或核对隔离产物与合入文件一致，运行新场景和相邻回归，记录所用构建和仍需实机确认的项目。

<a id="isolated-run"></a>

## 跑通第一款壁纸

先准备[「琥珀余波」示例包](implementation.md#first-pack)。在仓库根目录运行下面这段 PowerShell，最后一行是包目录。脚本将包、默认校准和键位表复制到 `artifacts/wallpaper-qa-*`，为播放器分配独立的运行根目录、状态目录和命名管道。该实例用 D3D11 WARP 离屏运行，以协议的 `testTrigger` 触发当前壁纸。

```powershell
@'
import json
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

repo = Path.cwd()
source = Path(sys.argv[1]).resolve()
pack_id = json.loads((source / "manifest.json").read_text(encoding="utf-8"))["id"]
assert source.name == pack_id, "Directory should match the manifest ID"
player = repo / "dist/Action75Native/Action75Native.exe"
assert player.is_file(), "Build Action75Native first"

run = repo / "artifacts" / ("wallpaper-qa-" + uuid.uuid4().hex[:12])
app, state = run / "app", run / "state"
pipe = "Action75WallpaperQA-" + uuid.uuid4().hex
shutil.copytree(source, app / "wallpapers-native" / pack_id)
for source_path, app_path in [
    ("existing-resources/finalmouse/action75-ue-keymap.json",
     "existing-resources/finalmouse/action75-ue-keymap.json"),
    ("assets/defaults/fm-calibration.json", "data/fm-calibration.json"),
]:
    target = app / app_path
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / source_path, target)
state.mkdir()
options = [str(player), "--root", str(app), "--state-dir", str(state),
           "--pipe", pipe, "--headless-test"]

def call(action, *, expect=True, **fields):
    request = json.dumps({"action": action, **fields}, ensure_ascii=True)
    result = subprocess.run([*options, "--command", request], capture_output=True,
                            encoding="utf-8", timeout=12)
    reply = json.loads(result.stdout)
    assert reply.get("ok") is expect, reply
    return reply.get("result")

with (run / "player.log").open("wb") as log:
    process = subprocess.Popen(options, stdout=log, stderr=subprocess.STDOUT)
    try:
        # Wait for this instance and leave its log available on startup failure.
        deadline = time.monotonic() + 12
        while True:
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f"Player did not start. See {log.name}")
            try:
                status = call("status")
                break
            except (AssertionError, json.JSONDecodeError):
                time.sleep(0.1)

        assert status["headless"] and status["fps"] == 60, status
        entry = next(p for p in call("listPacks") if p["id"] == pack_id)
        assert entry["supported"] and entry["schemaAvailable"], entry
        active = call("selectPack", id=pack_id)
        assert active["packId"] == pack_id, active
        call("testFrame")
        frame = state / "test-frame.png"
        idle_image = frame.read_bytes()
        trigger = call("testTrigger", code="KeyA")
        assert trigger["acceptedTriggers"] == 1, trigger
        deadline = time.monotonic() + 1
        while True:
            call("testFrame")
            if frame.read_bytes() != idle_image:
                break
            if time.monotonic() >= deadline:
                raise AssertionError("The key did not change the frame")
            time.sleep(0.03)

        profile = state / "wallpaper-settings" / f"{pack_id}.json"
        before = profile.read_bytes()
        preview = call("previewPackSettings", id=pack_id, values={"brightness": 1.2})
        assert preview["values"]["brightness"] == 1.2 and profile.read_bytes() == before
        call("setPackSettings", id=pack_id, values={"brightness": 1.2})
        assert json.loads(profile.read_bytes())["values"]["brightness"] == 1.2
        call("selectPack", id="missing-pack-for-qa", expect=False)
        assert call("status")["packId"] == pack_id

        summary = {"pack": pack_id, "renderer": active["renderer"], "trigger": trigger,
                   "visibleKeyFeedback": True, "previewAndSave": True, "failedSelectionPreservedPack": True}
        (run / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(run / "result.json")
        call("stop")
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)
'@ | python - 'wallpapers-native/amber-ripple'
```

用于别的作品时，把目录、可触发键和参数值换成该包的实际值。背景自身运动时，在键位附近对照同一时刻的无输入帧。像黑洞静默区那样会抑制特效的场景，分别在阴影内外读回画面。`--state-dir` 隔离播放器状态和各壁纸参数，`--root` 还决定校准、壁纸目录配置、日志等路径。两者和管道都用隔离值。

## 查看真实画面

- 用适合当前后端的离屏探针画出指定时刻并导出 PNG。T0 留一张待机画面，另抓首次可见、峰值和淡出。需要短片时，按固定时间步进导出连续帧，编码成 60 FPS MP4。保留原始画幅，另看 480×180 缩略图。
- 想给示例图集出图，可参考 `SpriteBurstProbe.cpp`。先以 1920×720 初始化离屏 `NativeRenderer`、加载背景与图集，再按清单参数和配置设置 `SpriteBurst`。待机时在 `beginFrame` 后直接保存。激活时调用 `push` 注入归一化位置，在给定时刻依次调用 `beginFrame`、`draw` 和 `saveFrame`。这样能用固定输入对比图集的节奏、叠加和范围。 [图集探针](../../native/windows-player/tests/SpriteBurstProbe.cpp) · [原生渲染器](../../native/windows-player/include/NativeRenderer.h)
- 现有的黑洞与水面测试目标可以直接出图，两者都接收「包目录、输出目录」。`action75_black_hole_scene_tests.exe` 产出 `idle.png`、`interactive.png` 和 `half-horizon.png`，`action75_water_surface_tests.exe` 产出 `idle.png`、`touch.png` 和 `ripples.png`。默认路径为 `native/windows-player/build/Release/`。普通 `--headless-test` 用来测协议和状态。`--self-test` 走 Effekseer 的诊断样例。 [黑洞探针](../../native/windows-player/tests/black_hole_scene_tests.cpp) · [水面探针](../../native/windows-player/tests/water_surface_tests.cpp)
- 「碎光玻璃」使用 `action75_glass_shatter_tests.exe <包目录> <输出目录> --sequence`。输出待机、三种背景及 `frames` 里的连续帧。将 `--sequence` 换成 `--core-sizes` 可在同一点、同一力度下输出三档中心大小，用 `bare-cracks.png` 对照外围主裂纹。随机力度区间由场景探针直接读取样本验证。 [玻璃探针](../../native/windows-player/tests/glass_shatter_tests.cpp)
- 检查控制台时，用 `app/data/app-settings.json` 的 `setupComplete: true` 启用壁纸库。要取在线画面，保持隔离播放器运行，将它的管道名写入 `app/data/player-pipe.txt`。然后以 `Action75Manager.exe --root "<app绝对路径>" --screenshot "<输出PNG>" --exit-after-screenshot` 取图。管理器会打开自己的窗口，注意安排在实机查看阶段。 [管理器启动入口](../../apps/manager/src/main.rs) · [安装验证脚本](../../tools/verify-release.py)

## 交付前验收

| 场景 | 通过表现 |
| --- | --- |
| 壁纸发现与切换 | 刷新后能找到新 ID、正确名称、真实封面和参数。点击后生效。连点保持最终选择一致。坏包或加载失败时，保留原壁纸并提示。 |
| 输入与视觉 | 中间、边缘和空格触发位置正确。多键不会互相吞掉，同键连按自然叠加或按设计限流。按住、松开、打开暂停、切换或断开后，按设计清退。 |
| 设置与持久化 | 前景、背景各自变化。查看最小值、默认值和最大值。拖动预览与最终保存行为准确。测试 A→B→A、重启与恢复默认，原有包和用户参数能正常加载。 |
| 显示与兼容 | 实体 Action75 上核对黑边、不同键帽和物理输入响应。若改到窗口、鼠标或独占模式，另查副屏断开恢复、主屏任务栏、全屏切换和截图。 |
| 性能与稳定性 | 活动画面可持续按 60 FPS 输出，密集触发无明显掉帧、堆积或耗时尖峰。轮流测试安静、单击、同键高频、多键叠加、离开再切回和长时间运行。 |

### 如何测性能

硬件离屏运行增加 `--headless-hardware`。D3D11 可再加 `--perf-stats`，用 `perfReset` 开始，用 `perfRead` 读取播放器自身的 D3D11 GPU 时间戳、CPU 提交时间和帧间隔。D3D12 使用帧计数和操作系统的 GPU 引擎计数器。在原生作品中，按 `playerPid`、`nativeWorkerPid` 及 `gpuAdapterLuid` 分别记录两个进程的 GPU 引擎和内存样本。共享显存按进程相加可能重复记账。记录分辨率、平均帧率、负载和时长。以相同设备、配置和触发序列作前后对照。

发行验证用 `playerD3D11Perf` 标注宿主时间戳，按角色记录各自工作集。黑洞基准给出两个进程按适配器筛选的 GPU 引擎样本。用每秒约 40 次的输入建立短测，再加同键、多键和参数极值。静止场景用像素回读和空闲帧差同时验收。 [安装验证脚本](../../tools/verify-release.py) · [逐进程基准](../../examples/native-wallpapers/black-hole/benchmark.py)

`testTrigger` 的 `count` 在一次请求中重复同一个物理键码。原生包的 `acceptedTriggers` 统计事件送达次数。状态里的 `activeEffects` 和 `activeParticles` 只覆盖宿主管理的效果。测试原生「烟火」的击键间隔时，逐键发送并与待机画面比对。连续打字压力测试按时间分布输入，使用持久管道避免每键启动一个 `--command` 子进程。

`python -B tools/verify-wallpaper-behavior.py --player <播放器路径> --output <证据目录>` 用隔离播放器复验磁流体暂停、三款场壁纸休眠和原生烟火的每三键一次规则。需要 Pillow。结果 JSON 同时记录烟火的送达次数与各次像素差。 [离屏回归](../../tools/verify-wallpaper-behavior.py)

### 力度与背景如何验收

- 令两个端点相等，验证触发用固定力度。将同一位置的低力度和高力度截图对比，确认变化落在尺寸、裂纹或运动上。在非零宽度的区间触发多次，记录样本或从探针检查上、下界与变化。
- 分别把下限拖过上限和上限拖过下限，检查有效值与面板回执。再从 IPC 一次提交倒序的两端，核对归整规则、预览是否写盘、最终保存与重启结果。
- 固定键位、力度与渲染时刻，逐个拍下背景选项。确认背景变化能看见，同一个力度的按键反馈在各背景都能辨认。测切换背景后的参数、切回其他壁纸再回来，以及重新启动。
- 联调已有壁纸时，对照同一版本 `settings.json` 的 `binding.mapping` 与测试预期。若旧联调脚本写死了倍率，先核对当前分段锚点再定位回归。 [现有参数联调](../../native/windows-player/tests/controls_integration.py)

<a id="release"></a>

## 汇报与部署

- **工作区交付** · 提供壁纸 ID、包及源码路径、怎么在壁纸库选择、实际图或短片。说明使用了哪个构建、跑了哪些测试、关键性能结果、实机项目已测或待测。若用户要求切换实机，由当前工作区的可用构建和正常壁纸库入口应用。部署前备份需要迁移的用户校准和参数。
- **安装包交付** · 用户需要安装或分发时，使用 `tools/package.ps1 -Compiler '<ISCC.exe 路径>'`。优先使用独立构建目录，确保暂存内容对应当前清单。`tools/verify-release.py` 安装并卸载测试实例。发布校验会遍历安装目录中的壁纸，并检查必含和排除的包 ID。改动随包交付的基线时，同步更新相应断言。使用独立测试 Windows 环境验证安装、覆盖升级、自定义壁纸目录和用户数据保留。核对项目与每项随包素材的分发权、来源和许可证。 [打包脚本](../../tools/package.ps1) · [交付验证](../release-verification.md)
