---
name: "vjvision-viz-debug"
description: "VJVision 可视化窗口（pygame-ce/SDL2 GPU 渲染）改动与排查指南。当需要修改 visualizer.py/debug_ui.py 的渲染显示、排查可视化子进程问题（黑屏/马赛克/多窗口/帧率/全屏）、或验证 GPU 渲染效果时调用。"
---

# VJVision 可视化渲染调试指南

VJVision 的可视化输出是一个 **multiprocessing spawn 出的子进程**（`vjvision/visualizer.py` 的 `run(queue, display_index)`），控制台 `debug_ui.py`（customtkinter）通过队列向它发消息。本 skill 汇总该渲染链路的关键事实与已踩过的坑，避免重复试错。

## 1. 进程与日志（第一条要记住的事）

- 可视化是**独立子进程**，它的 stdout/stderr **不会**出现在 `python main.py` 的控制台里。
- 子进程用 `logging.FileHandler` 写日志到**项目根目录 `visualizer.log`**（mode="w"，每次启动覆盖）。排查时永远先看这个文件：
  - `GPU renderer initialised (accelerated+vsync) size=WxH` — GPU 路径是否启用
  - `Visualizer display=1 size=... fullscreen=... gpu=True/False` — 最终渲染模式
  - `FPS=.. frame_time=.. render=WxH` — 每 ~2s 一行，验证帧率与分辨率
  - 图片加载失败、纹理上传失败等 warning 也在这里
- 进程管理（Windows PowerShell）：
  - 启动：项目根目录 `python main.py`（后台运行）
  - 杀干净：`Get-Process python | Stop-Process -Force; Start-Sleep 2`（改代码后必须全杀再启，否则旧进程会在退出时用旧配置回写 prefs）
  - 正常应有 2-3 个 python 进程（main 控制台 + visualizer + 可能的 indexer）
- 用户配置：打包版在 **exe 旁 `data\prefs.json`**（便携，跟 data 文件夹走；2026-09-09 起，旧 `%APPDATA%\VJVision\prefs.json` 仅作一次性迁移源）；开发模式在项目根 `cache\prefs.json`。可视化子进程不继承内存 SETTINGS，靠 `load_prefs()` 读它。

## 2. GPU 渲染 API 事实（pygame-ce 2.5.8 / SDL 2.32 / Python 3.14，已实测）

```python
from pygame._sdl2 import video, Texture
window   = video.Window(title, size=(w,h), resizable=True)   # 或 fullscreen_desktop=True
renderer = video.Renderer(window, accelerated=1, vsync=1)
renderer.clear(); renderer.present()
renderer.draw_color = (r,g,b,a); renderer.fill_rect(rect); renderer.draw_line(p1,p2)
renderer.logical_size = (w,h)          # 高 DPI 下钉住逻辑坐标空间
tex = Texture.from_surface(renderer, surf)
tex.draw(srcrect=None, dstrect=rect, angle=deg, origin=(x,y))
tex.alpha = 0..255                      # 不是 alpha_mod
tex.blend_mode                          # RGBA surface 自动=1(BLEND)
```

易错点（全部真实踩过）：
- **Renderer / Texture 没有 `.destroy()`**；Window 有 `.destroy()`。重建时 `window.destroy()` 并把纹理引用置 None。
- 窗口尺寸用 `window.size`（属性），没有 `get_size()`。
- 旋转 `origin` 是**相对 dstrect 左上角**的坐标，用 `(rect.w//2, rect.h//2)`，不是屏幕绝对中心。
- **GPU 模式绝不能先调 `pygame.display.set_mode()`**（会报 "Surface already associated with window"）。`_reinit_display()` 里必须 GPU 路径优先、`set_mode` 仅作 GPU 失败后的软件回退——否则会建出两个窗口（"两个可视化窗口"bug 的根因）。
- GPU 模式不调 set_mode，`surface.convert_alpha()` 会抛 "No convert format has been set"。用容错 helper：先试 `convert_alpha()`，失败返回原 surface。
- **叠加类临时 surface（频谱等）必须 `pygame.Surface((w,h), pygame.SRCALPHA)`**；默认 surface 是不透明黑底，当纹理贴上去会盖住整块背景（表现为右半/整块黑屏）。
- 背景是小尺寸波场 surface 交给 GPU 放大：默认缩放是 **NEAREST → 马赛克格子+色带**。必须在 `pygame.init()` 前设 `os.environ["SDL_RENDER_SCALE_QUALITY"]="linear"`（SDL 建纹理时读取），即得双线性平滑放大、零 CPU 成本。GPU 背景源分辨率用 320 宽（软件路径 160 + smoothscale）。
- **原生分辨率全屏**：子进程要在 `pygame.init()` 前声明 DPI 感知 `ctypes.windll.shcore.SetProcessDpiAwareness(2)`，否则 SDL 全屏只看到 640x480；全屏用 `fullscreen_desktop=True`（桌面当前原生分辨率无边框，vsync 友好），不用 `fullscreen=True`。
- 占位圆：renderer 无 draw_circle，用 ~32 段 draw_line 近似。

## 3. 控制台 ↔ 可视化通信

- debug_ui 用 `self._send({"type": ..., ...})` 投递到 out 队列；visualizer 主循环 `queue.get_nowait()` 排空。
- 消息类型：`spectrum` / `track` / `status` / `settings` / `quit`。
- **改显示类设置**用 `{"type":"settings", key: value}`，visualizer 在 `mtype=="settings"` 分支逐项处理（style/rotation_speed/beat_reactive/bg_mode/fullscreen/font_name/standby_image/demo_mode）。
  - 运行时即时生效的（bg/fullscreen/font/demo）走队列消息。
  - 只在启动时读取的（如 gpu_acceleration 决定建哪种 renderer）：debug_ui 保存 prefs 后必须 `viz_mgr.restart()` 重启子进程。
- **CTkCheckBox/CTkButton 等控件构造时必须显式传 `text=t("key")`**；`_reg(widget, key)` 只在切换语言时重设文案，不传 text 会显示字面量类名（如 "CTkCheckBox"）。
- 新增文案：`vjvision/i18n.py` 的 zh/en 两个 dict 都要加，键数保持一致。

## 4. 验证手段（截图 / 按键 / ground truth）

- **GPU 内容截图**：`PrintWindow` 对 GPU 表面返回全黑；要用 `System.Drawing.Graphics.CopyFromScreen` 按窗口 rect 抓屏（先 SetForegroundWindow + ShowWindow 9）。
- **给 SDL 窗口发按键**：SendMessage 进不了 SDL 事件队列。用 `PostMessage(hwnd, WM_KEYDOWN=0x0100, vk, lParam)` + WM_KEYUP（F11 vk=0x7A scancode=0x57；D vk=0x44 scancode=0x20）。
- **点控制台（customtkinter）复选框**：SetCursorPos + mouse_event(LEFTDOWN/UP) 点复选框小方块；点整行文字也能切换。
- **GPU 渲染 ground truth**：怀疑是截图时机/合成问题时，用 `renderer.to_surface()` 回读当前帧再 `pygame.image.save()`——这是渲染缓冲的真实内容，可区分"渲染本身错"还是"抓屏/淡入时机错"。
- 找窗口：EnumWindows + GetWindowText 匹配标题（可视化="可视化输出"，控制台="控制台"）。

## 5. 边界与约定

- **不要动**识别/采集模块：`fingerprint.py`、`matcher.py`、`audio_capture.py`。只改 visualizer / config / debug_ui / i18n。
- 工作分支：`feat/gpu-acceleration`（可回滚）；不要主动提交，除非用户要求。
- 无音频输入时的渲染测试：控制台"可视化显示"区有**演示模式**复选框（随机旋转 cache/covers 里的封面）；GPU 硬件加速复选框也在同一区。热键 F/F11 全屏、Esc 退出全屏。
- 临时验证脚本/截图（tmp_*.py、tmp_*.png）用完即删，勿留在项目根目录。
