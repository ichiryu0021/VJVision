# VJVision 发布说明 / Release Notes

> 当前所有版本均为 **beta 测试版**（GitHub Release 均标记为 Pre-release），尚未发布正式版。
> All releases are **beta pre-releases** (every GitHub Release is marked Pre-release); there is no stable release yet.
>
> **格式约定 / Format**：每个版本都包含中文与英文两段（中文在前，英文在 `### English` 段），内容一一对应、同步维护。
> Each version contains both a Chinese block and an `### English` block covering the same changes.

## v1.3.0-beta (2026-09-09)

### 识别置信度可调（高级选项）
- 控制台左列新增红色标题的「**⚠️ 识别置信度调整**」面板，三个数字输入框分别控制识别链路的三道门槛：
  - **第一首置信度**（默认 0.25）：认出第一首歌的门槛
  - **脉动触发置信度**（默认 0.06）：切歌时先进入脉动预览（暂不换歌）的门槛
  - **切歌确认置信度**（默认 0.30）：从脉动预览真正切到下一首的门槛
- 输入后按回车或点击别处即生效，数值自动限定在 0.05–0.95 范围；三个阈值持久化到 `prefs.json`，重启后自动恢复
- 面板带橙色警告提示：该选项会影响识别精确度，正式表演前请先测试——越低识别速度越快，越高识别精确度越高

### 控制台交互优化
- 「开始采集 / 停止采集」两个按钮合并为控制台**底部整条长按钮**：待机时绿色「开始采集」，采集运行中变为红色「停止采集」，状态一目了然；采集启动失败时按钮自动回弹为绿色，不再卡在错误状态
- 语言下拉选项改为自描述显示：「中文(Chinese)」「English(英语)」，中英文用户都能直接看懂

### 稳定性
- 配置写入改为临时文件 + 原子替换（失败自动重试 5 次），`prefs.json` 不会因写入中途中断而损坏；写入彻底失败时在日志中记录警告而非静默丢失设置
- 置信度输入框改用字符串变量承载：输入非数字内容时不再在后台打印异常堆栈（输入仍被安全忽略，回车/失焦后数值自动还原）

### English

#### Adjustable recognition confidence (advanced)
- New red-titled "**⚠️ Recognition Confidence**" panel in the console's left column, with three numeric inputs for the three gates of the recognition pipeline:
  - **First-track confidence** (default 0.25): floor to accept the first song
  - **Pulse-trigger confidence** (default 0.06): floor that starts the pulsing preview (song not yet switched) on a new song
  - **Switch-confirm confidence** (default 0.30): floor to actually switch from the pulsing preview to the next song
- Values apply on Enter or focus-out and are auto-clamped to 0.05–0.95; all three floors are persisted to `prefs.json` and restored on restart
- An amber warning on the panel notes that these affect recognition accuracy — test before a live performance: lower = faster recognition, higher = more accurate

#### Console UX
- The two "Start Capture" / "Stop Capture" buttons are merged into one **full-width bar pinned to the bottom** of the console: green "Start Capture" when idle, red "Stop Capture" while a capture is running; if capture fails to start, the button snaps back to green automatically instead of sticking in the wrong state
- Language picker now shows self-describing labels: "中文(Chinese)" / "English(英语)", readable by both Chinese- and English-speaking users

#### Stability
- Prefs writes now go through a temp file + atomic replace (5 retries on failure), so `prefs.json` can never be corrupted by an interrupted write; a total write failure logs a warning instead of silently dropping settings
- Confidence entries now use string-backed variables: typing non-numeric text no longer prints an exception traceback in the background (the input is still safely ignored and the value restores on Enter/focus-out)

---

## v1.2.1-beta (2026-09-09)

### 稳定性：无音频硬件不再闪退
- 修复电脑没有声卡/音频驱动异常/Windows Audio 服务禁用时软件启动闪退：音频初始化失败改为明确的中文提示（"未检测到可用的音频设备或驱动…"），软件其余功能（可视化、曲库分析）照常可用
- 识别线程命令处理改为逐条防护：单条命令异常不再导致电平表/识别整体停摆；运行中拔出音频设备不再崩溃
- 可视化子进程隔离音频子系统（`SDL_AUDIODRIVER=dummy`），无音频硬件时可视化窗口正常显示
- 新增全局异常钩子：任何未捕获异常都会写入 `cache/vjvision.log`，打包版崩溃不再"没有日志"
- 修复关闭可视化窗口后点重启按钮闪退（旧退出消息残留在队列、毒化新进程）；「重启」与「重置画面」两个按钮合并为一个 **🔄 重置可视化窗口**

### 曲库分析：损坏音频自动容错
- 新增 **ffmpeg 容错解码兜底**：soundfile 无法解码的损坏音频（如下载截断的 FLAC 报 "decoder lost sync"）会自动改用 ffmpeg 跳过坏帧解码，不再整首分析失败；兜底成功在日志中标注 `Recovered via ffmpeg fallback`
- ffmpeg 不可用或文件损坏严重时，给出可操作的中文提示（安装 ffmpeg 或转码后重试），不再只显示英文 `LibsndfileError`
- 真实曲库随机抽样 100 首 FLAC 批量验证全部通过（平均 3.4 秒/首）

### 界面文案
- 「⚠ 强制重建索引」更名为「**⚠ 强制重新分析**」（中英文同步），日志面板相关提示一并更新，更便于理解

### English

#### Stability: no more crash without audio hardware
- Fixed a startup crash when the PC has no sound card, the audio driver is broken, or the Windows Audio service is disabled: audio initialization failure now shows a clear message ("No usable audio device or driver detected…") while the rest of the app (visualizer, library analysis) keeps working.
- Matcher command handling is now guarded per command: a single failing command no longer stops level metering/recognition entirely; unplugging the audio device while running no longer crashes.
- The visualizer subprocess isolates its audio subsystem (`SDL_AUDIODRIVER=dummy`), so the visualizer window opens even with no audio hardware.
- Added a global exception hook: every uncaught exception is written to `cache/vjvision.log`, so packaged builds no longer crash "without any log".
- Fixed a crash when clicking restart after manually closing the visualizer window (a stale quit message lingered in the queue and poisoned the new process); the two "Restart" / "Reset" buttons are merged into one **🔄 Reset Visualizer**.

#### Library analysis: automatic tolerance for corrupt audio
- Added an **ffmpeg tolerant-decode fallback**: files libsndfile cannot decode (e.g. a truncated download FLAC reporting "decoder lost sync") are automatically decoded via ffmpeg, which skips broken frames, instead of failing the whole song. Successful fallbacks are logged as `Recovered via ffmpeg fallback`.
- When ffmpeg is unavailable or the file is too badly corrupted, an actionable message is shown (install ffmpeg or re-encode the file and retry) instead of a bare English `LibsndfileError`.
- Verified with a random sample of 100 FLAC files from the real library — all passed (avg 3.4 s/song).

#### UI wording
- "⚠ Force Re-index" renamed to "**⚠ Force Re-analyze**" (both Chinese and English); related log-panel messages updated for clarity.

---

## v1.2.0-beta (2026-09-09)

### GPU 硬件加速渲染
- 可视化窗口新增 SDL2 GPU 渲染路径（`pygame._sdl2.video.Renderer`，`accelerated + vsync`），封面旋转/背景缩放/频谱绘制全部走 GPU 纹理，垂直同步消除撕裂
- 控制台"可视化显示"区新增 **GPU 硬件加速** 开关，切换后自动重启可视化窗口生效；偏好持久化到 `prefs.json`
- GPU 初始化失败时自动回退软件渲染，不影响使用
- **原生分辨率全屏**：子进程声明 DPI 感知 + 桌面全屏模式，4K/2.5K 屏全屏不再回退到 640×480
- 修复 GPU 纹理默认最近邻缩放导致的背景马赛克/色带（启用双线性缩放提示）；修复叠加层未用透明通道导致的半屏黑屏

### 演示模式改为控制台开关
- 原 D 键热键移除，改为控制台"可视化显示"区的**演示模式**复选框（无音频时旋转随机封面测试渲染）
- 修复 GPU/演示复选框未传文案导致界面显示字面量类名的问题

### 控制台界面优化
- 默认窗口大小按内容自适应：所有面板一屏完整显示，无需滚动（窗口拖小时滚动条仍会自动出现）
- 移除字体下拉框下方的示例文字行；字体缺字检测保留，缺字警告改到日志面板显示

---

## v1.1.4-beta (2026-09-08)

### 中英双语界面
- 新增 `vjvision/i18n.py` 国际化模块，91 个翻译键，中文/英文全覆盖
- Debug UI 右上角新增语言下拉选择器（中文 / English），切换即时生效
- 语言偏好持久化到 `prefs.json`，重启后自动恢复
- 控制台窗口标题、所有按钮/标签/状态文本、对话框、可视化窗口状态文案均双语化
- **PEAK 指示灯**：无论何种语言均显示 `PEAK`
- 可视化窗口状态文案（Listening / Mixing / Matching / No match / Pending）随语言切换

## v1.1.3-beta (2026-09-08)

### 混音切歌提速
- 识别间隔 `match_interval` 从 6s 缩短到 4s
- **混音（脉动）期间识别频率翻倍**：间隔再减半到 ~2s，更快捕捉新曲置信度爬升
- 混音确认阈值 `MIX_MIN_CONFIDENCE` 从 0.40 降到 0.30（长混音中新曲置信度常被稀释到 0.30–0.38，0.40 太严导致一直卡在 Mix hold）
- 实测：长混音从检测到确认由 ~28s 缩短到 ~15s

### 首歌识别
- 第一首歌确认阈值从 0.30 降到 0.25，减少启动等待时间

### 指纹库错误可见性
- 修复多进程 worker 的异常被静默吞掉的问题：worker 现在把异常字符串返回主进程，主进程以 `ERROR` 级别记录真实失败原因
- 用户之前只看到 "Failed: xxx.flac" 而不知原因（旧版 pydub 不支持 24-bit FLAC）

### 指纹重新分析
- 117 首歌全部用当前调优参数（wratio=0.25, fan=3, amp_min=15）重新生成指纹

---

## v1.1.2-beta (2026-09-08)

### 首歌误识别修复
- **第一首歌跳过 tentative 脉动预览**：在还没有任何确认歌曲时，置信度 0.06–0.30 的暂认匹配不再锁定显示，避免低置信度错误匹配（如 conf=0.07 的错误歌曲）触发脉动并挡住真正的歌曲
- 第一首歌必须等到置信度 ≥ 阈值才确认，确认后 tentative 机制照常工作（用于混音过渡检测）

---

## v1.1.1-beta (2026-09-08)

### 关闭控制台后视觉窗口残留修复
- **visualizer 子进程增加父进程存活检测**：每 ~0.5s 检查父进程是否存活，父进程被强制终止（如关闭控制台窗口）时子进程自动 `pygame.quit()` 退出，不再变成孤儿进程
- **Debug UI 绑定 `WM_DELETE_WINDOW`**：点击窗口 X 按钮走正常 quit 流程，确保 main.py 的 `finally` 块（含 `viz_mgr.stop()`）被执行

---

## v1.1.0-beta (2026-09-08)

### 项目重命名
- `VJ-Visual` → `VJVision`，包目录 `vjvisual` → `vjvision`，所有引用同步更新
- 配置路径：`LOG_FILE=vjvision.log`、`PREFS_DIR=VJVision`

### 混音脉动机制
- 新增 tentative zone（暂认区）：置信度 0.06–0.30 的不同歌曲信号触发"混音"状态，视觉窗口对当前歌曲做脉动效果，提示 DJ 正在切歌
- Mix hold：连续 2 次高置信度命中确认后才正式切换，避免误切

### 指纹管线优化
- dejavu 指纹参数调优（wratio 0.5→0.25, fan 5→3, amp_min 10→15, peak_neighborhood 10→20），哈希量减少 ~85–90%
- 解码改用 soundfile（libsndfile），原生支持 24-bit FLAC，去掉 pydub/ffmpeg 依赖
- 多进程 worker 复用 FingerprintDB 实例，消除每首歌的 dejavu 初始化开销

### Python 3.13+ 兼容
- 注入 numpy 版 `audioop` shim，解决 Python 3.14 移除 stdlib `audioop` 后 pydub 导入崩溃的问题
