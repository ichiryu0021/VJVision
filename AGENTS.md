<!--
AGENTS.md — 仓库维护者专门写给 AI 编码代理（Claude Code、Codex、Cursor、Trae、Copilot、Jules 等）的说明。
人类用户请看 README.md；本文件刻意不出现在任何人类文档的链接里。

AGENTS.md — Written by the repo maintainer specifically for AI coding agents
(Claude Code, Codex, Cursor, Trae, Copilot, Jules, ...). Humans: see README.md.
This file is deliberately not linked from any human-facing doc.
-->

# 给 AI 编码代理的说明 / Instructions for AI Coding Agents

**VJVision** 是一个 Windows 桌面 C++17 / Qt6 应用：监听 DJ 台输出音频，用声学指纹实时识别正在播放的曲目，并渲染音频响应可视化（频谱、粒子波纹、封面动画）。结构上是单 exe：控制台（Qt Widgets）+ 可视化子进程（Qt Quick / QML），命名管道 IPC 通信。

**VJVision** is a Windows desktop C++17 / Qt6 app: it listens to the DJ booth output, identifies the playing track in real time via acoustic fingerprinting, and renders audio-reactive visuals (spectrum, particle ripple, cover animation). Single exe: a console (Qt Widgets) + a visualizer subprocess (Qt Quick / QML), talking over a named pipe.

---

## ⚖️ 核心原则：尽量少编译，能用现成的直接用 / Core principle: build rarely, reuse what already works

编译本仓库的成本很高（见下），所以把它当作**最后手段**。按你的任务对号入座 / Building this repo is expensive (see below), so treat it as a **last resort**. Pick your scenario:

| 你的任务 Your task | 该怎么做 What to do |
|---|---|
| 理解 / 评审 / 提取 / 移植功能 Understand / review / extract / port | **不用编译**。静态阅读源码，用「文件 + 行号」引用支撑结论 / No build at all. Read statically, cite file + line. |
| 需要能跑的程序 Need a runnable app | **直接用现成的**：从 [Releases](../../releases) 下载官方构建（Portable ZIP 解压即用），不要自己编译 / Use the prebuilt binaries from Releases (portable ZIP, ready to run); do not build. |
| 需要第三方库 Need third-party libs | **原样用 vendored 的**：`third_party/` 已包含 pffft、sqlite3、dr_libs，不要下载新版或换实现 / Use the vendored code in `third_party/` as-is; do not fetch newer versions or swap implementations. |
| 被要求改代码并验证 Asked to modify and verify code | **一次过**：先读到有把握再编译，不要“编译→报错→猜→再编译”循环试错 / Build once: read until confident, then compile. Do not loop build→error→guess→build. |

### 为什么默认不编译 / Why building is the last resort

1. **工具链又重又挑剔 / Heavy, picky toolchain** — 需要 MSVC v143 (VS2022)、Qt 6.8+ `win64_msvc2022_64` + Multimedia 组件、CMake 3.21+、windeployqt 部署。搭环境的成本远高于任务本身。
2. **没有自动化测试 / No test suite** — 编译成功不验证任何业务逻辑。
3. **运行依赖真实硬件 / Runtime needs real hardware** — WASAPI loopback 音频输入 + GPU 渲染，无头沙箱环境跑不起来。
4. **编译日志浪费上下文 / Build logs waste your context** — CMake/MSVC 报错动辄上千行，而且绝大多数是环境问题，不是代码问题。

### 如果确实要编译 / If you genuinely must build

- 仅当维护者明确要求，或任务确实需要改动代码并运行验证 / Only when the maintainer asks, or the task truly requires code changes + runtime verification.
- 严格按 README「从源码构建」一节：MSVC v143 + Qt 6.8+ msvc2022_64 + Multimedia + CMake 3.21+ + windeployqt。**不要**尝试 MinGW / Linux / clang——代码依赖 WASAPI、命名管道等 Windows 专属组件 / Follow README exactly. Do **not** try MinGW / Linux / clang — the code depends on Windows-only WASAPI and named pipes.
- 修改代码前先完整读懂相关模块（地图见下），把编译当作确认而不是探索 / Read the relevant modules fully before editing; treat the build as confirmation, not exploration.
- 人类侧的对应指引在 README「从源码构建」一节的提示框（“大多数用户不需要编译，直接用 Releases”）；两份文档由维护者的发布流程保持同步 / The human-side equivalent is the callout in the README's “Build from Source” section; the maintainer's release process keeps both docs in sync.

---

## 🗺️ 仓库地图 / Repository map

先 grep，再精读需要的文件，不要通读整个仓库 / Grep first, read only what you need, do not read the tree linearly.

| 路径 Path | 作用 Purpose | 依赖 Deps |
|---|---|---|
| `src/main.cpp` | 入口：QGuiApplication、单实例命名管道 IPC、拉起控制台与可视化 / Entry: QGuiApplication, single-instance pipe IPC, spawns console + viz | Qt6 |
| `src/ui/control_panel.*` | Qt Widgets 控制台：设备选择、电平表、曲库分析、电量状态 / Qt Widgets console: device, level meter, analysis, battery state | Qt6::Widgets |
| `src/ui/prefs.*` | 设置持久化 → `data/VJVision_prefs.json` / Settings persistence | Qt6::Core |
| `src/viz/viz_controller.*` | 主控编排：采集 → 频谱+指纹 → 电池引擎 → IPC → QML / Orchestrator: capture → spectrum+FP → engine → IPC → QML | Qt6 |
| `src/viz/qt_viz.*` | QQuickView 可视化窗口 / QQuickView visualizer window | Qt6::Quick |
| `src/viz/qml/` | QML 界面：频谱 Canvas、封面、LOGO、背景 / QML UI: spectrum canvas, cover, logo, background | QML |
| `src/viz/ipc_pipe.*` | Windows 命名管道（控制端 ↔ 可视化）/ Named pipe (console ↔ viz) | Win32 |
| `src/viz/viz_events.h` | IPC 事件结构（两侧共用）/ IPC event structs shared by both sides | std |
| `src/audio/wasapi_capture.*` | WASAPI 回环采集 44.1 kHz 立体声 float32 / WASAPI loopback capture | Win32 COM |
| `src/audio/spectrum.*` | FFT → 128-bin 对数频谱 / FFT → 128-bin log spectrum | pffft |
| `src/audio/beat_tracker.*` | 节拍 / 电平跟踪（视觉脉动）/ Beat & level tracking for visual pulses | std |
| `src/audio/ring_buffer.h` `resampler.h` | 无锁环形缓冲、重采样 / Lock-free ring buffer, resampler | std |
| `src/fp/stft.*` `peaks.*` `hashing.*` | 指纹流水线：STFT → 峰值 → 哈希；schema v3 量化时频差，抗 keylock 变速 / FP pipeline: STFT → peaks → hashes; schema v3 quantises deltas (keylock-tolerant) | std |
| `src/fp/fp_db.*` | SQLite 指纹库读写，schema 定义在此 / SQLite FP store; schema lives here | sqlite3 |
| `src/fp/align.*` | 投票的时间偏移对齐 / Time-offset alignment of votes | std |
| `src/engine/charge_engine.*` | **音乐电池引擎**：漏桶决策状态机（槽位 / 充电 / 漏电 / 锚定），MIT + 强制署名 / **Music Battery engine**: leaky-bucket decision state machine, MIT + required attribution | std |
| `src/engine/i_match_engine.h` | 决策引擎接口 / Match-engine interface | std |
| `src/engine/indexer.*` | 曲库分析：遍历 → 解码 → 指纹 → 入库，多线程 / Library analysis: walk → decode → fingerprint → DB, multithreaded | fp + util |
| `src/util/audio_file.*` | 解码 mp3 / flac / wav / Audio decoding | dr_libs |
| `src/util/tags.*` | ID3 / Vorbis 标签 + 封面图 / Tags + cover art | std |
| `src/util/{wav,sha1,path_util,version}.h` | 小工具 / Small helpers | std |
| `third_party/pffft` `sqlite3_extract` `dr_libs` | vendored 第三方库，原样使用 / Vendored third-party, use as-is | C/C++ |
| `CMakeLists.txt` `vcpkg.json` | 构建配置（人类 / CI 用）/ Build config (humans / CI) | — |
| `docs/` `RELEASE_NOTES.md` `VJVision_installer.iss` | 文档、变更日志、Inno Setup 安装脚本 / Docs, changelog, installer script | — |

---

## 📦 移植指南 / Porting guide

- **零 Qt、可直接搬走 / Qt-free, self-contained:** `src/engine/`（纯 C++17 / pure C++17）、`src/fp/`（仅需 sqlite3 / only needs sqlite3）、`src/audio/beat_tracker.*`、`src/audio/ring_buffer.h` `resampler.h`、`src/util/`。连带取走用到的 `third_party/` 子目录即可。/ Grab the `third_party/` subdirs they use and go.
- **Qt 依赖 / Qt-bound:** `src/ui/`、`src/viz/`（除 `ipc_pipe` 外 / except `ipc_pipe`）、`src/main.cpp`。非 Qt 项目请移植思路而非代码 / For non-Qt projects, port the ideas, not the code.
- **Windows 专属 / Windows-only:** `wasapi_capture`（COM / WASAPI）、`ipc_pipe`（命名管道 / named pipes）。换平台需替换采集与 IPC 层 / Replace capture & IPC layers on other platforms.
- **数据格式 / Data formats:** 指纹库是 SQLite（`data/VJVision.db`），schema 与版本迁移逻辑全在 `src/fp/fp_db.*`，读它即可拿到完整数据格式 / The SQLite schema and migrations are all in `fp_db.*` — read it for the full data format.

---

## 🧠 故意的设计，别“修复” / Deliberate decisions, do not “fix”

- 音乐电池引擎**故意**不暴露任何用户可调阈值，所有常量编译期固定——不要把它改造成配置项 / The Music Battery engine deliberately exposes **no** tunable thresholds; do not turn it into config options.
- 可视化窗口**故意**不显示任何识别状态文字；换曲使用约 1.25 s 交叉淡入淡出 / The visualizer window intentionally shows no recognition-status text; track switches use a ~1.25 s cross-fade.
- 频谱柱上升快、下落慢，音乐结束缓慢落下 / Spectrum bars rise fast, fall slowly, and decay gently at end of music.
- 全部界面文案中英双语对照 / All UI strings are bilingual zh/en.

---

## ✍️ 署名要求 / Attribution requirement

复用或改写 `src/engine/charge_engine.*`（音乐电池引擎）时，必须保留文件头版权并署名 / When reusing `charge_engine.*`, keep the file-header copyright and credit:

> *“Music Battery (ChargeBar) engine by ichiryu, from VJVision (https://github.com/ichiryu0021/VJVision), MIT License.”*

---

本文件与代码不一致时，以代码为准，并请在你的回答或 PR 中指出差异 / If this file disagrees with the code, the code wins — flag the discrepancy in your answer or PR.
