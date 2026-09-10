# VJVisionFX：音乐节奏驱动的背景纹理系统 - 实施计划

> 说明：所有 Task 均在 `<FX> = c:\Users\jason\Documents\trae_projects\VJprg\VJVisionFX`
> 内实施（沙箱限制先嵌套在主仓内，已加入主仓 `.git/info/exclude`；交付后由用户
> 移动为同级目录 `c:\Users\jason\Documents\trae_projects\VJVisionFX`）。

## Task 1: Fork 为独立项目 VJVisionFX 并完成品牌化
- **Status**: `completed`
- **Priority**: `high`
- **Depends On**: None
- **Description**:
  - 在 `<FX>` 复制源码/配置/文档（main.py、build.bat、release.ps1、VJVision.spec、
    requirements.txt、README.md、RELEASE_NOTES.md、LICENSE、.gitignore、整个 vjvision/ 包），
    排除 .git/cache/dist/build/*.log/__pycache__/.trae。
  - 新目录 `git init` + 首次提交 `chore: initial fork of VJVision as VJVisionFX`。
  - 品牌化：`__init__.py` 版本 `0.1.0-beta`；i18n `app.title` zh/en = VJVisionFX；
    README 主标题 VJVisionFX + FX 分支说明。
- **Acceptance Criteria Addressed**: AC-1
- **Completion Evidence**:
  - `<FX> = c:\Users\jason\Documents\trae_projects\VJVisionFX`，独立 git 仓库；
    首次提交 `06cc325`（19 tracked files）；与 VJprg 不共享 .git；已从 VJprg 内
    移出为同级目录，主仓 `.git/info/exclude` 过期条目已删除。
  - 无 cache/dist/build/*.log（__pycache__ 为运行时生成，已 gitignore）。
  - `vjvision/__init__.py` 版本 0.1.0-beta；i18n app.title 含 VJVisionFX；全量 py_compile 通过。

## Task 2: 实时节奏特征提取器
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 1
- **Description**:
  - 新增 `<FX>/vjvision/fx_features.py`：有状态类 `RhythmFeatures`。
  - 输入：当前帧 `bins`(np.ndarray 0..1, 可空/None)、`peak`(float)、`dt`(秒)。
  - 输出：`bass`/`mid`/`treble`/`energy`（0..1 平滑能量）、`beat`（onset 置 1 后按
    `exp(-dt/tau)` 衰减的包络 0..1）、`onset`(本帧 bool)。
  - onset 判定：低频能量滑动基线（EMA 均值/方差），低频通量 > 基线 + k*标准差 且
    距上次触发 > ~0.12s；空/全零 bins 输出平滑空闲值、不触发。纯 numpy。
  - tmp 自测脚本（FX 目录内，用完删）：静音序列 beat<0.05 且无 onset；每 0.5s 低频
    尖峰序列在尖峰帧 onset、beat 峰值 ≥0.5 后衰减回 <0.1。
- **Acceptance Criteria Addressed**: AC-2
- **Test Requirements**:
  - `rule` TR-2.1：静音/空 bins 下所有特征 finite、beat < 0.05、onset 恒 False。
  - `rule` TR-2.2：模拟鼓点序列每个尖峰帧 onset=True（±1 帧），beat 峰值 ≥0.5 并衰减
    回 <0.1；非尖峰帧不触发。
  - `rule` TR-2.3：py_compile 通过，无新第三方依赖。

## Task 3: FX 纹理框架与渲染层接入
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 2
- **Description**:
  - 新增 `<FX>/vjvision/fx_textures.py`：
    - 注册表 `FX_TEXTURES = {key: (label_zh, label_en, render_fn)}`。
    - 统一签名 `render(colors, w, h, t, feat, state, gpu) -> (small_surface, state)`；
      state 为跨帧持久 dict（None 时初始化）。
    - 公共工具：低分辨率网格（GPU 320px / 软件 160px）、numpy→pygame 小图、
    - 调色板映射（**内置默认调色板**，colors 入参可被叠层主色覆盖）、统一暗化(×0.55)+
      中心晕影；空/零 feat 时待机轻柔运动。
    - 软件路径返回 smoothscale 全屏图；GPU 路径返回小图由调用方做 Texture。
  - `visualizer.py` 接入背景模式 `bg_mode == "fx"`：每帧 `feat = rhythm.update(...)`；
    `frame_no % 3` 调纹理渲染并缓存；GPU Texture 上采样 / 软件 blit；纹理 key 取自
    `SETTINGS.visual.fx_texture`；settings 消息更新 key 并重置纹理 state。
  - `config.py`：`bg_mode` 默认改 `"fx"`；新增 `fx_texture: str = "pulse"`。
  - 保留 flow/blur 分支代码（fx 为默认；blur 在两层化后无封面，UI 不再提供）。
- **Acceptance Criteria Addressed**: AC-6, AC-7
- **Test Requirements**:
  - `rule` TR-3.1：fx 模式静音/空 bins 连续 ≥300 帧不抛异常、输出 finite 且非全黑、
    帧间有缓慢变化（三纹理各跑）。
  - `rule` TR-3.2：320px 网格单次重绘 ≤ 约 5ms（与 `_make_flowing_bg` 同档）。
  - `rule` TR-3.3：bg_mode="fx" 时 GPU/软件两路径均产出背景；flow 分支代码仍在。
  - `rubric` TR-3.4：框架可扩展性（新增纹理仅需注册 render_fn）；1-5；阈值 >=4。

## Task 4: 实现三套首版纹理（pulse / ripple / particles）
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 3
- **Description**:
  - **pulse 律动波场**：以现波场为基底，振幅/空间频率受 beat/energy 调制，beat 触发
    瞬时提升并随包络回落；叠加焦点径向推挤项；保留慢漂移层保证静音不冻结。
  - **ripple 同心涟漪**：state 维护活跃环列表；onset 按压入环（bass=大慢环、
    treble=小快细环），每帧半径增长、亮度指数衰减、出屏移除；小图上加性环带亮场。
  - **particles 粒子迸发**：onset 按能量生成 N 个粒子（焦点、随机角度、初速与 bass
    正相关），每帧积分（阻尼+漂移），画布逐帧衰减形成余辉拖尾；粒子数封顶。
  - 三套均：默认调色板（可被叠层主色覆盖）、统一暗化/晕影、空 feat 待机、GPU/软件均可。
  - label zh/en：律动波场/Pulse field；同心涟漪/Ripples；粒子迸发/Particles。
- **Acceptance Criteria Addressed**: AC-3, AC-5, AC-7
- **Test Requirements**:
  - `rule` TR-4.1：FX_TEXTURES 含 pulse/ripple/particles，均有 zh/en label 与可调用 render_fn。
  - `rule` TR-4.2：模拟鼓点帧序列驱动三纹理各 ≥200 帧不抛异常、状态正确更新
    （ripple 环数 onset 后增再归零；particles 同理；pulse beat 项随包络变化）。
  - `rule` TR-4.3：节拍帧 vs 静音帧输出图均像素绝对差 / 静音相邻帧差 ≥ 2（三纹理各自）。
  - `rubric` TR-4.4：节奏视觉同步度（锚点见 AC-5）；阈值 >=4；客观帧差 + 用户人工。
  - `rubric` TR-4.5：三套纹理风格区分度；阈值 >=4；用户人工 + 帧图对比。

## Task 5: 画面两层化渲染改造（去前景，叠层全程顶层）
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 4
- **Description**:
  - `visualizer.py` 渲染循环在 fx 模式下**只**绘制：① 节奏背景纹理；② 顶层叠层。
  - 移除/跳过 fx 模式下的：频谱 bar/wave/mirror 绘制、旋转封面渲染、歌名/专辑文字、
    PEAK/电平以外的前景元素；移除待机判定（`state.title==""` 分支）与两段待机淡入淡出
    （STANDBY_FADE）；blur 背景不再调用（无封面）。
  - 叠层绘制改为**全程无条件**：素材加载后每帧绘制在最上层——居中、最大边 ≤40% 屏、
    不放大、保留 alpha、无淡入淡出；无素材时不绘制。
  - 叠层资源由 Task 6 的加载器提供（本任务先接静态图路径，动态接口预留）。
  - 纹理配色：无叠层素材用内置默认调色板；有素材时从素材提取主色（metadata 工具）
    生成调色板，加载/更换素材时更新 colors 并重置纹理 state。
- **Acceptance Criteria Addressed**: AC-4, FR-6, FR-8, FR-12
- **Test Requirements**:
  - `rule` TR-5.1：fx 模式渲染循环代码路径不含频谱/封面/文字/待机淡入淡出调用；
    叠层 blit 每帧无条件执行（有素材时）。证据：代码核查。
  - `rule` TR-5.2：启动后 visualizer.log 无封面/待机相关错误；画面无前景元素
    （用户人工）。
  - `rule` TR-5.3：设置叠层素材后纹理调色板变为素材主色（代码路径 + 日志）。

## Task 6: 动态/alpha 叠层素材解码与循环播放
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 5
- **Description**:
  - 新增 `<FX>/vjvision/fx_overlay.py`（或并入现有模块）：叠层素材加载器。
  - 接受静态图（PNG/JPG/BMP/WEBP）：pygame 直接加载 RGBA。
  - 动图（动态 WebP/APNG/GIF）与带 alpha 视频（WebM 等）：用现有 ffmpeg
    （`_find_ffmpeg` 路径逻辑）逐帧解码为 RGBA 帧序列（`-c:v png`/rawvideo 管道），
    读取素材帧率；按帧率循环播放；CREATE_NO_WINDOW。
  - 限制：总帧数上限（如 ≤ 600 帧）与帧分辨率上限（长边 ≤ 1024），超限时降采样/
    抽帧；内存缓存帧序列（pygame Surface 列表）。
  - 回退：无 ffmpeg → 静态图直接用；动图/视频解码失败 → 尝试首帧，再失败 → 不绘制
    并日志告警；绝不崩溃。
  - visualizer 端：每帧按经过时间取当前帧（循环），交由 Task 5 的顶层 blit 绘制；
    素材路径经 settings 消息下发，加载在可视化子进程内完成（含加载日志）。
  - debug_ui 文件对话框扩展名放开 .png/.jpg/.jpeg/.bmp/.webp/.apng/.gif/.webm/.mov/.mp4。
- **Acceptance Criteria Addressed**: AC-8, FR-9, FR-10
- **Test Requirements**:
  - `rule` TR-6.1：静态图加载返回单帧；构造/真实动态素材解码返回多帧+帧率，循环取帧
    正确（时间超过周期后回绕）。证据：tmp 脚本断言（ffmpeg 存在时）。
  - `rule` TR-6.2：无 ffmpeg/坏文件路径不抛异常，回退首帧/None 并有日志。证据：monkeypatch/坏文件测试。
  - `rule` TR-6.3：帧数/分辨率上限生效（超限不爆内存）。证据：测试断言。

## Task 7: UI 精简、纹理/叠层控制、i18n、prefs 与识别移除
- **Status**: `pending`
- **Priority**: `high`
- **Depends On**: Task 6
- **Description**:
  - `debug_ui.py`：**移除**分析/曲库/索引相关面板与按钮（分析队列、强制重新分析、
    曲库状态、音乐目录、数据库面板等）；保留输入设备（Host API/设备/刷新）、电平表
    （进度条+PEAK）、可视化显示区。
  - 可视化显示区：背景模式默认 fx（保留下拉但 blur 移除/仅 fx+flow 或直接 fx）；
    新增"纹理/Texture"下拉（选项由 FX_TEXTURES zh/en label 生成，fx 模式启用）；
    叠层选择按钮改用新文件过滤器（含动态素材）；保留 GPU/演示/语言/最小化控制。
  - 切换纹理 → `{"type":"settings","fx_texture": key}`；叠层选择 → 下发素材路径消息；
    均实时生效。
  - `config.py`/prefs：fx_texture、叠层路径（复用 standby 配置位）持久化，重启恢复。
  - **识别移除**：`matcher.py`/`main.py` 不再启动 dejavu、不连指纹 DB、不索引、
    不发识别结果；matcher 仅保留采集启动/设备切换/频谱+电平计算/对可视化消息管线；
    相关命令与代码可保留但不被调用（保证零识别行为、零 DB 依赖）。
  - `i18n.py`：新增 zh/en key（fx 模式名、纹理下拉标签、三套纹理名、叠层相关文案、
    移除面板的残留引用清理），zh/en 键数一致。
- **Acceptance Criteria Addressed**: AC-3, AC-9, NFR-5
- **Test Requirements**:
  - `rule` TR-7.1：i18n zh/en 键数一致、无缺键（parity 脚本 PASS）。
  - `rule` TR-7.2：debug_ui 无分析/曲库/索引面板；纹理下拉存在且 fx 模式可切换；
    叠层文件过滤器含动态扩展名。证据：代码核查。
  - `rule` TR-7.3：matcher/main 启动路径无 dejavu/FingerprintDB/MySQL/SQLite 调用；
    运行日志无识别/DB/索引行。证据：代码核查 + 启动日志。
  - `rule` TR-7.4：prefs.json 写入 fx_texture 与叠层路径，重启恢复。证据：prefs 文件。

## Task 8: 端到端验证、日志干净、清理与提交
- **Status**: `pending`
- **Priority**: `medium`
- **Depends On**: Task 7
- **Description**:
  - 全量 py_compile；启动 FX（`python main.py`），fx 模式三套纹理 + 静态/动态叠层
    人工验证；确认 visualizer.log FPS≥55、无 ERROR/traceback、无 dejavu/DB 行。
  - 跑特征/纹理/帧差/叠层自测脚本收集证据后删除所有 tmp_* 脚本。
  - FX 仓库提交全部改动（不 push、不打包、不发 Release）。
  - 交付说明：给用户移动目录命令（`Move-Item VJprg\VJVisionFX trae_projects\VJVisionFX`）；
    交用户人工做视觉主观确认（AC-5/AC-8 人工部分）。
  - 独立 Review pass（对照 AC-1..AC-9）。
- **Acceptance Criteria Addressed**: AC-1..AC-9（汇总证据）
- **Test Requirements**:
  - `rule` TR-8.1：visualizer.log 无新增错误、FPS 达标、无识别/DB 行。
  - `rule` TR-8.2：工作区无 tmp_* 残留；FX 仓库 git status 干净（改动已提交）。
  - `rubric` TR-8.3：整体交付完成度（AC 全覆盖、证据齐备）；阈值 >=4；独立评审。
