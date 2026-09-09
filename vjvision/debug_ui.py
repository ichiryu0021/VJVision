"""First-screen debug UI built with CustomTkinter.

Controls:
    - Audio device picker (+ refresh)
    - Music directory picker (+ "Index library" button)
    - Spectrum style selector (Bar / Radial / Wave)
    - Rotation speed slider
    - Beat-reactive toggle
    - Start / Stop capture buttons
    - Current track info (title / artist / album / confidence)
    - Live log
    - Language switcher (top-right)

All control changes are pushed onto ``out_queue`` as ``{'type': 'settings', ...}``
messages. The matcher process polls ``in_queue`` for status updates which we
display via ``root.after`` (Tkinter is not thread-safe).
"""
from __future__ import annotations

import logging
import queue as _queue
import tkinter as tk
from pathlib import Path
from tkinter import filedialog
from typing import Callable, Optional

import customtkinter as ctk

from . import __version__
from .i18n import t

log = logging.getLogger(__name__)

# (value, key) — value is the engine id, key is the i18n key for the label
STYLE_OPTIONS = [("bar", "style.bar"), ("wave", "style.wave"), ("mirror", "style.mirror")]

# Language picker: display label (self-describing, shown in both UIs) ->
# internal language code stored in SETTINGS.language.
LANG_DISPLAY = {"zh": "中文(Chinese)", "en": "English(英语)"}
LANG_CODE = {label: code for code, label in LANG_DISPLAY.items()}


class DebugUI:
    """CustomTkinter main window."""

    def __init__(
        self,
        out_queue: "_queue.Queue",
        in_queue: "_queue.Queue",
        viz_mgr=None,
    ) -> None:
        self.out = out_queue
        self.input = in_queue
        self.viz_mgr = viz_mgr

        # Registry of (widget, i18n_key) whose text must update on language
        # switch.  Dynamic widgets (queue count, track title, ...) are
        # handled separately.
        self._i18n_widgets: list[tuple[object, str]] = []

        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")
        self.root = ctk.CTk()
        self.root.title(f"{t('app.title')}  v{__version__}")
        # Clicking the window X runs the same shutdown path as the quit
        # button: notify the matcher, then destroy the window so main.py's
        # finally block runs (stops the visualizer child process, etc.).
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        screen_h = self.root.winfo_screenheight()
        # Wide two-column panel sized to fit content without excess
        # blank space at the bottom.
        win_h = min(screen_h - 80, 660)
        self.root.geometry(f"1240x{win_h}+40+20")
        self.root.minsize(1000, 560)

        # === Bottom capture toggle bar (prominent, fixed position) ===
        # Single start/stop toggle, packed side="bottom" BEFORE the
        # scroll frame so it always anchors the bottom edge of the
        # window: green "开始采集/Start Capture" when idle, red
        # "停止采集/Stop Capture" while a recognition capture is running.
        self._capturing = False
        self.cap_bar = ctk.CTkFrame(self.root, fg_color="transparent")
        self.cap_bar.pack(side="bottom", fill="x", padx=10, pady=(2, 10))
        self.capture_btn = ctk.CTkButton(
            self.cap_bar, text=t("cap.start"), height=46,
            font=ctk.CTkFont(size=18, weight="bold"),
            fg_color="#2f9e44", hover_color="#2b8a3e",
            command=self._on_capture_toggle,
        )
        self.capture_btn.pack(fill="x")

        self.scroll = ctk.CTkScrollableFrame(
            self.root,
            fg_color="transparent",
            border_width=0,
            label_text="",
        )
        self.scroll.pack(fill="both", expand=True)

        self._build_widgets()
        self._refresh_devices()

        self._on_rotation_change(self.rotation_var.get())
        self._on_style_change()
        self._on_beat_change()
        # Push the (possibly prefs-restored) confidence floors so the
        # matcher starts from exactly what the boxes show.
        self._on_conf_first_change()
        self._on_conf_tentative_change()
        self._on_conf_switch_change()

        self.root.after(100, self._poll_in)

        self._configure_after_id: Optional[str] = None
        self.root.after(50, self._update_scrollbar_visibility)
        self.root.after(150, self._fit_window_to_content)
        self.root.bind("<Configure>", self._on_root_configure)

    # -- helpers ----------------------------------------------------------
    def _reg(self, widget, key: str) -> object:
        """Register a widget for i18n re-rendering and return it."""
        self._i18n_widgets.append((widget, key))
        return widget

    def _apply_lang(self) -> None:
        """Re-render every registered widget's text in the current language."""
        self.root.title(f"{t('app.title')}  v{__version__}")
        for widget, key in self._i18n_widgets:
            try:
                widget.configure(text=t(key))
            except Exception:
                pass
        # Refresh dynamic / stateful widgets.
        self._refresh_queue_display()
        self._update_viz_status_label()
        # Style radio button labels.
        for rb, (_, key) in zip(self._style_rbs, STYLE_OPTIONS):
            rb.configure(text=t(key))
        # Background mode menu values.
        self.bg_mode_menu.configure(values=[t("display.bg_flow"), t("display.bg_blur")])
        # Font menu needs a full rebuild because display names include CJK tag.
        self._rebuild_font_menu()
        # Track state label respects current capture state.
        self._refresh_track_state_label()

    def _send(self, msg: dict) -> None:
        try:
            self.out.put_nowait(msg)
        except _queue.Full:
            log.warning("out_queue full - dropping %s", msg.get("type"))

    def _on_root_configure(self, _evt: object) -> None:
        if getattr(self, "_configure_after_id", None):
            self.root.after_cancel(self._configure_after_id)
        self._configure_after_id = self.root.after(
            60, self._update_scrollbar_visibility
        )

    def _update_scrollbar_visibility(self) -> None:
        try:
            sb = self.scroll._scrollbar
            canvas = self.scroll._parent_canvas
            self.scroll.update_idletasks()
            region = canvas.cget("scrollregion")
            view_h = self.scroll.winfo_height()
            if not region:
                return
            parts = [int(x) for x in region.split()]
            content_h = parts[3] - parts[1] if len(parts) == 4 else 0
        except Exception:
            return
        if content_h <= view_h + 2:
            try:
                sb.grid_remove()
            except Exception:
                pass
        else:
            try:
                sb.grid(
                    column=1, row=1, sticky="nesw",
                    padx=(0, 1), pady=6, rowspan=1, columnspan=1,
                )
            except Exception:
                pass

    def _fit_window_to_content(self) -> None:
        """Grow the default window so every panel is visible without
        scrolling (capped to the screen work area). Called once after
        the UI has been built."""
        try:
            self.scroll.update_idletasks()
            canvas = self.scroll._parent_canvas
            canvas.configure(scrollregion=canvas.bbox("all"))
            bbox = canvas.bbox("all")
            if not bbox:
                return
            content_h = bbox[3] - bbox[1]
            content_w = bbox[2] - bbox[0]
            screen_h = self.root.winfo_screenheight()
            screen_w = self.root.winfo_screenwidth()
            # Allowance for window chrome / outer padding / scrollbar gutter.
            win_h = int(min(screen_h - 60, content_h + 56))
            win_w = int(min(screen_w - 40, max(1240, content_w + 60)))
            self.root.geometry(f"{win_w}x{win_h}+40+20")
        except Exception:
            return

    def _build_widgets(self) -> None:
        pad = {"padx": 8, "pady": 6}

        # === Top bar: language selector (right-aligned) =================
        top_bar = ctk.CTkFrame(self.scroll, fg_color="transparent")
        top_bar.pack(fill="x", **pad)
        ctk.CTkLabel(top_bar, text=f"v{__version__}", text_color="gray").pack(
            side="left")
        from .config import SETTINGS, save_prefs
        self.lang_var = ctk.StringVar(
            value=LANG_DISPLAY.get(SETTINGS.language, LANG_DISPLAY["zh"]))
        self.lang_menu = ctk.CTkOptionMenu(
            top_bar, variable=self.lang_var,
            values=list(LANG_DISPLAY.values()),
            command=self._on_lang_change, width=150,
        )
        self.lang_menu.pack(side="right")

        # === Two-column body ============================================
        # Left column: library + capture; right column: audio + visual.
        col_row = ctk.CTkFrame(self.scroll, fg_color="transparent")
        col_row.pack(fill="both", expand=True, **pad)
        self.left_col = ctk.CTkFrame(col_row, fg_color="transparent")
        self.left_col.pack(side="left", fill="both", expand=True, padx=(0, 6))
        self.right_col = ctk.CTkFrame(col_row, fg_color="transparent")
        self.right_col.pack(side="left", fill="both", expand=True, padx=(6, 0))

        # === Audio device ==============================================
        self.dev_frame = ctk.CTkFrame(self.left_col)
        self.dev_frame.pack(fill="x", **pad)
        self._reg(ctk.CTkLabel(self.dev_frame, text=t("dev.title")), "dev.title").pack(
            anchor="w", padx=8, pady=(8, 0))

        api_row = ctk.CTkFrame(self.dev_frame, fg_color="transparent")
        api_row.pack(fill="x", padx=8, pady=(4, 4))
        self.api_var = ctk.StringVar(value="")
        self.api_menu = ctk.CTkOptionMenu(
            api_row, variable=self.api_var, values=[t("dev.none")],
            command=self._on_api_change, width=120,
        )
        self.api_menu.pack(side="left", padx=(0, 8))
        self.device_var = ctk.StringVar(value="")
        self.device_menu = ctk.CTkOptionMenu(
            api_row, variable=self.device_var, values=[t("dev.no_devices")],
            command=self._on_device_change, width=180,
        )
        self.device_menu.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.refresh_btn = self._reg(
            ctk.CTkButton(api_row, text=t("dev.refresh"), width=80,
                          command=self._refresh_devices),
            "dev.refresh",
        )
        self.refresh_btn.pack(side="left")

        meter_row = ctk.CTkFrame(self.dev_frame, fg_color="transparent")
        meter_row.pack(fill="x", padx=8, pady=(0, 8))
        self._reg(ctk.CTkLabel(meter_row, text=t("dev.input_level")), "dev.input_level").pack(side="left")
        self.level_bar = ctk.CTkProgressBar(meter_row, width=300)
        self.level_bar.set(0.0)
        self.level_bar.pack(side="left", padx=(8, 8), fill="x", expand=True)
        self.level_status = ctk.CTkLabel(meter_row, text=t("dev.no_device"),
                                         text_color="gray", width=150,
                                         anchor="w")
        self.level_status.pack(side="left", padx=(0, 8))
        self.clip_label = self._reg(
            ctk.CTkLabel(meter_row, text=t("dev.peak"), text_color="gray", width=40),
            "dev.peak",
        )
        self.clip_label.pack(side="left")

        # === Library preparation =======================================
        self._queue: list[str] = []
        self.prep_frame = ctk.CTkFrame(self.left_col)
        self.prep_frame.pack(fill="x", **pad)
        self._reg(ctk.CTkLabel(self.prep_frame, text=t("prep.title")), "prep.title").pack(
            anchor="w", padx=8, pady=(8, 0))

        add_row = ctk.CTkFrame(self.prep_frame, fg_color="transparent")
        add_row.pack(fill="x", padx=8, pady=(6, 0))
        self.add_folder_btn = self._reg(
            ctk.CTkButton(add_row, text=t("prep.add_folder"), width=120,
                          command=self._add_folder),
            "prep.add_folder",
        )
        self.add_folder_btn.pack(side="left")
        self.add_files_btn = self._reg(
            ctk.CTkButton(add_row, text=t("prep.add_files"), width=110,
                          command=self._pick_audio_files),
            "prep.add_files",
        )
        self.add_files_btn.pack(side="left", padx=4)
        self.clear_btn = self._reg(
            ctk.CTkButton(add_row, text=t("prep.clear"), width=60,
                          command=self._clear_queue),
            "prep.clear",
        )
        self.clear_btn.pack(side="left")
        self.queue_label = ctk.CTkLabel(add_row, text=t("prep.queue_count", n=0))
        self.queue_label.pack(side="left", padx=12)

        self.queue_list = ctk.CTkTextbox(self.prep_frame, height=80, state="disabled")
        self.queue_list.pack(fill="x", padx=8, pady=2)

        action_row = ctk.CTkFrame(self.prep_frame, fg_color="transparent")
        action_row.pack(fill="x", padx=8, pady=(0, 4))
        self.analyze_btn = self._reg(
            ctk.CTkButton(action_row, text=t("prep.analyze"), fg_color="green",
                          command=self._on_analyze_queue),
            "prep.analyze",
        )
        self.analyze_btn.pack(side="left")
        self.cancel_btn = self._reg(
            ctk.CTkButton(action_row, text=t("prep.cancel"), width=80,
                          fg_color="gray", hover_color="#555555",
                          state="disabled",
                          command=self._on_cancel_analyze),
            "prep.cancel",
        )
        self.cancel_btn.pack(side="left", padx=4)
        self.force_reindex_btn = self._reg(
            ctk.CTkButton(action_row, text=t("prep.force_reindex"), width=150,
                          fg_color="#B22222", hover_color="#8B0000",
                          command=self._on_force_reindex),
            "prep.force_reindex",
        )
        self.force_reindex_btn.pack(side="left", padx=4)
        self.refresh_status_btn = self._reg(
            ctk.CTkButton(action_row, text=t("prep.refresh_status"), width=120,
                          command=self._on_refresh_status),
            "prep.refresh_status",
        )
        self.refresh_status_btn.pack(side="left")

        self.index_progress = ctk.CTkProgressBar(self.prep_frame, height=18)
        self.index_progress.set(0.0)
        self.index_progress.pack(fill="x", padx=8, pady=(8, 0))
        self.prep_status = ctk.CTkLabel(self.prep_frame, text=t("prep.unknown"))
        self.prep_status.pack(anchor="w", padx=8, pady=(2, 8))

        # === Spectrum style ============================================
        self.style_frame = ctk.CTkFrame(self.right_col)
        self.style_frame.pack(fill="x", **pad)
        self._reg(ctk.CTkLabel(self.style_frame, text=t("style.title")), "style.title").pack(
            anchor="w", padx=8, pady=(8, 0))
        self.style_var = ctk.StringVar(value="bar")
        self._style_rbs = []
        for value, key in STYLE_OPTIONS:
            rb = ctk.CTkRadioButton(
                self.style_frame, text=t(key), variable=self.style_var, value=value,
                command=self._on_style_change,
            )
            rb.pack(side="left", padx=12, pady=8)
            self._style_rbs.append(rb)

        # === Rotation + beat ============================================
        self.anim_frame = ctk.CTkFrame(self.right_col)
        self.anim_frame.pack(fill="x", **pad)
        rot_row = ctk.CTkFrame(self.anim_frame, fg_color="transparent")
        rot_row.pack(fill="x", padx=8, pady=(8, 2))
        self._reg(ctk.CTkLabel(rot_row, text=t("anim.rotation")), "anim.rotation").pack(side="left")
        self.rotation_var = ctk.DoubleVar(value=0.25)
        self.rotation_entry = ctk.CTkEntry(
            rot_row, textvariable=self.rotation_var, width=70,
            justify="center",
        )
        self.rotation_entry.pack(side="left", padx=(6, 8))
        self._reg(ctk.CTkLabel(rot_row, text=t("anim.range"), text_color="gray"), "anim.range").pack(side="left")
        self.rotation_entry.bind("<Return>", lambda e: self._on_rotation_change(self.rotation_var.get()))
        self.rotation_entry.bind("<FocusOut>", lambda e: self._on_rotation_change(self.rotation_var.get()))
        self.beat_var = ctk.BooleanVar(value=False)
        self.beat_cb = self._reg(
            ctk.CTkCheckBox(self.anim_frame, text=t("anim.beat"),
                            variable=self.beat_var, command=self._on_beat_change),
            "anim.beat",
        )
        self.beat_cb.pack(anchor="w", padx=8, pady=(0, 8))

        # === Display mode ==============================================
        self.display_frame = ctk.CTkFrame(self.right_col)
        self.display_frame.pack(fill="x", **pad)
        self._reg(ctk.CTkLabel(self.display_frame, text=t("display.title")), "display.title").pack(
            anchor="w", padx=8, pady=(8, 0))

        standby_row = ctk.CTkFrame(self.display_frame, fg_color="transparent")
        standby_row.pack(fill="x", padx=8, pady=(0, 8))
        self._reg(ctk.CTkLabel(standby_row, text=t("display.standby")), "display.standby").pack(
            side="left", padx=(0, 6))
        self.standby_label = ctk.CTkLabel(
            standby_row, text=t("dev.none"), text_color="gray", anchor="w",
        )
        self.standby_label.pack(side="left", fill="x", expand=True)
        self.clear_standby_btn = self._reg(
            ctk.CTkButton(standby_row, text=t("display.clear"), width=60,
                          fg_color="#555555", hover_color="#777777",
                          command=self._on_clear_standby),
            "display.clear",
        )
        self.clear_standby_btn.pack(side="right", padx=(4, 0))
        self.browse_standby_btn = self._reg(
            ctk.CTkButton(standby_row, text=t("display.browse"), width=90,
                          command=self._on_choose_standby),
            "display.browse",
        )
        self.browse_standby_btn.pack(side="right")
        _si = SETTINGS.visual.standby_image or ""
        if _si:
            self.standby_label.configure(text=Path(_si).name)

        bg_row = ctk.CTkFrame(self.display_frame, fg_color="transparent")
        bg_row.pack(fill="x", padx=8, pady=(0, 8))
        self._reg(ctk.CTkLabel(bg_row, text=t("display.bg")), "display.bg").pack(side="left", padx=(0, 6))
        self.bg_mode_var = ctk.StringVar(
            value=t("display.bg_flow") if SETTINGS.visual.bg_mode == "flow" else t("display.bg_blur"))
        self.bg_mode_menu = ctk.CTkOptionMenu(
            bg_row, variable=self.bg_mode_var,
            values=[t("display.bg_flow"), t("display.bg_blur")],
            command=self._on_bg_mode_change, width=140,
        )
        self.bg_mode_menu.pack(side="left")

        font_row = ctk.CTkFrame(self.display_frame, fg_color="transparent")
        font_row.pack(fill="x", padx=8, pady=(0, 8))
        self._reg(ctk.CTkLabel(font_row, text=t("display.font")), "display.font").pack(side="left", padx=(0, 6))
        self._font_options = self._build_font_options()
        display_default = SETTINGS.visual.font_name or "auto"
        matching = [d for d, v in self._font_options
                    if v.lower() == display_default.lower()]
        self.font_var = ctk.StringVar(
            value=matching[0] if matching else self._font_options[0][0],
        )
        self.font_menu = ctk.CTkOptionMenu(
            font_row, variable=self.font_var,
            values=[d for d, _ in self._font_options],
            command=self._on_font_change, width=220,
        )
        self.font_menu.pack(side="left")

        viz_btn_row = ctk.CTkFrame(self.display_frame, fg_color="transparent")
        viz_btn_row.pack(fill="x", padx=8, pady=(0, 0))
        # Single button: soft reset (back to standby) when the visualizer
        # is alive, fresh spawn when the process exited / crashed. The
        # old separate "restart" button killed+respawned the process and
        # could flash-and-die due to a stale 'quit' left in the queue;
        # reset() picks the right path automatically.
        self.viz_reset_btn = self._reg(
            ctk.CTkButton(viz_btn_row, text=t("display.viz_reset"),
                          width=180, fg_color="#2b6cb0", hover_color="#3182ce",
                          command=self._on_viz_reset),
            "display.viz_reset",
        )
        self.viz_reset_btn.pack(side="left")
        self.viz_status_label = ctk.CTkLabel(viz_btn_row, text="", text_color="gray")
        self.viz_status_label.pack(side="right")
        self._update_viz_status_label()

        self.fullscreen_hint = self._reg(
            ctk.CTkLabel(self.display_frame, text=t("display.fullscreen_hint"),
                         text_color="gray", font=("", 12)),
            "display.fullscreen_hint",
        )
        self.fullscreen_hint.pack(anchor="w", padx=8, pady=(8, 8))

        # GPU acceleration toggle (requires visualizer restart to apply).
        from .config import SETTINGS
        self.gpu_var = ctk.BooleanVar(value=SETTINGS.visual.gpu_acceleration)
        self.gpu_checkbox = self._reg(
            ctk.CTkCheckBox(
                self.display_frame,
                text=t("display.gpu_accel"),
                variable=self.gpu_var,
                command=self._on_gpu_toggle,
            ),
            "display.gpu_accel",
        )
        self.gpu_checkbox.pack(anchor="w", padx=8, pady=(0, 4))

        # Demo mode: rotates a random cached cover so rendering can be
        # tested without audio input. Runtime-only state (not persisted);
        # a visualizer restart returns to standby, so the box resets too.
        self.demo_var = ctk.BooleanVar(value=False)
        self.demo_checkbox = self._reg(
            ctk.CTkCheckBox(
                self.display_frame,
                text=t("display.demo_mode"),
                variable=self.demo_var,
                command=self._on_demo_toggle,
            ),
            "display.demo_mode",
        )
        self.demo_checkbox.pack(anchor="w", padx=8, pady=(0, 4))

        self.root.after(3000, self._poll_viz_alive)

        # === Confidence thresholds (ADVANCED — red warning) ===========
        # Recognition-engine floors: pulse-trigger (脉动) and hard-confirm
        # for the first track / for switching.  Wrong values cause false
        # song jumps or no switching at all, hence the warning styling.
        from .config import SETTINGS
        self.conf_frame = ctk.CTkFrame(self.left_col)
        self.conf_frame.pack(fill="x", **pad)
        self._reg(
            ctk.CTkLabel(self.conf_frame, text=t("conf.title"),
                         text_color="#ff5555"),
            "conf.title",
        ).pack(anchor="w", padx=8, pady=(8, 0))
        self._reg(
            ctk.CTkLabel(self.conf_frame, text=t("conf.warning"),
                         text_color="#e8a33d", justify="left",
                         wraplength=540, font=("", 12)),
            "conf.warning",
        ).pack(anchor="w", padx=8, pady=(2, 4))

        self.conf_first_var = ctk.DoubleVar(
            value=SETTINGS.capture.first_track_min_confidence)
        self.conf_tentative_var = ctk.DoubleVar(
            value=SETTINGS.capture.tentative_min_confidence)
        self.conf_switch_var = ctk.DoubleVar(
            value=SETTINGS.capture.switch_min_confidence)

        # (label_key, hint_key, var, handler) — one number box per floor.
        self._conf_rows = [
            ("conf.first", "conf.first_hint", self.conf_first_var,
             self._on_conf_first_change),
            ("conf.tentative", "conf.tentative_hint", self.conf_tentative_var,
             self._on_conf_tentative_change),
            ("conf.switch", "conf.switch_hint", self.conf_switch_var,
             self._on_conf_switch_change),
        ]
        for row_idx, (label_key, hint_key, var, handler) in enumerate(self._conf_rows):
            row = ctk.CTkFrame(self.conf_frame, fg_color="transparent")
            row.pack(fill="x", padx=8,
                     pady=(2, 8 if row_idx == len(self._conf_rows) - 1 else 2))
            self._reg(
                ctk.CTkLabel(row, text=t(label_key), width=120, anchor="w"),
                label_key,
            ).pack(side="left")
            entry = ctk.CTkEntry(row, textvariable=var, width=70,
                                 justify="center")
            entry.pack(side="left", padx=(6, 8))
            self._reg(
                ctk.CTkLabel(row, text=t(hint_key), text_color="gray",
                             font=("", 12), anchor="w"),
                hint_key,
            ).pack(side="left")
            entry.bind("<Return>", lambda e, h=handler: h())
            entry.bind("<FocusOut>", lambda e, h=handler: h())

        # === Capture state (hidden holder) =============================
        # The start/stop controls live in the prominent bottom bar now
        # (self.capture_btn).  This label is deliberately NOT packed: it
        # only holds the current capture-state text ("采集：运行中" /
        # "监听输入中" / ...) which drives the track-state label refresh
        # and the toggle button colour via _refresh_track_state_label().
        self.capture_status = ctk.CTkLabel(self.left_col, text=t("cap.stopped"))

        # === Current track ============================================
        self.track_frame = ctk.CTkFrame(self.left_col)
        self.track_frame.pack(fill="both", expand=True, **pad)
        self._reg(ctk.CTkLabel(self.track_frame, text=t("track.title")), "track.title").pack(
            anchor="w", padx=8, pady=(8, 0))
        self.track_title = ctk.CTkLabel(self.track_frame, text=t("track.none"), font=("Arial", 16, "bold"))
        self.track_title.pack(anchor="w", padx=8)
        self.track_artist = ctk.CTkLabel(self.track_frame, text="")
        self.track_artist.pack(anchor="w", padx=8)
        self.track_conf = ctk.CTkLabel(self.track_frame, text="")
        self.track_conf.pack(anchor="w", padx=8)
        self.track_state = ctk.CTkLabel(self.track_frame, text=t("track.listening"), text_color="#63b3ed")
        self.track_state.pack(anchor="w", padx=8, pady=(0, 8))

        # === Log ======================================================
        self.log_frame = ctk.CTkFrame(self.right_col)
        self.log_frame.pack(fill="both", expand=True, **pad)
        self._reg(ctk.CTkLabel(self.log_frame, text=t("log.title")), "log.title").pack(
            anchor="w", padx=8, pady=(8, 0))
        self.log_text = ctk.CTkTextbox(self.log_frame, height=120, state="disabled")
        self.log_text.pack(fill="both", expand=True, padx=8, pady=(0, 8))

    # -- language ---------------------------------------------------------
    def _on_lang_change(self, display: str) -> None:
        from .config import SETTINGS, save_prefs
        lang = LANG_CODE.get(display, "zh")
        SETTINGS.language = lang
        save_prefs()
        self._apply_lang()

    # -- device picker ----------------------------------------------------
    def _refresh_devices(self) -> None:
        from .audio_capture import AudioCapture
        from .config import SETTINGS, save_prefs
        devices = AudioCapture.list_input_devices()
        self._devices = devices

        from collections import OrderedDict
        groups: dict[str, list[dict]] = OrderedDict()
        for d in devices:
            ha = d["host_api"] or "Unknown"
            groups.setdefault(ha, []).append(d)
        self._api_groups = groups

        api_labels = list(groups.keys()) if groups else [t("dev.no_devices")]
        self.api_menu.configure(values=api_labels)

        saved_index: int | None = None
        try:
            raw = SETTINGS.audio_device
            if isinstance(raw, int):
                saved_index = raw
        except Exception:
            pass

        saved_match = None
        usb_match = None
        first_match = None
        for d in devices:
            if saved_index is not None and d["index"] == saved_index:
                saved_match = d
            if "usb" in d["name"].lower() and usb_match is None:
                usb_match = d
            if first_match is None:
                first_match = d

        chosen = saved_match or usb_match or first_match
        if chosen is not None:
            chosen_api = chosen["host_api"] or "Unknown"
            self.api_var.set(chosen_api)
            self._populate_devices_for_api(chosen_api)
            label = f"[{chosen['index']}] {chosen['name']}"
            self.device_var.set(label)
            if SETTINGS.audio_device != chosen["index"]:
                SETTINGS.audio_device = chosen["index"]
                save_prefs()
        else:
            self.api_var.set(api_labels[0])
            self._populate_devices_for_api(api_labels[0])
        self._on_device_change(self.device_var.get())

    def _populate_devices_for_api(self, api_name: str) -> None:
        devs = self._api_groups.get(api_name, [])
        labels = [f"[{d['index']}] {d['name']}" for d in devs]
        if not labels:
            labels = [t("dev.no_devices")]
        self.device_menu.configure(values=labels)

    def _on_api_change(self, api_name: str) -> None:
        self._populate_devices_for_api(api_name)
        labels = self.device_menu.cget("values")
        if labels:
            self.device_var.set(labels[0])
            self._on_device_change(labels[0])

    def _on_device_change(self, label: str) -> None:
        from .config import SETTINGS, save_prefs

        if label in (t("dev.no_devices"), t("dev.none"), ""):
            return

        try:
            import re
            m = re.search(r"\[(\d+)\]", label)
            idx = int(m.group(1)) if m else None
        except (ValueError, IndexError):
            idx = None

        if idx is not None:
            if SETTINGS.audio_device != idx:
                SETTINGS.audio_device = idx
                save_prefs()
        self._send({"type": "device", "index": idx})

    # -- folder / file queue --------------------------------------------
    def _add_folder(self) -> None:
        from pathlib import Path
        from .config import SETTINGS
        initial = str(SETTINGS.music_dir)
        chosen = filedialog.askdirectory(initialdir=initial)
        if not chosen:
            return
        SETTINGS.music_dir = Path(chosen)
        self._send({"type": "music_dir", "path": str(SETTINGS.music_dir)})

        root = Path(chosen)
        exts = {".flac", ".wav", ".mp3", ".aiff", ".aif", ".ogg"}
        found = sorted(
            str(p) for p in root.rglob("*")
            if p.is_file() and p.suffix.lower() in exts
        )
        if not found:
            self._append_log(t("prep.no_audio_in_folder", path=chosen))
            return
        added = 0
        for p in found:
            if p not in self._queue:
                self._queue.append(p)
                added += 1
        self._refresh_queue_display()
        self._append_log(t("prep.folder_added", path=chosen, n=added))

    def _on_force_reindex(self) -> None:
        from .config import SETTINGS
        import tkinter.messagebox as _mb
        folder = str(SETTINGS.music_dir)
        if not _mb.askyesno(
            t("dlg.force_reindex_title"),
            t("dlg.force_reindex_msg", path=folder),
        ):
            self._append_log(t("prep.force_reindex_cancelled"))
            return
        self._send({"type": "force_reindex"})
        self.cancel_btn.configure(state="normal")
        self._append_log(t("prep.force_reindex_start", path=folder))

    def _pick_audio_files(self) -> None:
        from .config import SETTINGS
        files = filedialog.askopenfilenames(
            initialdir=str(SETTINGS.music_dir),
            title=t("dlg.select_audio_title"),
            filetypes=[
                (t("dlg.audio_files"), "*.flac *.wav *.mp3 *.aiff *.aif *.ogg"),
                (t("dlg.all_files"), "*.*"),
            ],
        )
        if files:
            for p in files:
                if p not in self._queue:
                    self._queue.append(p)
            self._refresh_queue_display()

    def _clear_queue(self) -> None:
        self._queue.clear()
        self._refresh_queue_display()

    def _refresh_queue_display(self) -> None:
        self.queue_label.configure(text=t("prep.queue_count", n=len(self._queue)))
        self.queue_list.configure(state="normal")
        self.queue_list.delete("1.0", "end")
        for p in self._queue:
            self.queue_list.insert("end", p + "\n")
        self.queue_list.configure(state="disabled")

    def _on_analyze_queue(self) -> None:
        if not self._queue:
            self._append_log(t("prep.queue_empty"))
            return
        self.analyze_btn.configure(state="disabled")
        all_paths = list(self._queue)
        BATCH = 500
        for i in range(0, len(all_paths), BATCH):
            self._send({
                "type": "prepare_files_batch",
                "paths": all_paths[i : i + BATCH],
            })
        self._send({
            "type": "prepare_files_done",
            "total": len(all_paths),
        })
        self.cancel_btn.configure(state="normal")

    def _on_cancel_analyze(self) -> None:
        self._send({"type": "cancel_indexing"})
        self.cancel_btn.configure(state="disabled")
        self._append_log(t("prep.cancel_sent"))

    def _on_refresh_status(self) -> None:
        self._send({"type": "library_status"})

    # -- visual settings -------------------------------------------------
    def _on_style_change(self) -> None:
        self._send({"type": "settings", "style": self.style_var.get()})

    def _on_rotation_change(self, value) -> None:
        try:
            v = round(float(value), 2)
        except (TypeError, ValueError):
            return
        if v < 0.05:
            v = 0.05
        elif v > 2.0:
            v = 2.0
        self._send({"type": "settings", "rotation_speed": v})

    def _on_beat_change(self) -> None:
        self._send({"type": "settings", "beat_reactive": bool(self.beat_var.get())})

    # -- confidence thresholds (advanced, red section) -------------------
    def _clamp_conf(self, var: ctk.DoubleVar) -> Optional[float]:
        """Parse + clamp a confidence entry; write the clamped value back."""
        try:
            v = round(float(var.get()), 2)
        except (TypeError, ValueError, tk.TclError):
            return None
        v = min(0.95, max(0.05, v))
        var.set(v)
        return v

    def _on_conf_first_change(self) -> None:
        v = self._clamp_conf(self.conf_first_var)
        if v is not None:
            self._send({"type": "settings", "first_track_min_confidence": v})

    def _on_conf_tentative_change(self) -> None:
        v = self._clamp_conf(self.conf_tentative_var)
        if v is not None:
            self._send({"type": "settings", "tentative_min_confidence": v})

    def _on_conf_switch_change(self) -> None:
        v = self._clamp_conf(self.conf_switch_var)
        if v is not None:
            self._send({"type": "settings", "switch_min_confidence": v})

    def _on_bg_mode_change(self, choice: str) -> None:
        raw = "blur" if choice == t("display.bg_blur") else "flow"
        self._send({"type": "settings", "bg_mode": raw})

    def _build_font_options(self) -> list[tuple[str, str]]:
        try:
            import pygame
            if not pygame.font.get_init():
                pygame.font.init()
            from .visualizer import _list_system_fonts
            raw = _list_system_fonts()
        except Exception:
            return [(t("display.font_auto"), "auto")]

        out: list[tuple[str, str]] = [(t("display.font_auto"), "auto")]
        for name, supports in raw:
            display = f"{name}{t('display.font_cjk')}" if supports else name
            out.append((display, name))
        return out

    def _rebuild_font_menu(self) -> None:
        """Rebuild the font picker after a language switch (CJK tag changes)."""
        old_raw = "auto"
        for display, name in self._font_options:
            if display == self.font_var.get():
                old_raw = name
                break
        self._font_options = self._build_font_options()
        self.font_menu.configure(values=[d for d, _ in self._font_options])
        matching = [d for d, v in self._font_options if v.lower() == old_raw.lower()]
        if matching:
            self.font_var.set(matching[0])
        else:
            self.font_var.set(self._font_options[0][0])

    def _on_font_change(self, choice: str) -> None:
        raw = "auto"
        for display, name in self._font_options:
            if display == choice:
                raw = name
                break
        try:
            import pygame
            if not pygame.font.get_init():
                pygame.font.init()
            from .visualizer import _pick_font
            resolved = _pick_font(raw)
            f = pygame.font.SysFont(resolved, 24)
            surf = f.render(t("display.font_sample"), True, (255, 255, 255))
            import numpy as np
            arr = pygame.surfarray.array_alpha(surf)
            total = arr.size
            ratio = int((arr > 0).sum()) / total if total > 0 else 0.0
            if ratio < 0.03:
                self._append_log(t("display.font_missing"))
        except Exception:
            pass
        self._send({"type": "settings", "font_name": raw})

    # -- standby image -----------------------------------------------------
    def _on_choose_standby(self) -> None:
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            title=t("dlg.select_image_title"),
            filetypes=[
                (t("dlg.images"), "*.png *.jpg *.jpeg *.bmp *.webp"),
                (t("dlg.all_files"), "*.*"),
            ],
        )
        if not path:
            return
        from .config import SETTINGS, save_prefs
        SETTINGS.visual.standby_image = path
        save_prefs()
        self.standby_label.configure(text=Path(path).name)
        self._send({"type": "settings", "standby_image": path})

    def _on_clear_standby(self) -> None:
        from .config import SETTINGS, save_prefs
        SETTINGS.visual.standby_image = ""
        save_prefs()
        self.standby_label.configure(text=t("dev.none"))
        self._send({"type": "settings", "standby_image": ""})

    # -- capture ----------------------------------------------------------
    def _sync_capture_btn(self) -> None:
        """Bottom bar text/colour reflect the current capture state."""
        if self._capturing:
            self.capture_btn.configure(
                text=t("cap.stop"), fg_color="#c92a2a", hover_color="#a61e1e")
        else:
            self.capture_btn.configure(
                text=t("cap.start"), fg_color="#2f9e44", hover_color="#2b8a3e")

    def _on_capture_toggle(self) -> None:
        if self._capturing:
            self._on_stop()
        else:
            self._on_start()

    def _on_start(self) -> None:
        self._send({"type": "start"})
        self.capture_status.configure(text=t("cap.running"))
        self.track_state.configure(text=t("track.capturing"), text_color="#48bb78")
        self._capturing = True
        self._sync_capture_btn()

    def _on_stop(self) -> None:
        self._send({"type": "stop"})
        self.capture_status.configure(text=t("cap.monitoring"))
        self.track_state.configure(text=t("track.listening"), text_color="#63b3ed")
        self._capturing = False
        self._sync_capture_btn()

    # -- visualizer management --------------------------------------------
    def _on_viz_restart(self) -> None:
        if self.viz_mgr is None:
            self._append_log(t("display.viz_mgr_unavailable"))
            return
        threading = __import__("threading")
        def worker():
            try:
                self.viz_mgr.restart()
            except Exception as exc:
                log.error("viz_restart failed: %s", exc)
        threading.Thread(target=worker, daemon=True, name="UIVizRestart").start()
        self._append_log(t("display.viz_restarting"))
        # Demo mode is runtime-only state; a restart returns to standby,
        # so clear the demo checkbox to stay in sync.
        if getattr(self, "demo_var", None) is not None:
            self.demo_var.set(False)
        self.root.after(300, self._update_viz_status_label)

    def _on_viz_reset(self) -> None:
        if self.viz_mgr is None:
            self._append_log(t("display.viz_mgr_unavailable"))
            return
        was_alive = self.viz_mgr.alive
        self.viz_mgr.reset()
        self._append_log(t("display.viz_reset_done"))
        # Demo mode is runtime-only state: a fresh process starts at
        # standby, and a soft reset returns to standby too — clear the
        # checkbox so the UI doesn't claim demo is still active.
        if getattr(self, "demo_var", None) is not None:
            self.demo_var.set(False)
        if not was_alive:
            self._append_log(t("display.viz_restarting"))
            self.root.after(400, self._update_viz_status_label)
        else:
            self.root.after(100, self._update_viz_status_label)

    def _on_gpu_toggle(self) -> None:
        from .config import SETTINGS, save_prefs
        SETTINGS.visual.gpu_acceleration = bool(self.gpu_var.get())
        save_prefs()
        msg = t("display.gpu_on") if SETTINGS.visual.gpu_acceleration else t("display.gpu_off")
        self._append_log(msg)
        # The renderer is created at visualizer startup, so restart the
        # visualizer process for the change to take effect.
        if self.viz_mgr is not None:
            self._on_viz_restart()

    def _on_demo_toggle(self) -> None:
        on = bool(self.demo_var.get())
        # Runtime-only test mode: tell the visualizer to show (or stop
        # showing) a rotating random cover. Not persisted to prefs.
        self._send({"type": "settings", "demo_mode": on})
        self._append_log(t("display.demo_on") if on else t("display.demo_off"))

    def _update_viz_status_label(self) -> None:
        if self.viz_mgr is None:
            self.viz_status_label.configure(text=t("display.viz_unknown"), text_color="gray")
            return
        if self.viz_mgr.alive:
            self.viz_status_label.configure(text=t("display.viz_running"), text_color="green")
        else:
            self.viz_status_label.configure(text=t("display.viz_stopped"), text_color="red")

    def _poll_viz_alive(self) -> None:
        self._update_viz_status_label()
        self.root.after(3000, self._poll_viz_alive)

    # -- status poller ---------------------------------------------------
    def _poll_in(self) -> None:
        try:
            while True:
                msg = self.input.get_nowait()
                self._handle_in(msg)
        except _queue.Empty:
            pass
        self.root.after(100, self._poll_in)

    def _refresh_track_state_label(self) -> None:
        """Re-apply the track-state label based on current capture text.

        Also keeps the bottom toggle button in sync: its text/colour is
        driven by the same capture-state text, so language switches and
        matcher status updates flow through one place.
        """
        try:
            text = self.capture_status.cget("text")
        except Exception:
            return
        if t("cap.running") in text or "运行中" in text:
            self.track_state.configure(text=t("track.capturing"), text_color="#48bb78")
            self._capturing = True
        elif "监听" in text or "monitoring" in text:
            self.track_state.configure(text=t("track.listening"), text_color="#63b3ed")
            self._capturing = False
        elif "已停止" in text or "Stopped" in text:
            self.track_state.configure(text=t("track.standby"), text_color="gray")
            self._capturing = False
        self._sync_capture_btn()

    def _handle_in(self, msg: dict) -> None:
        mtype = msg.get("type")
        if mtype == "log":
            self._append_log(msg.get("text", ""))
        elif mtype == "track":
            self.track_title.configure(text=msg.get("title", t("track.none")))
            self.track_artist.configure(text=f"{msg.get('artist', '')} — {msg.get('album', '')}")
            conf = msg.get("confidence")
            self.track_conf.configure(
                text=t("track.confidence", conf=conf) if conf is not None else "")
        elif mtype == "index_progress":
            done, total, info = msg.get("done", 0), msg.get("total", 0), msg.get("info", "")
            if total > 0:
                self.index_progress.set(min(1.0, done / total))
            else:
                self.index_progress.set(0.0)
            self.prep_status.configure(text=t("prep.analyzing", done=done, total=total, info=info))
        elif mtype == "index_done":
            songs = msg.get("songs", 0)
            self.index_progress.set(1.0)
            self.prep_status.configure(text=t("prep.prepared", n=songs))
        elif mtype == "library_status":
            prepared = msg.get("prepared", 0)
            pending = msg.get("pending", 0)
            total = msg.get("total", 0)
            line = t("prep.library_analyzed", n=prepared)
            if pending:
                line += t("prep.pending", pending=pending, total=total)
            self.prep_status.configure(text=line)
        elif mtype == "prep_done":
            new = msg.get("new", 0)
            total = msg.get("total", 0)
            self.cancel_btn.configure(state="disabled")
            self.analyze_btn.configure(state="normal")
            if "error" in msg:
                self._append_log(t("prep.prep_failed", error=msg["error"]))
            else:
                self._queue.clear()
                self._refresh_queue_display()
            self.prep_status.configure(text=t("prep.files_prepared", new=new, total=total))
        elif mtype == "capture_status":
            self.capture_status.configure(text=msg.get("text", ""))
            # Refreshes the track-state label AND the bottom toggle.
            self._refresh_track_state_label()
        elif mtype == "viz_status":
            self._append_log(msg.get("text", ""))
            self._update_viz_status_label()
        elif mtype == "level":
            peak = msg.get("peak", 0.0)
            rms = msg.get("rms", 0.0)
            peak_hold = msg.get("peak_hold", 0.0)
            signal = msg.get("signal", False)
            clips = msg.get("clips", 0)
            active = msg.get("active", False)
            capturing = msg.get("capturing", False)
            import math as _math
            dbfs = 20.0 * _math.log10(max(rms, 1e-3)) if rms > 0 else -60.0
            if not active:
                self.level_bar.set(0.0)
                self.clip_label.configure(text_color="gray")
                self.level_status.configure(text=t("dev.no_device"), text_color="#e56b6b")
            elif not signal:
                self.level_bar.set(0.0)
                self.clip_label.configure(text_color="gray")
                self.level_status.configure(text=t("dev.no_signal"), text_color="#f6ad55")
            else:
                self.level_bar.set(peak_hold)
                if clips > 0:
                    self.clip_label.configure(text_color="red")
                    self.level_status.configure(
                        text=t("dev.clip", dbfs=dbfs), text_color="red")
                elif capturing:
                    self.clip_label.configure(text_color="gray")
                    self.level_status.configure(
                        text=t("dev.capturing", dbfs=dbfs), text_color="#48bb78")
                else:
                    self.clip_label.configure(text_color="gray")
                    self.level_status.configure(
                        text=t("dev.signal_ok", dbfs=dbfs), text_color="#63b3ed")

    def _append_log(self, text: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text + "\n")
        if int(self.log_text.index("end-1c").split(".")[0]) > 500:
            self.log_text.delete("1.0", "2.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    # -- lifecycle --------------------------------------------------------
    def run(self) -> None:
        self.root.mainloop()

    def quit(self) -> None:
        self._send({"type": "quit"})
        self.root.destroy()
