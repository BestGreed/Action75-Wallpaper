# 潮标 · 偏岸

这是一个独立可导入的 CPU 粒子包。导入文件夹为 `tidemark-bearing`。青色刻线标示触发原点，橙色浮标的水平位置由作者定义的“浮标偏岸”控制，范围 −1 至 +1，默认 +0.5。

参数 `bearing` 绑定 `effect.dynamicInput` 的 `index: 0`。原创工程 `tidemark.efkproj` 把 `@O.x = @In0 * 60` 应用于橙色粒子的固定位置，青色粒子独立。两种颜色使用 Effekseer 的 CPU sprite 内置材质。工程、封面和参数清单由 build.py 创建。

用支持动态输入的宿主，保持 KeyG、430 ms 目标采样时刻与同一份 .efk。软件读回中青色中心均为 x=739，橙色中心从 x=678.5（输入 −1）移到 x=798.5（输入 +1），相差 120 像素。证据保存在 evidence/bearing--1、evidence/bearing-1 和 evidence/measurements.json；测量文件同时记录宿主和资源 SHA-256。verify.py 通过两种颜色的通道差异测定各自中心，并检查方向和移动距离。

旧版宿主在选包时报 Unsupported wallpaper settings target。evidence/resource-default 是旧版使用临时清单绕开不支持的控制项、对原始 .efk 默认输入 +0.5 的诊断回读，实测浮标在原点右侧 29.5 像素。

## 复验

```powershell
python build.py --editor <Effekseer.exe>
python verify.py --player <支持动态输入的Action75Native.exe> --host-root <带校准和键位表的宿主资源目录> --trace <作者技能/scripts/trace.py>
```

参考资料：[Effekseer 编辑器的动态参数](https://effekseer.github.io/Helps/18x/Tool/en/ToolReference/dynamicParameter.html)。
