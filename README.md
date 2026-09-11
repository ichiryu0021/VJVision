<div align="center">

# VJVision 2.0

**实时音频识别 + 音频响应可视化，专为 DJ 现场设计**
<br>*Real-time audio recognition + audio-reactive visualizer for DJ live sets.*

[![Release](https://img.shields.io/github/v/release/ichiryu0021/VJVision?logo=github&label=Release)](https://github.com/ichiryu0021/VJVision/releases/latest)
[![License](https://img.shields.io/github/license/ichiryu0021/VJVision?logo=opensourceinitiative&label=License)](LICENSE)
[![Stars](https://img.shields.io/github/stars/ichiryu0021/VJVision?logo=github&label=Stars)](https://github.com/ichiryu0021/VJVision/stargazers)
[![Language](https://img.shields.io/github/languages/top/ichiryu0021/VJVision?logo=c%2B%2B&label=C%2B%2B)](https://github.com/ichiryu0021/VJVision)

[**下载最新版**](https://github.com/ichiryu0021/VJVision/releases/latest) · [**快速开始**](#快速开始--quick-start) · [**构建源码**](#从源码构建--build-from-source) · [**支持项目**](#支持项目--support)

</div>

---

## 目录 / Table of Contents

- [简介 / Introduction](#简介--introduction)
- [功能 / Features](#功能--features)
- [系统要求 / Requirements](#系统要求--requirements)
- [快速开始 / Quick Start](#快速开始--quick-start)
- [数据目录 / Data Folder](#数据目录--data-folder)
- [控制台分区 / Console Layout](#控制台分区--console-layout)
- [可视化快捷键 / Visualizer Shortcuts](#可视化快捷键--visualizer-shortcuts)
- [从源码构建 / Build from Source](#从源码构建--build-from-source)
- [识别参数 / Recognition Tunables](#识别参数--recognition-tunables)
- [常见问题 / FAQ](#常见问题--faq)
- [技术架构 / Architecture](#技术架构--architecture)
- [版本历史 / Changelog](#版本历史--changelog)
- [许可证 / License](#许可证--license)
- [支持项目 / Support](#支持项目--support)

---

## 简介 / Introduction

VJVision 监听 DJ 台的输出音频，自动识别当前播放的曲目，渲染随音乐律动的频谱、封面动画与粒子波纹。

VJVision listens to the DJ booth output, auto-recognises the playing track, and renders a music-reactive spectrum, cover animation and particle ripple.

---

## 功能 / Features

| 功能 Feature | 说明 Description |
|---|---|
| 🎵 **自动曲目识别** | 声学指纹（FFT → 峰值 → 哈希 → SQLite 索引），12 秒采样窗口实时识别 / Acoustic fingerprint, 12 s sampling window |
| ⚡ **C++ 重写** | 全栈 C++17 + Qt 6，比 Python V1 启动快 10×、内存占用低 60% / C++17 + Qt 6; 10× faster startup, 60% less memory |
| 🎮 **GPU 硬件加速** | Qt Quick Scene Graph（OpenGL/Vulkan 自动选择），60fps 高分辨率稳定渲染 / Qt Quick Scene Graph, steady 60 fps |
| 🌊 **音频响应可视化** | 柱状 / 镜像 / 放射 / 瀑布频谱、粒子波纹、封面随节拍脉冲 / Bar, mirror, radial, waterfall spectrum, particle ripple, cover pulse |
| 🖼️ **待机 LOGO 自定义** | 支持 PNG/JPG/GIF（透明通道保留、自动循环动画）/ PNG/JPG/GIF with alpha and auto-loop |
| 🎬 **背景媒体自定义** | 纯色 / GIF / WEBP / MP4 / MOV / MKV，PreserveAspectCrop 填满屏幕无黑边 / Solid, GIF, video backgrounds, edge-cropped fill |
| 🖥️ **多显示器支持** | 默认窗口化，拖到任意屏幕按 `F` 全屏 / Windowed by default, press `F` for fullscreen on any display |
| 🗃️ **无 DB 也能跑** | 没有指纹库时仅渲染频谱与波形（standby-only 模式）/ Spectrum + ripple only when no fingerprint DB |
| 🌐 **中英双语界面** | 一键切换 中文 / English / One-click Chinese / English switch |
| 💾 **便携部署** | exe + Qt 运行库 + 空 `data/` 文件夹拷走即用 / Portable: exe + Qt runtime + empty `data/` folder |

---

## 系统要求 / Requirements

| 项目 Item | 要求 Requirement |
|---|---|
| 操作系统 OS | Windows 10 / 11 x64 |
| CPU | Intel / AMD x64，支持 SSE2（大部分 2010 年后的 CPU） |
| 内存 RAM | 4 GB 起；分析大曲库建议 8 GB+ |
| 显卡 GPU | 支持 OpenGL 3.0 的任意显卡（Qt Quick 自动选择 GL/VK） |
| 音频输入 Audio | WASAPI 回环（推荐，零延迟立体声）/ DirectSound / MME；虚拟音频线（VB-Cable 等）推荐 |
| 存储 Storage | 500 MB（程序 + Qt 运行库）；曲库按每首约 2–3 KB 指纹库扩展 |
| 显示器 Displays | 至少 1 块；2 块推荐（主屏控制台 + 副屏可视化） |

---

## 快速开始 / Quick Start

### 下载 / Download

从 [GitHub Releases](https://github.com/ichiryu0021/VJVision/releases/latest) 下载最新版压缩包，解压到任意位置。

Download the latest release from [GitHub Releases](https://github.com/ichiryu0021/VJVision/releases/latest) and extract it anywhere.

### 目录结构 / Layout

```
VJVision/
├── VJVision.exe           ← 双击运行入口
├── Qt6Core.dll ...        ← Qt 运行库（windeployqt 自动部署）
├── avcodec-61.dll ...     ← FFmpeg（MP4 播放）
├── qml/                   ← QML 插件
├── platforms/             ← Windows 平台插件
├── multimedia/            ← 多媒体后端（WASAPI 捕获 + FFmpeg 解码）
└── data/                  ← 空目录；首次运行自动生成内容
```

### 首次启动 / First Launch

```
1. 双击 VJVision.exe
   控制台打开；data/ 文件夹在 exe 同级自动创建
2. （可选）选择「数据文件夹」→「音乐目录」→ 点「分析」
   生成 VJVision.db 指纹库
3. 选择音频设备（监听 DJ 输出的声卡/虚拟线）
   放音乐时电平表应有跳动
4. 选待机 LOGO（可选 PNG/GIF）和背景（默认/自定义）
5. 点「启动可视化」→ viz 窗口弹出
   拖到任意屏幕，按 F 全屏
```

启动后也可以**跳过分析**，直接看频谱 + 波纹（standby 模式）。
You can also **skip analysis** and enjoy spectrum + ripple visuals right away (standby mode).

---

## 数据目录 / Data Folder

全部位于 `data/` 文件夹内（与 exe 同级，首次运行自动创建）/ All inside `data/` (next to the exe, auto-created on first launch):

| 文件 File | 内容 Contents |
|---|---|
| `VJVision.db` | 音频指纹库（SQLite）— 执行「分析」后生成 / Fingerprint DB (SQLite), created after analysis |
| `VJVision_prefs.json` | 全部设置：音频设备、语言、置信度阈值、LOGO、背景、遮罩 / All settings: device, language, confidence gates, logo, background, overlay |
| `covers/` | 专辑封面缓存 / Album cover cache |

> **便携迁移 / Portable migration** — 把整个 `VJVision/` 文件夹（exe + dll + qml + data/）拷到 U 盘或另一台电脑，设置和指纹库完全保留，无需重新分析。换电脑后音频设备索引可能需要重选。
> Copy the whole `VJVision/` folder to a USB stick or another machine — settings and fingerprints carry over. Only the audio-device index may need re-picking on a different machine.

---

## 控制台分区 / Console Layout

| 区域 Region | 内容 Contents |
|---|---|
| 音频硬件 | 设备下拉 + 刷新 + **输入电平表**（PEAK 削波指示）/ Device dropdown, refresh, **level meter** with PEAK clip indicator |
| 数据 | 数据文件夹、音乐目录、**分析**按钮 + 进度条 + 歌曲数 / Data folder, music folder, **Analyze** button + progress + track count |
| 识别阈值 | 三道置信度高级调节 + **恢复默认** / Three confidence gates + **Reset to defaults** |
| 视觉效果 | 待机 LOGO（浏览 + 清除）、背景来源（默认/自定义）、底色/背景媒体、黑色遮罩深度 / Standby logo, background source, colour or media picker, black overlay depth |
| 识别状态 | 当前歌曲、识别日志、错误信息 / Current track, recognition log, errors |
| 底部整条 | **启动可视化 / 停止** 大按钮 / **Start / Stop Visualizer** bar |

---

## 可视化快捷键 / Visualizer Shortcuts

| 键 Key | 动作 Action |
|---|---|
| `F` | 全屏 ↔ 窗口切换 / Toggle fullscreen ↔ windowed |
| `Esc` | 仅全屏时退出全屏 / Exit fullscreen only |

---

## 从源码构建 / Build from Source

**Windows（官方支持）/ Windows (officially supported):**

```powershell
# 前置 / Prerequisites
# - CMake 3.21+
# - Visual Studio 2022 (MSVC v143)
# - Qt 6.8+ with MSVC 2022 64-bit + Multimedia component
#   （用 MaintenanceTool 装 Qt，或 aqtinstall: pip install aqtinstall && aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtmultimedia）

git clone https://github.com/ichiryu0021/VJVision.git
cd VJprg          # Trae cwd 名；git 仓库相同
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# 产物 / Output: build/Release/VJVision.exe
# 部署 Qt 运行库 / Deploy Qt runtime:
windeployqt --qmldir src/viz/qml build/Release/VJVision.exe
```

---

## 识别参数 / Recognition Tunables

| 参数 Param | 默认 Default | 说明 Description |
|---|---|---|
| 首曲确认置信度 First-track | 0.25 | 认出第一首歌的门槛 / Floor to accept first track |
| 噪声下限 Noise floor | 0.13 | 低于此值视为环境噪声 / Below this → ambient noise |
| 切歌确认置信度 Switch-confirm | 0.30 | 从脉动预览真正切到下一首的门槛 / Floor to confirm the switch |
| 确认次数 Confirmations | 2 | 连续命中几次才切歌 / Consecutive hits required |

---

## 常见问题 / FAQ

**没有声音 / 电平表不动？/ No signal on the level meter?**
确认选对了监听 DJ 输出的设备；音量太低时 WASAPI 回环会静默，把 DJ 台推子推到正常音量。
Make sure you picked the device monitoring the DJ output; WASAPI loopback stays silent when volume is too low.

**MP4 背景不播放？/ MP4 background doesn't play?**
Qt Multimedia 需要 FFmpeg 后端；windeployqt 输出里应该有 `avcodec-61.dll`、`avformat-61.dll`、`avutil-59.dll`。确认这些文件在 exe 同级。
Qt Multimedia needs an FFmpeg backend; confirm `avcodec-61.dll`, `avformat-61.dll`, `avutil-59.dll` are next to the exe.

**GIF 透明 LOGO 变黑底？/ Animated GIF shows black background?**
确认 GIF 自身带 alpha 通道；AnimatedImage 保留原始 alpha。用 FFmpeg 检查：`ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of default=noprint_wrappers=1 input.gif`，预期输出 `rgba`。
Confirm the GIF actually has an alpha channel; AnimatedImage preserves whatever alpha is there. Check with FFmpeg: expect `rgba`.

**GIF LOGO 播放一次就停？/ Animated GIF stops after one loop?**
部分 GIF 制作工具默认只设置循环 1 次（而非无限循环）。用 FFmpeg 重新封装强制无限循环：`ffmpeg -i input.gif -loop 0 output.gif`（`-loop 0` = 无限循环）。VJVision 会尊重 GIF 自身的循环设置，不会强行覆盖。
Some GIF authoring tools default to a single loop. Re-encode with FFmpeg to force infinite looping: `ffmpeg -i input.gif -loop 0 output.gif`. VJVision respects the GIF's own loop setting and does not override it.

**VJVision 启动闪退？/ VJVision crashes on launch?**
1. 看 exe 同级有没有 `VJVision_prefs.json`，删了让它重新生成默认值
2. 确认 Qt 运行库完整部署（windeployqt 跑过）
3. 确认显卡驱动支持 OpenGL 3.0+

---

## 技术架构 / Architecture

```
main.cpp          ← QGuiApplication + IPC server (named pipe)
├── ControlPanel  ← QWidget 控制台 (Qt Widgets)
└── QtVizSink     ← QQuickView 子进程 (Qt Quick / QML)
viz_controller.cpp  ← 主控逻辑（音频采集 + 指纹识别 + 推给 QML）
viz.qrc             ← qrc:/viz/qml/viz.qml（编译进 exe）
└── viz.qml         ← 波形 Canvas + MediaPlayer + cover Image + logo AnimatedImage
```

- **音频采集**：WASAPI 回环立体声 44.1kHz float32 / WASAPI loopback, stereo 44.1 kHz float32
- **频谱**：pffft（SIMD FFT）→ 128-bin 对数频谱 / pffft (SIMD FFT) → 128-bin log spectrum
- **指纹**：自己实现 STFT → 峰值 → 哈希；SQLite 存储 / In-house STFT → peaks → hash; SQLite storage
- **IPC**：Windows Named Pipe（控制端 ↔ 可视化子进程）/ Windows Named Pipe (console ↔ visualizer subprocess)
- **QML 桥接**：ContextProperty `viz`（QtVizSink 暴露 Q_PROPERTY 给 QML）/ ContextProperty `viz` exposes Q_PROPERTY to QML

---

## 版本历史 / Changelog

见 [RELEASE_NOTES.md](RELEASE_NOTES.md) / See [RELEASE_NOTES.md](RELEASE_NOTES.md).

---

## 许可证 / License

MIT — 详见 [LICENSE](LICENSE)。
MIT — see [LICENSE](LICENSE).

---

## 支持项目 / Support

<div align="center">

如果 VJVision 在你的现场演出中帮到了你，欢迎赞助一杯咖啡，让项目持续迭代。
<br>*If VJVision has helped your live sets, consider sponsoring a coffee to keep it evolving.*

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor%20on%20GitHub-ichiryu0021-pink?logo=github)](https://github.com/sponsors/ichiryu0021)

### 中国用户 / For users in China

| 支付宝 Alipay | 微信 WeChat |
|:---:|:---:|
| ![支付宝二维码](docs/sponsor-alipay.png) | ![微信二维码](docs/sponsor-wechat.png) |
| *扫码赞助 / Scan to sponsor* | *扫码赞助 / Scan to sponsor* |

</div>

> 二维码图片请替换为真实收款码：`docs/sponsor-alipay.png` 与 `docs/sponsor-wechat.png`。
> Replace the QR code images with your real payment codes: `docs/sponsor-alipay.png` and `docs/sponsor-wechat.png`.
