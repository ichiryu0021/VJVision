# VJVision 发布说明 / Release Notes

> v2.0.0 — 首个正式稳定版 / First official stable release.

---

## v2.0.0 (2026-09-10)

### 🚀 里程碑 / Milestone

**VJVision 从 Python V1 完整重写为 C++17 + Qt 6**。启动速度提升 ~10×，内存占用降低 ~60%，部署简化为单 exe + Qt 运行库，指纹库与设置通过空 `data/` 文件夹自动初始化。

**Full C++17 + Qt 6 rewrite of VJVision.** ~10× faster startup, ~60% less memory, deployment simplified to a single exe + Qt runtime, fingerprints and settings auto-generated in an empty `data/` folder on first launch.

### ✨ 新增功能 / New Features

| # | 功能 Feature | 说明 Description |
|---|---|---|
| 1 | **纯 C++17 + Qt 6** | 移除 Python / PyInstaller 依赖；直接 MSVC 编译成原生 exe |
| 2 | **Qt Quick Scene Graph GPU 渲染** | OpenGL/Vulkan 自动选择；60fps 稳定渲染 |
| 3 | **WASAPI 回环音频采集** | 立体声 44.1kHz float32；零延迟、内置设备枚举 |
| 4 | **pffft SIMD FFT** | 128-bin 对数频谱；比 Python numpy.fft 快 3–5× |
| 5 | **自研声学指纹引擎** | STFT → 峰值 → 哈希 → SQLite 索引；移除 Dejavu 依赖 |
| 6 | **Windows Named Pipe IPC** | 控制台 ↔ 可视化子进程；比 multiprocessing.Queue 更轻 |
| 7 | **待机 LOGO 支持 GIF/WEBP 动画** | AnimatedImage 保留透明通道；自动循环播放 |
| 8 | **自定义背景媒体** | 默认纯色 / 自定义 GIF、WEBP、MP4、MOV、MKV |
| 9 | **PreserveAspectCrop 缩放** | 填满屏幕、裁剪边缘、无黑边 |
| 10 | **黑色遮罩深度** | 自定义背景上盖 0–100% 半透明遮罩 |
| 11 | **任意屏幕窗口化 + F 全屏** | 拖到任意屏幕按 F 切换全屏；无需选显示器 |
| 12 | **无 DB standby 模式** | 没有指纹库也能启动 viz（仅频谱 + 波纹） |
| 13 | **空 data/ 自动初始化** | 首次运行自动生成 VJVision.db + prefs.json |
| 14 | **置信度阈值可恢复默认** | 新增「恢复默认」按钮 |

### 🔧 改进 / Improvements

- **启动速度**：从冷启动 ~5s 降到 <1s（Release 编译）
- **内存占用**：运行时 ~80 MB（Python V1 为 ~200–300 MB）
- **崩溃恢复**：父进程退出时 viz 子进程自动关闭；无音频设备也能启动
- **构建简化**：cmake + windeployqt，无 PyInstaller 复杂 spec

### ⚠️ 破坏性变更 / Breaking Changes

- **数据格式不兼容**：V1 的 `fingerprints.db`（Dejavu）无法在 V2 直接使用，需重新「分析」曲库
- **prefs.json 格式变更**：字段名全部更新，首次启动自动迁移旧格式（legacy dataDir 从 dbPath parent 推断）
- **打包结构变更**：从 PyInstaller onedir → CMake + windeployqt；部署产物为 exe + Qt 运行库 dll/qml 目录

### 🛠️ 已知问题 / Known Limitations

- **MP4 背景不能用 MultiEffect 做 blur**（Qt Multimedia 的 VideoOutput 是原生 GL surface，不能当 FBO source）
- **macOS 暂无官方打包**：源码兼容但 macOS 打包由 mac 分支维护者构建
- **Linux 暂无官方打包**：源码未在 Linux 上测试

### 📦 依赖版本 / Dependencies

| 组件 | 版本 |
|---|---|
| C++ 标准 | C++17 |
| CMake | ≥ 3.21 |
| Qt | 6.8+（Core / Gui / Quick / Widgets / Multimedia / Svg） |
| pffft | 1.0.0 |
| SQLite3 | amalgamation 3.49.1 |
| dr_libs | dr_mp3 + dr_flac |
| MSVC | v143 (Visual Studio 2022) |

### 📄 第三方许可证 / Third-Party Licenses

| 库 Library | 许可证 License |
|---|---|
| Qt | LGPL 3.0 / GPL 3.0 (commercial available) |
| pffft | BSD 2-Clause |
| SQLite3 | Public Domain |
| dr_libs | Public Domain |
| FFmpeg (Qt Multimedia backend) | LGPL 2.1 |

---

## v1.x 历史 / v1.x History

（略 — V1 为 Python + Dejavu + CustomTkinter 版本，详见 git tag 历史）
