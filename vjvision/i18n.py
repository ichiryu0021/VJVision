"""Internationalisation (i18n) for VJVision.

Provides a tiny translation layer: a ``TRANSLATIONS`` dict keyed by
language code (``"zh"`` / ``"en"``) and a ``t(key)`` helper that returns
the string for the currently active language.

The active language is owned by :data:`vjvision.config.SETTINGS.language`
so it is persisted across runs.  Callers should not cache translated
strings — always call ``t(...)`` at render time so a language switch
takes effect immediately.
"""
from __future__ import annotations

from . import config as cfg

# Language code -> { key -> translated string }
TRANSLATIONS: dict[str, dict[str, str]] = {
    "zh": {
        # --- window / general ---
        "app.title": "VJVision 控制台",
        "lang.zh": "中文",
        "lang.en": "English",

        # --- audio device frame ---
        "dev.title": "音频设备",
        "dev.refresh": "刷新",
        "dev.input_level": "输入电平：",
        "dev.no_device": "无输入设备",
        "dev.peak": "PEAK",           # always "PEAK" per spec
        "dev.no_signal": "无信号 — 请检查设备/调音台",
        "dev.clip": "削波！{dbfs:.0f} dBFS",
        "dev.capturing": "● 采集中  {dbfs:.0f} dBFS",
        "dev.signal_ok": "信号正常  {dbfs:.0f} dBFS（监听）",
        "dev.none": "（无）",
        "dev.no_devices": "（无设备）",

        # --- library prep frame ---
        "prep.title": "曲库准备",
        "prep.add_folder": "+ 添加文件夹",
        "prep.add_files": "+ 添加文件",
        "prep.clear": "清空",
        "prep.queue_count": "队列：{n} 个文件",
        "prep.analyze": "分析队列",
        "prep.cancel": "✖ 取消",
        "prep.force_reindex": "⚠ 强制重新分析",
        "prep.refresh_status": "刷新状态",
        "prep.unknown": "曲库：未知",
        "prep.no_audio_in_folder": "[该文件夹中没有音频文件：{path}]",
        "prep.folder_added": "+ 文件夹：{path} → 已加入队列 {n} 个文件",
        "prep.force_reindex_cancelled": "已取消强制重新分析。",
        "prep.force_reindex_start": "⚠ 强制重新分析：正在清空全部数据并重新分析 {path}…",
        "prep.queue_empty": "[队列为空] 请先选择音频文件。",
        "prep.cancel_sent": "[取消] 已向索引器发送取消信号…",
        "prep.already_running": "已有索引任务在运行中，忽略重复的分析请求。",
        "prep.analyzing": "分析中：{done}/{total} — {info}",
        "prep.prepared": "曲库：已准备 {n} 首",
        "prep.library_analyzed": "曲库：已分析 {n} 首",
        "prep.pending": "，待处理 {pending} 首（共 {total} 首）",
        "prep.prep_failed": "准备失败：{error}",
        "prep.files_prepared": "已准备 {new}/{total} 个文件",

        # --- spectrum style ---
        "style.title": "频谱样式",
        "style.bar": "柱状",
        "style.wave": "波形",
        "style.mirror": "镜像",

        # --- animation ---
        "anim.rotation": "旋转速度（圈/秒）：",
        "anim.range": "（范围：0.05–2.0）",
        "anim.beat": "随节拍旋转",

        # --- confidence thresholds (advanced) ---
        "conf.title": "⚠️ 识别置信度调整",
        "conf.warning": "注意：该选项会影响识别精确度，在正式表演前请先测试。越低识别速度越快，越高识别精确度越高。",
        "conf.first": "第一首置信度：",
        "conf.first_hint": "认出第一首歌的门槛（默认 0.25 · 范围 0.05–0.95）",
        "conf.tentative": "脉动触发置信度：",
        "conf.tentative_hint": "切歌时先进入脉动预览的门槛（默认 0.06 · 范围 0.05–0.95）",
        "conf.switch": "切歌确认置信度：",
        "conf.switch_hint": "从脉动真正切到下一首的门槛（默认 0.30 · 范围 0.05–0.95）",

        # --- display ---
        "display.title": "可视化显示",
        "display.standby": "待机画面/LOGO图片：",
        "display.clear": "清除",
        "display.browse": "选择…",
        "display.bg": "背景：",
        "display.bg_flow": "流光 (flow)",
        "display.bg_blur": "封面模糊 (blur)",
        "display.font": "字体：",
        "display.font_auto": "自动（优选中文字体）",
        "display.font_cjk": " [中文字体]",
        "display.font_sample": "示例：中文 / 日本語 / test  123 ABC",
        "display.font_missing": "  [有缺字！]",
        "display.viz_restart": "🔄 重启可视化窗口",
        "display.viz_reset": "🔄 重置可视化窗口",
        "display.fullscreen_hint": "点击窗口后按 F 或 F11 全屏，Esc 退出。",
        "display.gpu_accel": "GPU 硬件加速（需重启可视化窗口）",
        "display.gpu_on": "[GPU] 硬件加速已开启，重启可视化窗口生效",
        "display.gpu_off": "[GPU] 硬件加速已关闭，重启可视化窗口生效",
        "display.demo_mode": "演示模式（无音频时旋转封面测试渲染）",
        "display.demo_on": "[演示] 已开启：随机封面旋转测试",
        "display.demo_off": "[演示] 已关闭：返回待机画面",
        "display.viz_unknown": "可视化：未知",
        "display.viz_running": "● 运行中",
        "display.viz_stopped": "● 已停止",
        "display.viz_mgr_unavailable": "[可视化] 可视化管理器不可用",
        "display.viz_restarting": "🔄 正在重启可视化窗口…",
        "display.viz_reset_done": "🔄 可视化窗口已重置",

        # --- capture ---
        "cap.start": "开始采集",
        "cap.stop": "停止采集",
        "cap.stopped": "采集：已停止",
        "cap.running": "采集：运行中",
        "cap.monitoring": "采集：已停止（监听输入中）",
        "cap.no_audio": "音频初始化失败：未检测到可用的音频设备或驱动（无声卡/驱动未安装/音频服务已禁用）。可视化等其他功能不受影响；连接声卡后请在控制台重新选择输入设备",
        "cap.init_failed": "采集初始化失败",
        "cap.start_failed": "采集启动失败",
        "cap.monitor_unavailable": "输入监听不可用（设备 {device}）：{error}。请选择其他输入设备，或连接声卡后重试",

        # --- track ---
        "track.title": "当前曲目",
        "track.none": "（无）",
        "track.confidence": "置信度：{conf:.2f}",
        "track.listening": "监听中",
        "track.capturing": "采集中",
        "track.standby": "待机",

        # --- log ---
        "log.title": "日志",

        # --- viz status (rendered in the visualizer window) ---
        "viz.capture_started": "Capture started",
        "viz.matching": "Matching…",
        "viz.no_match": "No match",
        "viz.listening": "Listening…",
        "viz.mixing": "Mixing…",
        "viz.pending": "Pending ({cur}/{need})",
        "viz.exited": "⚠ 可视化进程已退出 — 请点击『🔄 重置可视化窗口』按钮",

        # --- dialogs ---
        "dlg.force_reindex_title": "强制重新分析",
        "dlg.force_reindex_msg": (
            "这将删除所有已存指纹（MySQL + SQLite），\n"
            "并重新分析以下文件夹中的每一首歌：\n  {path}\n\n"
            "当识别置信度偏低、或歌曲曾用错误参数分析时，\n"
            "才使用此功能。\n\n确定继续？"
        ),
        "dlg.select_audio_title": "选择要分析的音频文件",
        "dlg.audio_files": "音频文件",
        "dlg.all_files": "所有文件",
        "dlg.select_image_title": "选择待机图片（推荐带透明通道的 PNG）",
        "dlg.images": "图片",
    },

    "en": {
        # --- window / general ---
        "app.title": "VJVision Console",
        "lang.zh": "中文",
        "lang.en": "English",

        # --- audio device frame ---
        "dev.title": "Audio Device",
        "dev.refresh": "Refresh",
        "dev.input_level": "Input Level:",
        "dev.no_device": "No input device",
        "dev.peak": "PEAK",
        "dev.no_signal": "No signal — check device / mixer",
        "dev.clip": "CLIP! {dbfs:.0f} dBFS",
        "dev.capturing": "● Capturing  {dbfs:.0f} dBFS",
        "dev.signal_ok": "Signal OK  {dbfs:.0f} dBFS (monitoring)",
        "dev.none": "(None)",
        "dev.no_devices": "(No device)",

        # --- library prep frame ---
        "prep.title": "Library Prep",
        "prep.add_folder": "+ Add Folder",
        "prep.add_files": "+ Add Files",
        "prep.clear": "Clear",
        "prep.queue_count": "Queue: {n} files",
        "prep.analyze": "Analyze Queue",
        "prep.cancel": "✖ Cancel",
        "prep.force_reindex": "⚠ Force Re-analyze",
        "prep.refresh_status": "Refresh Status",
        "prep.unknown": "Library: Unknown",
        "prep.no_audio_in_folder": "[No audio files in folder: {path}]",
        "prep.folder_added": "+ Folder: {path} → {n} files queued",
        "prep.force_reindex_cancelled": "Force re-analyze cancelled.",
        "prep.force_reindex_start": "⚠ Force re-analyze: clearing all data and re-analyzing {path}…",
        "prep.queue_empty": "[Queue empty] Please select audio files first.",
        "prep.cancel_sent": "[Cancel] Sent cancel signal to indexer…",
        "prep.already_running": "Indexing already in progress, ignoring duplicate request.",
        "prep.analyzing": "Analyzing: {done}/{total} — {info}",
        "prep.prepared": "Library: {n} tracks prepared",
        "prep.library_analyzed": "Library: {n} analyzed",
        "prep.pending": ", {pending} pending (total {total})",
        "prep.prep_failed": "Prep failed: {error}",
        "prep.files_prepared": "Prepared {new}/{total} files",

        # --- spectrum style ---
        "style.title": "Spectrum Style",
        "style.bar": "Bar",
        "style.wave": "Wave",
        "style.mirror": "Mirror",

        # --- animation ---
        "anim.rotation": "Rotation Speed (rev/s):",
        "anim.range": "(Range: 0.05–2.0)",
        "anim.beat": "Beat-reactive Rotation",

        # --- confidence thresholds (advanced) ---
        "conf.title": "⚠️ Recognition Confidence",
        "conf.warning": "Note: this affects recognition accuracy — test before a live performance. Lower = faster recognition, higher = more accurate.",
        "conf.first": "First-track confidence:",
        "conf.first_hint": "floor to accept the first song (default 0.25 · range 0.05–0.95)",
        "conf.tentative": "Pulse-trigger confidence:",
        "conf.tentative_hint": "floor that starts the pulsing preview on a new song (default 0.06 · range 0.05–0.95)",
        "conf.switch": "Switch-confirm confidence:",
        "conf.switch_hint": "floor to actually switch from pulse to the next song (default 0.30 · range 0.05–0.95)",

        # --- display ---
        "display.title": "Visualizer Display",
        "display.standby": "Standby / LOGO Image:",
        "display.clear": "Clear",
        "display.browse": "Browse…",
        "display.bg": "Background:",
        "display.bg_flow": "Flow",
        "display.bg_blur": "Cover Blur",
        "display.font": "Font:",
        "display.font_auto": "Auto (CJK preferred)",
        "display.font_cjk": " [CJK]",
        "display.font_sample": "Sample: 中文 / 日本語 / test  123 ABC",
        "display.font_missing": "  [missing glyphs!]",
        "display.viz_restart": "🔄 Restart Visualizer",
        "display.viz_reset": "🔄 Reset Visualizer",
        "display.fullscreen_hint": "Click window then press F or F11 for fullscreen, Esc to exit.",
        "display.gpu_accel": "GPU Hardware Acceleration (restart visualizer to apply)",
        "display.gpu_on": "[GPU] Hardware acceleration ON, restart visualizer to apply",
        "display.gpu_off": "[GPU] Hardware acceleration OFF, restart visualizer to apply",
        "display.demo_mode": "Demo mode (rotate a cover to test rendering without audio)",
        "display.demo_on": "[Demo] ON: rotating a random cover to test rendering",
        "display.demo_off": "[Demo] OFF: returning to standby screen",
        "display.viz_unknown": "Visualizer: Unknown",
        "display.viz_running": "● Running",
        "display.viz_stopped": "● Stopped",
        "display.viz_mgr_unavailable": "[Visualizer] Visualizer manager unavailable",
        "display.viz_restarting": "🔄 Restarting visualizer window…",
        "display.viz_reset_done": "🔄 Visualizer window reset",

        # --- capture ---
        "cap.start": "Start Capture",
        "cap.stop": "Stop Capture",
        "cap.stopped": "Capture: Stopped",
        "cap.running": "Capture: Running",
        "cap.monitoring": "Capture: Stopped (monitoring input)",
        "cap.no_audio": "Audio init failed: no usable audio device or driver detected (no sound card / driver missing / audio service disabled). Visualizer and other features are unaffected; reconnect a sound card and reselect the input device",
        "cap.init_failed": "Capture init failed",
        "cap.start_failed": "Capture start failed",
        "cap.monitor_unavailable": "Input monitor unavailable (device {device}): {error}. Pick another input device, or reconnect a sound card and retry",

        # --- track ---
        "track.title": "Current Track",
        "track.none": "(None)",
        "track.confidence": "Confidence: {conf:.2f}",
        "track.listening": "Listening",
        "track.capturing": "Capturing",
        "track.standby": "Standby",

        # --- log ---
        "log.title": "Log",

        # --- viz status (rendered in the visualizer window) ---
        "viz.capture_started": "Capture started",
        "viz.matching": "Matching…",
        "viz.no_match": "No match",
        "viz.listening": "Listening…",
        "viz.mixing": "Mixing…",
        "viz.pending": "Pending ({cur}/{need})",
        "viz.exited": "⚠ Visualizer process exited — click 『🔄 Reset Visualizer』",

        # --- dialogs ---
        "dlg.force_reindex_title": "Force Re-analyze",
        "dlg.force_reindex_msg": (
            "This will delete all stored fingerprints (MySQL + SQLite),\n"
            "and re-analyze every song in:\n  {path}\n\n"
            "Use only when recognition confidence is low,\n"
            "or songs were analyzed with wrong parameters.\n\nContinue?"
        ),
        "dlg.select_audio_title": "Select audio files to analyze",
        "dlg.audio_files": "Audio files",
        "dlg.all_files": "All files",
        "dlg.select_image_title": "Select standby image (PNG with alpha recommended)",
        "dlg.images": "Images",
    },
}


def t(key: str, **kwargs) -> str:
    """Return the translated string for ``key`` in the active language.

    Falls back to the key itself if no translation exists, so missing
    keys degrade gracefully instead of crashing.
    """
    lang = cfg.SETTINGS.language
    table = TRANSLATIONS.get(lang, TRANSLATIONS["zh"])
    text = table.get(key, key)
    if kwargs:
        try:
            text = text.format(**kwargs)
        except (KeyError, IndexError, ValueError):
            # Leave the unformatted string if placeholders don't match.
            pass
    return text
