"""2nd-screen visualizer process.

Launched as a separate process (multiprocessing.Process(target=run, args=...)).
Polls a multiprocessing.Queue for messages:

* {'type': 'spectrum', 'bins': [...], 'peak': float}
* {'type': 'track', 'title': str, 'artist': str, 'album': str, 'cover': path}
* {'type': 'settings', 'style': str, 'rotation_speed': float, 'beat_reactive': bool}
* {'type': 'status', 'text': str}
* {'type': 'quit'}

Layout (second display):
    +-------------------+-------------------+
    |                   |                   |
    |  rotating cover   |     spectrum      |
    |                   |   (bar/wave/mirror)
    |                   |                   |
    +-------------------+-------------------+
    |   title / artist / album (below spec) |
    +---------------------------------------+

Background = blurred + darkened album cover.
"""
from __future__ import annotations

import logging
import os
import queue as _q
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np

log = logging.getLogger(__name__)

# Capture the Empty exception class at module level so it stays reachable
# even when a function parameter shadows the ``queue`` name (the visualizer
# run() takes a parameter named ``queue``).
QueueEmpty = _q.Empty


def _is_process_alive(pid: int) -> bool:
    """Return True if the process with the given PID is still running.

    Used by the visualizer child process to detect a dead parent (e.g. the
    user closed the console window, killing the main process without
    running its shutdown hooks).  Without this the pygame window would
    keep running as an orphan.

    Works on Windows via ``OpenProcess`` / ``GetExitCodeProcess``.  On
    other platforms falls back to ``os.kill(pid, 0)``.
    """
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            exit_code = wintypes.DWORD()
            if ctypes.windll.kernel32.GetExitCodeProcess(
                    handle, ctypes.byref(exit_code)):
                # STILL_ACTIVE == 259
                return exit_code.value == 259
            return False
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    else:
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

SPECTRUM_STYLES = {"bar", "wave", "mirror"}

PALETTE_BG_DARKEN_ALPHA = 165   # 0..255 - higher = darker background
PALETTE_GRADIENT = [
    (70, 130, 255),    # cool blue (low energy)
    (130, 200, 255),
    (255, 200, 120),   # warm yellow (mid)
    (255, 80, 80),     # hot red (high)
]
PALETTE_TEXT = (255, 255, 255)
PALETTE_TRACK_SUB = (210, 210, 230)
PALETTE_PLACEHOLDER = (60, 60, 90)
PALETTE_STATUS = (150, 150, 170)

# Cover art is rendered (and rotated) at this many pixels across at most,
# then smoothly scaled up to the on-screen display size. At 4K the on-screen
# cover is ~1344 px; rotozoom + smoothscale of a 960 px source costs ~15 ms
# on rotation frames which (with bg + spectrum) blows the 16.7 ms budget.
# 720 px keeps rotation frames at ~9 ms (rotozoom 4 + smoothscale 5) so the
# full 4K frame stays under budget and hits 60 fps. The circle mask hides
# any upscaling softness.
MAX_COVER_RENDER_PX = 720


# --------------------------------------------------------------------------- #
# Message-driven state                                                        #
# --------------------------------------------------------------------------- #
@dataclass
class VisualState:
    bins: np.ndarray = field(default_factory=lambda: np.zeros(64, dtype=np.float32))
    peak: float = 0.0
    # Per-bin peak-hold markers.  Each entry is the last-seen peak height
    # (normalised 0..1) that has not yet decayed.  The peaks slowly drop
    # toward 0 every frame, leaving a visible "glow dot" above each bar
    # that shows the recent max.
    peak_holds: np.ndarray = field(default_factory=lambda: np.zeros(64, dtype=np.float32))
    # Per-bin peak-hold timers: how many frames since the peak was set.
    # Peak stays at full height for ~0.5s (30 frames @60fps) then decays.
    peak_timers: np.ndarray = field(default_factory=lambda: np.zeros(64, dtype=np.int32))
    title: str = ""
    artist: str = ""
    album: str = ""
    cover_path: Optional[str] = None
    style: str = "bar"
    rotation_speed: float = 0.25   # rev/s — must match DebugUI default
    beat_reactive: bool = False
    angle: float = 0.0
    status: str = ""
    status_until: float = 0.0   # monotonic timestamp; status hides after 5s
    # --- Cross-fade transition state ---
    fade_active: bool = False
    fade_progress: float = 0.0   # 0.0 = 100% old, 1.0 = 100% new
    fade_duration: float = 0.6   # seconds
    old_title: str = ""
    old_artist: str = ""
    old_album: str = ""
    old_cover_path: Optional[str] = None
    old_colors: tuple = ()       # (r,g,b) tuples from previous track
    # --- Tentative (low-confidence) display ---
    # True when the matcher showed a track whose confidence is still
    # below the confirm threshold (e.g. multi-version songs where the
    # hash count is diluted).  The text pulses gently to signal
    # "probably this, not yet locked in".
    tentative: bool = False


# --------------------------------------------------------------------------- #
# Asset helpers                                                                #
# --------------------------------------------------------------------------- #
def _set_display_env(display_index: int) -> None:
    """Tell SDL which monitor to use. Must run before pygame.init()."""
    os.environ["SDL_VIDEO_DISPLAY"] = str(display_index)


def _disable_window_ime() -> None:
    """Disassociate the IME from the current pygame window.

    On Chinese Windows, when a Chinese IME (e.g. Microsoft Pinyin) is in
    composition mode it swallows letter keys — SDL never delivers the
    KEYDOWN event, so hotkeys like F / F11 appear dead.  Calling
    ``ImmAssociateContext(hwnd, NULL)`` unbinds the IME from the window
    so keystrokes go straight through; the user does NOT have to switch
    to an English IME first.

    Must be called after every ``set_mode`` (the HWND may change when
    toggling fullscreen). No-op on non-Windows platforms.
    """
    import sys
    if sys.platform != "win32":
        return
    try:
        import ctypes
        import pygame
        wm_info = pygame.display.get_wm_info()
        hwnd = int(wm_info.get("window", 0))
        if not hwnd:
            return
        imm32 = ctypes.windll.imm32
        # HIMC ImmAssociateContext(HWND hWnd, HIMC hIMC)
        # Passing NULL disassociates the IME from this window.
        imm32.ImmAssociateContext.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        imm32.ImmAssociateContext.restype = ctypes.c_void_p
        imm32.ImmAssociateContext(hwnd, None)
    except Exception as exc:  # pragma: no cover - platform defensive
        log.warning("Could not disable window IME: %s", exc)


def _monitor_rects() -> list[tuple[int, int, int, int]]:
    """Win32 ``EnumDisplayMonitors`` rects (l, t, r, b) in SDL index order."""
    import sys
    if sys.platform != "win32":
        return []
    try:
        import ctypes
        from ctypes import wintypes
        rects: list[tuple[int, int, int, int]] = []
        MONITORENUMPROC = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
            ctypes.POINTER(wintypes.RECT), wintypes.LPARAM,
        )

        def _cb(_hmon, _hdc, lprect, _lparam):
            r = lprect.contents
            rects.append((r.left, r.top, r.right, r.bottom))
            return 1

        ctypes.windll.user32.EnumDisplayMonitors(
            0, 0, MONITORENUMPROC(_cb), 0)
        return rects
    except Exception as exc:  # pragma: no cover - platform defensive
        log.warning("EnumDisplayMonitors failed: %s", exc)
        return []


def _current_monitor_index() -> Optional[int]:
    """Return the SDL display index of the monitor the window is on NOW.

    Uses the window centre (virtual-desktop coordinates from
    ``pygame.display.get_window_position``) and the Win32
    ``EnumDisplayMonitors`` rect list.  SDL's Windows backend enumerates
    displays in the same ``EnumDisplayMonitors`` order, so the index in
    our rect list matches the ``display=`` index that ``set_mode``
    expects.  This lets F/F11 fullscreen the monitor the window is
    actually sitting on instead of a fixed config index (which could be
    a different physical screen, leaving the user stuck).
    """
    import sys
    if sys.platform != "win32":
        return None
    try:
        import pygame
        wx, wy = pygame.display.get_window_position()
        ww, wh = pygame.display.get_window_size()
        cx, cy = wx + ww // 2, wy + wh // 2
        for i, (l, t, r, b) in enumerate(_monitor_rects()):
            if l <= cx < r and t <= cy < b:
                return i
    except Exception as exc:  # pragma: no cover - platform defensive
        log.warning("Monitor detection failed: %s", exc)
    return None


def _convert_alpha(surf: "pygame.Surface") -> "pygame.Surface":
    """convert_alpha() with a safe fallback for the GPU (no set_mode) path.

    ``convert_alpha()`` needs a live display surface; in the GPU render
    path we create an SDL2 ``video.Window`` directly (no ``set_mode``), so
    no conversion format is registered and convert_alpha raises "No
    convert format has been set". In that case raw loaded images keep
    their on-disk format — often an 8-bit paletted PNG — and later
    ``pygame.transform.smoothscale`` calls crash with "Only 24-bit or
    32-bit surfaces can be smooth scaled" (standby logo / blurred-bg
    paths). Promote such surfaces to 32-bit SRCALPHA manually: blit
    needs no display mode, and the result also works with
    ``Texture.from_surface``.
    """
    try:
        return surf.convert_alpha()
    except Exception:
        pass
    try:
        import pygame
        if surf.get_bitsize() >= 24:
            return surf
        promoted = pygame.Surface(surf.get_size(), pygame.SRCALPHA, 32)
        promoted.blit(surf, (0, 0))
        return promoted
    except Exception:
        return surf


def _load_cover(path: Optional[str], square_size: int, gpu: bool = False):
    """Return (square_surface, raw_image_surface) or (None, None).

    The square is rendered at ``min(square_size, MAX_COVER_RENDER_PX)``
    pixels across. The caller scales the rotated result back up to the
    on-screen display size — this keeps ``rotozoom`` cheap at 4K while
    the circle mask hides any upscaling softness.
    """
    import pygame
    if not path or not Path(path).exists():
        return None, None
    try:
        img = pygame.image.load(path)
    except Exception as exc:
        log.warning("Could not load cover %s: %s", path, exc)
        return None, None
    img = _convert_alpha(img)

    # Internal render resolution. CPU path caps it so rotozoom stays
    # fast; GPU path renders at full display size because the renderer
    # rotates the texture in hardware (free) and we want full quality.
    render_size = square_size if gpu else min(square_size, MAX_COVER_RENDER_PX)

    # Square (preserve aspect via center-crop) for the rotating cover.
    w, h = img.get_size()
    s = min(w, h)
    square = pygame.Surface((s, s), pygame.SRCALPHA)
    square.blit(img, (-(w - s) // 2, -(h - s) // 2))
    square = pygame.transform.smoothscale(square, (render_size, render_size))

    # Mask to a circle so rotated corners stay transparent.
    mask = pygame.Surface((render_size, render_size), pygame.SRCALPHA)
    pygame.draw.circle(
        mask, (255, 255, 255, 255),
        (render_size // 2, render_size // 2), render_size // 2,
    )
    square.blit(mask, (0, 0), special_flags=pygame.BLEND_RGBA_MIN)
    return square, img


def _make_blurred_bg(img, screen_w: int, screen_h: int, blur_px: int):
    """Aspect-fill the screen with a heavily blurred, darkened cover copy.

    ``blur_px controls the blur strength: smaller values shrink further
    before re-enlarging, giving stronger blur. Default 8 produces near-solid
    colour blocks with no visible pixel structure.
    """
    import pygame
    img = _convert_alpha(img)   # smoothscale needs a 24/32-bit surface
    iw, ih = img.get_size()
    scale = max(screen_w / iw, screen_h / ih)
    sw, sh = max(1, int(iw * scale)), max(1, int(ih * scale))
    scaled = pygame.transform.smoothscale(img, (sw, sh))
    cropped = pygame.Surface((screen_w, screen_h))
    cropped.blit(scaled, ((screen_w - sw) // 2, (screen_h - sh) // 2))
    # Heavy blur: shrink to a tiny surface, then enlarge. blur_px=8 means
    # the intermediate is screen/8 in each dim - small enough to erase all
    # pixel structure.
    tw = max(2, screen_w // blur_px)
    th = max(2, screen_h // blur_px)
    tiny = pygame.transform.smoothscale(cropped, (tw, th))
    blurred = pygame.transform.smoothscale(tiny, (screen_w, screen_h))
    darken = pygame.Surface((screen_w, screen_h), pygame.SRCALPHA)
    darken.fill((0, 0, 0, PALETTE_BG_DARKEN_ALPHA))
    blurred.blit(darken, (0, 0))
    return blurred


# --- Key-colour extraction + flowing background ----------------------------
def _cluster_palette(pixels, n: int = 3) -> list[tuple[int, int, int]]:
    """Cluster a list of (r, g, b) pixels into ``n`` dominant colours.

    Quantises to 32-step buckets, keeps the most frequent ones while
    skipping near-black / near-white and near-duplicate colours.
    """
    from collections import Counter
    default = [(40, 40, 60), (70, 50, 90), (30, 60, 80)]
    quantised = [(r // 32 * 32, g // 32 * 32, b // 32 * 32)
                 for (r, g, b) in pixels]
    counts = Counter(quantised).most_common()
    picked: list[tuple[int, int, int]] = []
    for (qr, qg, qb), _ in counts:
        # Skip near-black / near-white (boring backgrounds).
        if qr < 20 and qg < 20 and qb < 25:
            continue
        if qr > 230 and qg > 230 and qb > 230:
            continue
        # Skip if too similar to an already-picked colour.
        too_close = any(
            abs(qr - p[0]) < 40 and abs(qg - p[1]) < 40 and abs(qb - p[2]) < 40
            for p in picked
        )
        if too_close:
            continue
        picked.append((qr, qg, qb))
        if len(picked) >= n:
            break
    # Fallback fill if not enough distinct colours.
    while len(picked) < n:
        picked.append(default[len(picked) % len(default)])
    return picked[:n]


def _extract_dominant_colors(img, n: int = 3) -> list[tuple[int, int, int]]:
    """Extract ``n`` dominant colours from a cover surface.

    Downscales to 10x10 and clusters similar colours, returning the most
    representative colour from each cluster. Falls back to a default
    palette if the image is empty.
    """
    import pygame
    try:
        tiny = pygame.transform.smoothscale(img, (10, 10))
        arr = pygame.surfarray.array3d(tiny)   # (10, 10, 3)
        pixels = arr.reshape(-1, 3).tolist()
    except Exception:
        return [(40, 40, 60), (70, 50, 90), (30, 60, 80)][:n]
    return _cluster_palette(pixels, n)


def _extract_dominant_colors_alpha(img, n: int = 3) -> list[tuple[int, int, int]]:
    """Dominant colours from an image WITH an alpha channel.

    Only opaque pixels (alpha >= 128) contribute, so transparent logo
    backgrounds don't pollute the palette. Used for the standby image.
    """
    import pygame
    try:
        tiny = pygame.transform.smoothscale(img, (24, 24))
        rgb = pygame.surfarray.array3d(tiny).reshape(-1, 3)
        alpha = pygame.surfarray.array_alpha(tiny).reshape(-1)
        pixels = [tuple(px) for px, a in zip(rgb.tolist(), alpha.tolist())
                  if a >= 128]
    except Exception:
        return [(40, 40, 60), (70, 50, 90), (30, 60, 80)][:n]
    if not pixels:
        return [(40, 40, 60), (70, 50, 90), (30, 60, 80)][:n]
    return _cluster_palette(pixels, n)


# Max fraction of the screen the standby image may occupy (per axis).
STANDBY_MAX_RATIO = 0.40
# Standby → first-track reveal is a TWO-STAGE dissolve: the image fades
# out (STANDBY_FADE_DURATION), there's a short gap showing only the
# flowing background (STANDBY_GAP), then the cover + text fade in
# (STANDBY_FADE_DURATION again).
STANDBY_FADE_DURATION = 1.5
STANDBY_GAP = 0.5


def _load_standby_raw(path: str):
    """Load the standby image WITH its alpha channel and its palette.

    Returns ``(raw_surface, [dominant colours])`` or ``(None, None)``.
    """
    import pygame
    try:
        raw = _convert_alpha(pygame.image.load(path))
    except Exception as exc:
        log.warning("Standby image load failed (%s): %s", path, exc)
        return None, None
    colors = _extract_dominant_colors_alpha(raw, n=3)
    return raw, colors


def _scale_standby(raw, screen_w: int, screen_h: int):
    """Scale the standby image to fit within 40% of both screen axes.

    Never upscales (keeps small logos crisp); preserves aspect ratio and
    the alpha channel via ``smoothscale``.
    """
    import pygame
    # smoothscale rejects 8-bit paletted surfaces; in the GPU path loaded
    # images may still be 8-bit (no set_mode → convert_alpha unavailable).
    # Normalise here too so the function is safe regardless of caller.
    raw = _convert_alpha(raw)
    iw, ih = raw.get_size()
    if iw <= 0 or ih <= 0:
        return None
    box_w = screen_w * STANDBY_MAX_RATIO
    box_h = screen_h * STANDBY_MAX_RATIO
    scale = min(box_w / iw, box_h / ih, 1.0)
    if scale >= 1.0:
        return raw   # already fits — keep the original (alpha intact)
    nw = max(1, int(iw * scale))
    nh = max(1, int(ih * scale))
    return pygame.transform.smoothscale(raw, (nw, nh))


def _make_flowing_bg(
    colors: list[tuple[int, int, int]],
    screen_w: int, screen_h: int,
    t: float, energy: float = 0.0, gpu: bool = False,
) -> "pygame.Surface":
    """Draw a full-screen, soft, flowing vector wave-field background.

    Renders a continuous field of overlapping low-frequency sine waves
    whose amplitude varies across the screen. Each wave band carries
    one of the dominant colours, and every band fills the full screen
    width - so the result reads as gently rippling colour swells that
    drift and morph, with no empty gaps and no pixelation.

    Softness comes from three design choices:
      * Low spatial frequencies (<=2 cycles per screen) - wide bands,
        no tight stripe boundaries.
      * Cosine-smoothed colour blend at colour stops - buttery gradient
        transitions instead of linear ramps' abrupt direction change.
      * Slow temporal coefficients (periods 60-100s) and 5+ wave layers
        with different drift speeds so the field continuously morphs
        without ever appearing to "tick".

    The wave-field subtly reacts to music energy: higher energy
    increases wave amplitude, making the swells more pronounced.
    """
    import pygame
    import math
    import numpy as np

    # Work at low resolution then upscale for both performance and
    # built-in softness. 160px wide is plenty for sub-2-cycle waves.
    # The GPU path returns this small surface and lets the renderer
    # upscale it (bilinear filtering via SDL_RENDER_SCALE_QUALITY); 320px
    # there keeps 4K fullscreen upscales crisp and band-free, while the
    # field generation is still only ~64k cells (a millisecond or two).
    base_w = 320 if gpu else 160
    low_w, low_h = base_w, max(1, int(base_w * screen_h / screen_w))
    low_w, low_h = max(2, low_w), max(2, low_h)

    # Build the coordinate grid.
    x = np.linspace(0.0, 1.0, low_w, dtype=np.float32)
    y = np.linspace(0.0, 1.0, low_h, dtype=np.float32)
    X, Y = np.meshgrid(x, y)   # both (low_h, low_w)

    # --- Compute the wave phase field ---
    # Stack of low-frequency sine waves. Each layer has a different
    # spatial direction + drift speed so the field continuously morphs
    # rather than scrolling. Energy grows the amplitude slightly.
    amp = 0.25 + 0.12 * energy

    # Layer 1: very slow vertical swell, drifting diagonally.
    # Period ~60s, wavelength ~2/3 of screen height.
    phase = (
        0.5
        + amp * np.sin(Y * math.pi * 1.5 + t * 0.10 + X * 0.6)
        # Layer 2: counter-drifting medium wave (creates morph).
        # Period ~90s.
        + amp * 0.7 * np.sin(Y * math.pi * 2.0 - t * 0.07 + X * 1.1)
        # Layer 3: slow diagonal flow (X+Y) - breaks pure-vertical banding
        # and adds a sense of left-right motion.
        + amp * 0.5 * np.sin((X + Y) * math.pi * 1.6 + t * 0.06)
        # Layer 4: counter-diagonal, even slower.
        + amp * 0.4 * np.sin((X - Y) * math.pi * 1.2 - t * 0.045)
        # Layer 5: subtle texture ripple - low amplitude, mid frequency,
        # prevents the field from looking totally flat in still frames.
        + amp * 0.18 * np.sin(X * math.pi * 2.4 + Y * math.pi * 1.8
                              + t * 0.08)
    )
    # Normalise to [0, 1].
    phase = (phase - phase.min()) / (phase.max() - phase.min() + 1e-9)

    # --- Blend colours across the phase field with cosine smoothing ---
    # Map phase [0,1] to a position along the colour ramp, wrapping so
    # the colours cycle smoothly. Cosine smoothing on the blend factor
    # gives buttery transitions at colour stops (the derivative is zero
    # at the boundaries), avoiding the abrupt direction change that
    # linear interpolation produces.
    n = max(2, len(colors))
    ramp = np.array(colors[:n], dtype=np.float32)   # (n, 3)
    pos = phase * n   # 0..n - cycles through all colours
    i0 = np.floor(pos).astype(np.int32) % n
    i1 = (i0 + 1) % n
    raw_frac = pos - np.floor(pos)
    # Cosine smoothstep: 0->0, 0.5->0.5, 1->1, derivative=0 at ends.
    frac = (1.0 - np.cos(raw_frac * math.pi)) * 0.5
    frac = frac[..., None]   # (low_h, low_w, 1)
    c0 = ramp[i0]   # (low_h, low_w, 3)
    c1 = ramp[i1]
    rgb = c0 * (1.0 - frac) + c1 * frac   # (low_h, low_w, 3)

    # Soft vignette toward the edges so the foreground text / cover stay
    # readable. Kept gentle (0.25 strength) so it doesn't read as a
    # hard frame.
    cx, cy = 0.5, 0.5
    dist = np.sqrt((X - cx) ** 2 + (Y - cy) ** 2)
    vignette = np.clip(1.0 - 0.25 * dist, 0.7, 1.0)[..., None]
    rgb = rgb * vignette

    # Darken overall so text stays legible.
    rgb = rgb * 0.55 + np.array([10, 10, 18], dtype=np.float32) * 0.45

    # Clip + convert to uint8 for blit.
    rgb = np.clip(rgb, 0, 255).astype(np.uint8)   # (low_h, low_w, 3)
    # pygame surfarray uses (w, h, 3) so transpose axes.
    arr = np.transpose(rgb, (1, 0, 2))   # (low_w, low_h, 3)

    surf = pygame.surfarray.make_surface(arr)
    if not gpu:
        return pygame.transform.smoothscale(surf, (screen_w, screen_h))
    # GPU path: return the small wave-field surface — the renderer
    # scales it up to the screen size on the GPU with bilinear filtering
    # (SDL_RENDER_SCALE_QUALITY=linear, set before SDL init), giving a
    # smooth, mosaic-free background at zero per-frame CPU cost.
    return surf


# --------------------------------------------------------------------------- #
# Font selection (CJK-aware)                                                   #
# --------------------------------------------------------------------------- #
# pygame's ``SysFont("Arial", ...)`` doesn't ship CJK glyphs on Windows, so
# Chinese / Japanese track titles render as empty boxes. We probe a list of
# fonts known to carry CJK characters and pick the first one that actually
# renders a sample CJK string without missing glyphs.

# Platform-ordered: probe native UI fonts first so titles render in the
# OS's standard CJK typeface, then fall back to cross-platform installs.
if sys.platform == "darwin":
    _CJK_FONT_CANDIDATES = [
        "PingFang SC",          # macOS, Simplified Chinese
        "PingFang TC",          # macOS, Traditional Chinese
        "Hiragino Sans GB",     # macOS, Simplified Chinese (older name)
        "Hiragino Sans",        # macOS, Japanese
        "STHeiti",              # macOS legacy
        "Arial Unicode MS",     # macOS fallback (Office)
        "Noto Sans CJK SC",     # Cross-platform fallback
        "Noto Sans CJK JP",
        "Source Han Sans SC",
        "Source Han Sans JP",
    ]
else:
    _CJK_FONT_CANDIDATES = [
        "Microsoft YaHei",      # Windows, Simplified Chinese
        "Microsoft YaHei UI",
        "Yu Gothic",            # Windows, Japanese
        "Yu Gothic UI",
        "MS Gothic",
        "SimHei",               # Windows, Simplified Chinese (legacy)
        "SimSun",
        "Noto Sans CJK SC",     # Cross-platform fallback
        "Noto Sans CJK JP",
        "Source Han Sans SC",
        "Source Han Sans JP",
        "Arial Unicode MS",
        "PingFang SC",          # macOS (in case the list runs elsewhere)
        "Hiragino Sans",
    ]

_cjk_font_cache: str | None = None

# Probe results cache: font_name -> supports_cjk (bool). Persisted across
# calls so the UI doesn't re-render sample strings every refresh.
_font_cjk_probe_cache: dict[str, bool] = {}

# Cached list of (name, supports_cjk) for the system's available fonts.
_system_font_list_cache: list[tuple[str, bool]] | None = None

# Sample strings used to probe a font's glyph coverage. If a font renders
# all of these without missing-glyph boxes, it's marked CJK-capable.
_CJK_PROBE_SAMPLE = "中文日本語한글"   # SC + TC + JP + KR
_LATIN_PROBE_SAMPLE = "ABC abc 123"


def _supports_cjk(name: str) -> bool:
    """Return True if ``name`` is a system font that natively carries
    CJK glyphs (Chinese / Japanese / Korean).

    Probes by loading the font file directly and rendering a single CJK
    character, then checking the rendered width. Real CJK glyphs at
    size 24 are full-width (~20-24px); missing-glyph .notdef boxes are
    narrow (~4-8px). Width-based detection is more reliable than alpha
    coverage, which is fooled by the tofu box's own border pixels.

    We load the font file DIRECTLY via ``Font(path)`` rather than
    ``SysFont(name)`` to bypass SDL's font-linking fallback, which on
    Windows silently substitutes Microsoft YaHei for missing CJK
    glyphs and would make every font appear CJK-capable.

    Caches per-font results.
    """
    import pygame
    if name in _font_cjk_probe_cache:
        return _font_cjk_probe_cache[name]
    try:
        # Resolve the font name to an actual file path. Returns None
        # if the name isn't a registered system font.
        path = pygame.font.match_font(name)
        if path is None:
            _font_cjk_probe_cache[name] = False
            return False
        # Load directly from the file - no SysFont fallback substitution.
        f = pygame.font.Font(path, 24)
        # Render a single CJK character and check its width. Real CJK
        # glyphs at size 24 are ~20-24px wide; .notdef tofu boxes are
        # typically 4-8px. Threshold at 14px (58% of size) to catch
        # narrow-but-real CJK fonts while rejecting tofu.
        surf = f.render("中", True, (255, 255, 255))
        supports = surf.get_width() >= 14
    except Exception:
        supports = False
    _font_cjk_probe_cache[name] = supports
    return supports


def _list_system_fonts() -> list[tuple[str, bool]]:
    """Enumerate the system's available fonts and CJK support.

    Returns a list of (font_name, supports_cjk) tuples sorted so that
    CJK-capable fonts come first (alphabetically within each group).
    Cached after the first call.
    """
    import pygame
    global _system_font_list_cache
    if _system_font_list_cache is not None:
        return _system_font_list_cache

    # pygame.font.get_fonts() returns lowercased normalised names from the
    # system. Dedup + title-case for display.
    raw = sorted({n for n in pygame.font.get_fonts() if n})
    out: list[tuple[str, bool]] = []
    for n in raw:
        display = n.title()
        out.append((display, _supports_cjk(n)))

    # Sort: CJK-capable first (alphabetical), then the rest (alphabetical).
    cjk = sorted([p for p in out if p[1]], key=lambda p: p[0].lower())
    other = sorted([p for p in out if not p[1]], key=lambda p: p[0].lower())
    _system_font_list_cache = cjk + other
    return _system_font_list_cache


def _pick_cjk_font() -> str:
    """Return a font name known to support CJK glyphs on this system.

    Walks the well-known CJK candidates list, returning the first that
    actually renders CJK glyphs (verified by direct file load, bypassing
    SDL's font-linking fallback). Caches the result so the probe runs
    only once per process.
    """
    global _cjk_font_cache
    if _cjk_font_cache is not None:
        return _cjk_font_cache

    for name in _CJK_FONT_CANDIDATES:
        if _supports_cjk(name):
            _cjk_font_cache = name
            log.info("Selected CJK font: %s", name)
            return name

    # Fallback: let pygame pick whatever it can.
    log.warning("No CJK font found - Chinese/Japanese text may not render.")
    _cjk_font_cache = "Arial"
    return "Arial"


def _pick_font(name: str) -> str:
    """Resolve a user-chosen font name to one pygame can actually load.

    If ``name`` is empty or "auto", returns the auto-detected CJK font.
    If ``name`` is a real system font, returns it as-is (the caller is
    responsible for handling missing-glyph rendering if it lacks CJK).

    Uses ``pygame.font.match_font`` to verify the name resolves to a
    real font file - ``SysFont`` silently substitutes a default font
    when the name is unknown, which would mask the error.
    """
    import pygame
    if not name or name.lower() in ("auto", "default", ""):
        return _pick_cjk_font()
    # Verify the font name resolves to an actual file on disk.
    if pygame.font.match_font(name) is None:
        log.warning("Font %r not found, using auto CJK fallback.", name)
        return _pick_cjk_font()
    return name


# --------------------------------------------------------------------------- #
# Responsive layout                                                            #
# --------------------------------------------------------------------------- #
@dataclass
class Layout:
    """All on-screen rectangles for one frame, in pixel coordinates.

    Computed by :func:`_compute_layout` from current screen size + aspect
    ratio. Renderers take these rects verbatim - they never re-derive
    geometry themselves.
    """
    cover_rect: "pygame.Rect"       # square area for the rotating cover art
    spectrum_rect: "pygame.Rect"   # area for the spectrum (right side)
    text_rect: "pygame.Rect"       # title/artist/album strip below spectrum


def _compute_layout(screen_w: int, screen_h: int) -> Layout:
    """Compute the on-screen rectangles for the current resolution.

    Design goals:
    * Cover stays square and centered in its half of the screen.
    * Spectrum area keeps a 4:3-ish internal aspect on the right so it
      doesn't stretch into a thin ribbon on ultrawide monitors.
    * Title strip reserves a proportional band of the screen height.
    * On portrait / squarish displays the layout gracefully degrades to
      a top/bottom split instead of left/right so neither element is
      crushed.
    """
    import pygame
    aspect = screen_w / max(1, screen_h)
    portrait = aspect < 1.0
    if portrait:
        return _compute_layout_vertical(screen_w, screen_h)
    return _compute_layout_horizontal(screen_w, screen_h, aspect)


def _compute_layout_horizontal(
    screen_w: int, screen_h: int, aspect: float,
) -> Layout:
    """Layout for landscape / ultrawide displays (left=cover, right=spectrum)."""
    import pygame
    # On ultrawide (aspect > 2.2) or 32:9 monitors, giving each side half
    # the width leaves the cover tiny and the spectrum enormous. Cap the
    # cover half to ~45% of screen width and let the spectrum take the
    # remainder - the basic "cover on the left, spectrum on the right"
    # composition is preserved.
    cover_half_ratio = 0.45 if aspect > 2.2 else 0.5
    left_w = int(screen_w * cover_half_ratio)
    right_w = screen_w - left_w

    # Title strip is a proportional band at the bottom of the right column.
    text_band_h = max(110, int(screen_h * 0.22))
    spectrum_h = screen_h - text_band_h
    spectrum_rect = pygame.Rect(left_w, 0, right_w, spectrum_h)
    # Shrink the text block to 70 % of its band and anchor it to the
    # left edge of the right column — i.e. toward the screen centre —
    # so the song info doesn't sprawl across the whole bottom-right.
    text_w = int(right_w * 0.70)
    text_h = int(text_band_h * 0.70)
    text_x = left_w
    text_y = spectrum_h + (text_band_h - text_h) // 2
    text_rect = pygame.Rect(text_x, text_y, text_w, text_h)

    # Square cover, centred in the left half, sized to 70% of the smaller
    # of (left_w, screen_h) so it never overflows.
    cover_side = int(min(left_w, screen_h) * 0.70)
    cover_x = (left_w - cover_side) // 2
    cover_y = (screen_h - cover_side) // 2
    cover_rect = pygame.Rect(cover_x, cover_y, cover_side, cover_side)

    return Layout(
        cover_rect=cover_rect,
        spectrum_rect=spectrum_rect,
        text_rect=text_rect,
    )


def _compute_layout_vertical(screen_w: int, screen_h: int) -> Layout:
    """Layout for portrait / squarish displays (top=cover, bottom=spectrum).

    Vertical order, top to bottom:
      1. Cover art (square, centred)
      2. Song info text strip (title / artist / album)
      3. Bar spectrum pinned to the bottom edge of the screen
    """
    import pygame
    # Proportions: cover ~50%, text ~18%, spectrum ~32% (bottom-pinned).
    cover_band_h = int(screen_h * 0.50)
    text_band_h = max(80, int(screen_h * 0.18))
    spectrum_h = screen_h - cover_band_h - text_band_h
    spectrum_h = max(80, spectrum_h)

    cover_side = int(min(screen_w, cover_band_h) * 0.80)
    cover_x = (screen_w - cover_side) // 2
    cover_y = (cover_band_h - cover_side) // 2
    cover_rect = pygame.Rect(cover_x, cover_y, cover_side, cover_side)

    text_y = cover_band_h
    text_rect = pygame.Rect(0, text_y, screen_w, text_band_h)

    spectrum_y = text_y + text_band_h
    spectrum_rect = pygame.Rect(0, spectrum_y, screen_w, spectrum_h)
    return Layout(
        cover_rect=cover_rect,
        spectrum_rect=spectrum_rect,
        text_rect=text_rect,
    )


# --------------------------------------------------------------------------- #
# Spectrum renderers                                                           #
# --------------------------------------------------------------------------- #
def _color_for(v: float):
    """Map bin height (0..1) to the energy gradient palette.

    Low-energy bars are cool blue, mid-energy warm yellow, high-energy
    hot red.  This is the classic spectrum-bar colour scheme that
    instantly communicates loudness.
    """
    idx = int(v * (len(PALETTE_GRADIENT) - 1))
    return PALETTE_GRADIENT[max(0, min(idx, len(PALETTE_GRADIENT) - 1))]


def _draw_bar(surf, bins, rect, peak_holds=None, cap_frac=0.70, peak_timers=None) -> None:
    """Draw bar spectrum with a hard ceiling on max bar height.

    ``cap_frac`` limits the tallest bar to a fraction of ``rect.height``.
    0.70 means full-volume bars only reach 70% of the allocated zone,
    leaving a visual breathing room above so the layout doesn't feel
    cramped.  Bars stay bottom-anchored.

    A small horizontal padding (``pad_frac`` of the rect width on each
    side) keeps the first/last bars from touching the screen edges.

    Peak-hold markers stay at full height for ~0.5s, then decay smoothly.
    """
    import pygame
    n = len(bins)
    if n == 0 or rect.width <= 0 or rect.height <= 0:
        return
    bins = np.nan_to_num(bins, nan=0.0, posinf=1.0, neginf=0.0)
    # Horizontal padding so bars don't touch the left/right edges.
    pad = max(4, int(rect.width * 0.025))
    draw_x = rect.x + pad
    draw_w = rect.width - 2 * pad
    if draw_w <= 0:
        draw_w = rect.width
        draw_x = rect.x
    bar_w = draw_w / n
    gap = max(1, int(bar_w * 0.12))
    # Peak-hold with true 0.5s hold: peak stays at full height for 30 frames
    # (0.5s @60fps) after the last time it was set, THEN starts decaying.
    if peak_holds is not None and len(peak_holds) == n:
        peak_holds = np.nan_to_num(peak_holds, nan=0.0, posinf=1.0, neginf=0.0)
        # Update peak values: if current bar exceeds stored peak, reset.
        new_peak_mask = bins > peak_holds
        peak_holds = np.where(new_peak_mask, bins, peak_holds)
        # Reset timers where new peaks were set.
        if peak_timers is not None:
            peak_timers = np.where(new_peak_mask, 0, peak_timers)
            peak_timers += 1
            # Hold phase: first 30 frames (0.5s @60fps), peak stays at full height.
            # Decay phase: after 30 frames, peak decays at 0.92/frame (~2s full drop).
            hold_frames = 30
            decay_mask = peak_timers > hold_frames
            decay_factor = 0.92
            peak_holds = np.where(decay_mask, peak_holds * decay_factor, peak_holds)
    cap_height = rect.height * cap_frac
    for i, v in enumerate(bins):
        v = float(v)
        if not (v >= 0.0):
            v = 0.0
        v = min(1.0, v)
        # Anchor bottom; cap at 70% of rect height.
        h = max(2, v * cap_height)
        x = draw_x + i * bar_w
        y = rect.y + rect.height - h
        pygame.draw.rect(
            surf, _color_for(v),
            (int(x), int(y), max(1, int(bar_w) - gap), int(h)),
        )
        # Peak-hold marker above the bar (also capped at cap_frac).
        if peak_holds is not None and len(peak_holds) == n:
            ph = float(peak_holds[i])
            ph = max(v, min(1.0, ph))
            if ph > 0.02:
                ph_h = ph * cap_height
                ph_y = rect.y + rect.height - ph_h
                peak_color = tuple(min(255, int(c * 1.3)) for c in _color_for(ph))
                cap_h = max(1, int(bar_w * 0.6))
                pygame.draw.rect(
                    surf, peak_color,
                    (int(x), int(ph_y - cap_h), max(1, int(bar_w) - gap), cap_h),
                )


def _draw_wave(surf, bins, rect, cap_frac=0.70) -> None:
    """Draw a filled waveform line with the SAME composition as ``_draw_bar``.

    Composition parity with the bar style:
      * Bottom-anchored (line rides along the bottom edge, rises up).
      * Max height capped at ``cap_frac`` (default 70%) of ``rect.height``
        so the visual breathing room above matches the bar style.
      * Horizontal padding (``pad_frac`` of rect width on each side) so
        the line endpoints don't touch the screen edges.
      * Same ``_color_for`` energy-based colour mapping as the bars
        (cool blue → warm yellow → hot red) instead of a hardcoded tint.

    The wave is a filled polygon (area beneath the line) plus an
    anti-aliased outline on top, drawn from the normalised bin values.
    """
    import pygame
    n = len(bins)
    if n == 0 or rect.width <= 0 or rect.height <= 0:
        return
    bins = np.nan_to_num(bins, nan=0.0, posinf=1.0, neginf=0.0)
    # Same horizontal padding as _draw_bar.
    pad = max(4, int(rect.width * 0.025))
    draw_x = rect.x + pad
    draw_w = rect.width - 2 * pad
    if draw_w <= 0:
        draw_w = rect.width
        draw_x = rect.x
    cap_height = rect.height * cap_frac

    pts = []
    for i, v in enumerate(bins):
        v = float(v)
        if not (v >= 0.0):
            v = 0.0
        v = min(1.0, v)
        x = draw_x + (i / max(1, n - 1)) * draw_w
        y = rect.y + rect.height - v * cap_height
        pts.append((x, y))

    # Filled translucent area beneath the line.  Colour is the mean
    # energy colour so the fill reads as the overall track intensity.
    mean_v = float(np.clip(bins.mean(), 0.0, 1.0))
    fill_rgb = _color_for(mean_v)
    fill = pygame.Surface((rect.width, rect.height), pygame.SRCALPHA)
    local = [(p[0] - rect.x, p[1] - rect.y) for p in pts]
    local += [(rect.width, rect.height), (0, rect.height)]
    pygame.draw.polygon(fill, (*fill_rgb, 110), local)
    surf.blit(fill, rect.topleft)

    # Anti-aliased outline using per-point energy colour (segment by
    # segment so the line shifts colour with local energy, like bars).
    if len(pts) >= 2:
        for i in range(len(pts) - 1):
            v = float(np.clip(bins[i], 0.0, 1.0))
            pygame.draw.aaline(surf, _color_for(v), pts[i], pts[i + 1])


def _draw_mirror(surf, bins, rect, cap_frac=0.70) -> None:
    """Draw a centre-symmetric linear waveform.

    Unlike ``_draw_wave`` (which is bottom-anchored), the mirror style
    draws the waveform symmetrically around the horizontal centre line
    of the spectrum rect: each bin's amplitude extends both UP and DOWN
    from the midline by the same amount.  This creates the classic
    "oscilloscope trace" look — a line that ripples above and below the
    centre axis.

    Composition parity with the bar / wave styles:
      * Max amplitude capped at ``cap_frac`` (default 70%) of half the
        rect height, so the total peak-to-peak swing is 70% of rect
        height — matching the bar ceiling.
      * Same horizontal padding as ``_draw_bar``.
      * Same ``_color_for`` energy-based colour mapping.

    Two passes:
      1. A filled translucent band between the upper and lower traces
         (gives it body / glow).
      2. Anti-aliased upper + lower outlines, coloured by local energy.
    """
    import pygame
    n = len(bins)
    if n == 0 or rect.width <= 0 or rect.height <= 0:
        return
    bins = np.nan_to_num(bins, nan=0.0, posinf=1.0, neginf=0.0)
    pad = max(4, int(rect.width * 0.025))
    draw_x = rect.x + pad
    draw_w = rect.width - 2 * pad
    if draw_w <= 0:
        draw_w = rect.width
        draw_x = rect.x

    mid_y = rect.y + rect.height / 2.0
    # Half-amplitude = cap_frac * half-height  →  peak-to-peak = cap_frac * height.
    amp_max = (rect.height * cap_frac) / 2.0

    upper = []
    lower = []
    for i, v in enumerate(bins):
        v = float(v)
        if not (v >= 0.0):
            v = 0.0
        v = min(1.0, v)
        x = draw_x + (i / max(1, n - 1)) * draw_w
        a = v * amp_max
        upper.append((x, mid_y - a))
        lower.append((x, mid_y + a))

    # Filled translucent band between upper and lower traces.
    mean_v = float(np.clip(bins.mean(), 0.0, 1.0))
    fill_rgb = _color_for(mean_v)
    fill = pygame.Surface((rect.width, rect.height), pygame.SRCALPHA)
    poly_local = [(p[0] - rect.x, p[1] - rect.y) for p in upper]
    poly_local += [(p[0] - rect.x, p[1] - rect.y) for p in reversed(lower)]
    pygame.draw.polygon(fill, (*fill_rgb, 90), poly_local)
    surf.blit(fill, rect.topleft)

    # Anti-aliased outlines (upper + lower), per-segment energy colour.
    if n >= 2:
        for i in range(n - 1):
            v = float(np.clip(bins[i], 0.0, 1.0))
            c = _color_for(v)
            pygame.draw.aaline(surf, c, upper[i], upper[i + 1])
            pygame.draw.aaline(surf, c, lower[i], lower[i + 1])


def _draw_text(surf, title, artist, album, rect, font, sub_font) -> None:
    import pygame
    y = rect.y
    if title:
        s = font.render(title, True, PALETTE_TEXT)
        surf.blit(s, (rect.x, y))
        y += s.get_height() + 8
    if artist:
        s = sub_font.render(artist, True, PALETTE_TRACK_SUB)
        surf.blit(s, (rect.x, y))
        y += s.get_height() + 4
    if album:
        s = sub_font.render(album, True, PALETTE_TRACK_SUB)
        surf.blit(s, (rect.x, y))


# --------------------------------------------------------------------------- #
# Main loop                                                                    #
# --------------------------------------------------------------------------- #
def run(queue, display_index: int = 1) -> None:
    import math
    import logging
    import pygame
    from . import config as _cfg
    from . import __version__ as _version
    from .config import SETTINGS

    # Spawn-ed child does not inherit parent's logging config. Write
    # visualizer logs to a file so GPU renderer info + FPS are visible.
    try:
        _fh = logging.FileHandler(
            os.path.join(os.path.dirname(os.path.dirname(__file__)),
                         "visualizer.log"), mode="w", encoding="utf-8")
        _fh.setFormatter(logging.Formatter(
            "%(asctime)s %(levelname)-7s %(name)s: %(message)s"))
        log.addHandler(_fh)
        log.setLevel(logging.INFO)
    except Exception:
        pass

    # This is a spawn-ed child process: it does NOT inherit the parent's
    # in-memory SETTINGS. Load persisted prefs explicitly so fullscreen
    # state, window size, standby image, fonts, etc. are correct before
    # the display surface is created (idempotent if already loaded).
    try:
        _cfg.load_prefs()
    except Exception as _exc:
        log.warning("prefs load failed, using defaults: %s", _exc)

    _set_display_env(display_index)
    # Bilinear texture scaling: the GPU background is a small wave-field
    # surface (e.g. 320x200) that the renderer upscales to the full screen
    # size (~8-16x). SDL's default scale mode is NEAREST, which turns the
    # upscaled background into hard-edged mosaic blocks / colour bands.
    # This hint is read when each texture is created and selects smooth
    # bilinear filtering instead. Must be set before pygame.init().
    os.environ["SDL_RENDER_SCALE_QUALITY"] = "linear"
    # The visualizer never plays audio.  pygame.init() brings the mixer
    # up anyway, and SDL audio init can fail on machines with no audio
    # hardware — force the dummy audio driver so the viz subprocess is
    # completely independent of any sound card/driver.  setdefault()
    # leaves an explicit override (set by the user or the environment)
    # untouched.
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    # Declare per-monitor DPI awareness BEFORE SDL creates a window.
    # Without this the process is DPI-virtualised: SDL reports a fallback
    # 640x480 desktop in fullscreen (instead of the monitor's real native
    # resolution, e.g. 2560x1600) and windowed content is bitmap-scaled
    # (blurry). With it, the GPU renderer's drawable is the true native
    # pixel size → crisp native-resolution rendering with vsync.
    if os.name == "nt":
        try:
            import ctypes
            # PROCESS_PER_MONITOR_DPI_AWARE = 2 (shcore, Win 8.1+)
            ctypes.windll.shcore.SetProcessDpiAwareness(2)
        except Exception:
            try:
                import ctypes
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:
                pass
    pygame.init()
    pygame.display.set_caption(f"VJVision 可视化输出  v{_version}")

    # Parent PID — used to detect a dead parent (console window closed,
    # main process killed without running shutdown hooks).  If the parent
    # dies we exit cleanly instead of lingering as an orphan window.
    _parent_pid = os.getppid()

    # ----- mutable rendering state ---------------------------------------
    # Wrapped in a dict so the nested ``_reinit_display`` helper can mutate
    # the locals without ``nonlocal`` gymnastics across many variables.
    rs = {
        "screen": None,
        "screen_w": 0,
        "screen_h": 0,
        # Last-known-good WINDOWED surface size.  This is the size we
        # restore to when leaving fullscreen.  It is ONLY ever updated
        # while windowed (user resize / actual surface size) — fullscreen
        # VIDEORESIZE events (which carry the monitor's full desktop
        # resolution) must never corrupt it, otherwise the second
        # fullscreen→windowed toggle comes back at the wrong resolution.
        "win_w": int(SETTINGS.visual.window_width),
        "win_h": int(SETTINGS.visual.window_height),
        "fullscreen": bool(SETTINGS.visual.fullscreen),
        "cover_square": None,
        "cover_bg": None,
        "dominant_colors": [(40, 40, 60), (70, 50, 90), (30, 60, 80)],
        "current_cover_path": None,
        "font": None,
        "sub_font": None,
        "status_font": None,
        # Cross-fade snapshots — stored here so render-time blending
        # can access both old and new cover surfaces / colours.
        "old_cover_square": None,
        "old_cover_texture": None,
        "old_dominant_colors": [(40, 40, 60), (70, 50, 90), (30, 60, 80)],
        # --- Standby (pre-first-track) image state ----------------------
        # standby_raw: full-resolution alpha surface (loaded once).
        # standby_surf: scaled-to-screen version (rebuilt on resize).
        # standby_colors: dominant colours extracted from opaque pixels.
        # standby_fadeout: True during the first-track dissolve.
        "standby_path": "",
        "standby_raw": None,
        "standby_surf": None,
        "standby_colors": None,
        "standby_fadeout": False,
        # --- Per-frame performance caches ------------------------------
        # The flowing background is regenerated at ~20 fps (it morphs
        # with 60-100 s periods, so 60 fps is wasted work) and blitted
        # from a full-screen cache on the other frames. Large covers are
        # rotated on the frames in between so no single frame pays both
        # costs at 4K (smoothscale+rotozoom ≈ 25 ms). Text surfaces are
        # cached by content — they only change on track change.
        "frame_no": 0,
        "bg_cache": None,
        "cover_rot_cache": None,
        "cover_disp_cache": None,
        # Cached rotated covers during cross-fade so we don't pay the
        # rotozoom cost twice every frame at 4K.
        "old_cover_rot_cache": None,
        "new_cover_rot_cache": None,
        "old_cover_disp_cache": None,
        "new_cover_disp_cache": None,
        "text_cache": None,
        "text_cache_key": None,
    }

    def _reload_cover_assets(path: Optional[str], layout: Layout) -> None:
        """Re-render the cover square + blurred bg + dominant colours."""
        # Rotated-frame cache belongs to the previous artwork.
        rs["cover_rot_cache"] = None
        rs["cover_disp_cache"] = None
        if not path:
            rs["cover_square"] = None
            rs["cover_bg"] = None
            rs["dominant_colors"] = [(40, 40, 60), (70, 50, 90), (30, 60, 80)]
            return
        cover_size = layout.cover_rect.width
        square, raw = _load_cover(path, cover_size, gpu=rs.get("renderer") is not None)
        rs["cover_square"] = square
        # GPU path: upload the cover square as a texture once. The
        # renderer rotates/scales it on the GPU each frame — no CPU
        # rotozoom needed, even at 4K.
        if rs.get("renderer") is not None and square is not None:
            try:
                from pygame._sdl2 import Texture
                rs["cover_texture"] = Texture.from_surface(
                    rs["renderer"], square)
            except Exception as exc:
                log.warning("cover texture upload failed: %s", exc)
                rs["cover_texture"] = None
        else:
            rs["cover_texture"] = None
        # Extract dominant colours for the flowing background.
        if raw is not None:
            rs["dominant_colors"] = _extract_dominant_colors(raw, n=3)
            rs["cover_bg"] = _make_blurred_bg(
                raw, rs["screen_w"], rs["screen_h"],
                SETTINGS.visual.bg_blur_px,
            ) if SETTINGS.visual.bg_mode == "blur" else None
        else:
            rs["cover_bg"] = None

    def _reload_standby() -> None:
        """(Re)load the optional pre-recognition standby image.

        Shown centered (≤40 % of screen, alpha preserved) before the
        first track is recognised; its dominant colours drive the
        flowing background until the cover takes over.
        """
        path = str(SETTINGS.visual.standby_image or "")
        if not path or not Path(path).is_file():
            rs["standby_path"] = ""
            rs["standby_raw"] = None
            rs["standby_surf"] = None
            rs["standby_colors"] = None
            return
        if rs["standby_raw"] is None or rs["standby_path"] != path:
            raw, colors = _load_standby_raw(path)
            rs["standby_raw"] = raw
            rs["standby_colors"] = colors
            rs["standby_path"] = path
        if rs["standby_raw"] is not None:
            rs["standby_surf"] = _scale_standby(
                rs["standby_raw"], rs["screen_w"], rs["screen_h"])
        else:
            rs["standby_surf"] = None
        # While no track is displayed, let the image colours drive the bg.
        if not rs["current_cover_path"] and rs["standby_colors"]:
            rs["dominant_colors"] = list(rs["standby_colors"])

    # Clamp the requested display index to a monitor that actually
    # exists. Config defaults to 1 (2nd screen); on a single-monitor
    # machine that index is invalid — fullscreen set_mode would raise
    # and silently fall back, leaving the fullscreen flag desynced from
    # reality (window stuck at old size on maximize).
    try:
        _num_displays = pygame.display.get_num_displays()
    except Exception:
        _num_displays = 1
    eff_display = max(0, min(int(display_index), _num_displays - 1))
    if eff_display != display_index:
        log.info("Requested display %s unavailable (found %s) — using display %s",
                 display_index, _num_displays, eff_display)

    def _reinit_display() -> None:
        """Re-create the pygame display surface and re-derive assets.

        Called on startup and whenever the window is resized or toggled
        between windowed and fullscreen mode. Re-renders the cached cover
        square + blurred background at the new resolution.

        Fallback chain (each step only if the previous raised):
          1. requested mode with explicit display index
          2. same mode without display kwarg (old pygame / odd drivers)
          3. fullscreen without display kwarg on display 0
          4. windowed RESIZABLE — and crucially the fullscreen FLAG is
             forced back to False so later VIDEORESIZE events are handled
             (a stale True flag otherwise freezes the surface at the old
             size when the user maximizes the window).
        """
        vcfg = SETTINGS.visual
        want_fullscreen = bool(rs["fullscreen"])

        # Leaving windowed for fullscreen: remember the PHYSICAL window
        # size so exiting fullscreen restores exactly this geometry.
        if want_fullscreen:
            try:
                if rs.get("renderer") is not None:
                    pw, ph = rs["renderer"].window.size
                else:
                    pw, ph = pygame.display.get_window_size()
                if pw >= 200 and ph >= 150:
                    rs["win_w"], rs["win_h"] = int(pw), int(ph)
            except Exception:
                rs["win_w"] = int(rs["screen_w"])
                rs["win_h"] = int(rs["screen_h"])

        # Fullscreen the monitor the WINDOW is currently on — not a fixed
        # config index. With two monitors this prevents the fullscreen
        # surface from appearing on the other screen (where it looked
        # stuck/unrecoverable). Falls back to the startup display index.
        detected = _current_monitor_index()
        target_display = detected if detected is not None else eff_display

        def _try(size, flags, use_display):
            if use_display:
                return pygame.display.set_mode(
                    size, flags, display=target_display, vsync=1)
            return pygame.display.set_mode(size, flags, vsync=1)

        # GPU path runs first (creates its own video.Window). If it fails,
        # the software fallback below uses set_mode — so do NOT set_mode
        # unconditionally here, that created a duplicate window.
        screen = None

        rs["screen"] = None
        rs["screen_w"] = 0
        rs["screen_h"] = 0
        rs["renderer"] = None
        rs["cover_texture"] = None
        rs["bg_texture"] = None

        # --- SDL2 hardware renderer (GPU acceleration) ---
        # When enabled, create the window + renderer directly (no
        # set_mode — set_mode binds a software surface to the window and
        # then a Renderer cannot be created on it). All heavy work
        # (cover rotation, bg upscaling, compositing) runs on the GPU;
        # vsync locks to the monitor refresh (no tearing).
        if SETTINGS.visual.gpu_acceleration:
            try:
                from pygame._sdl2 import video
                # Destroy previous renderer + window + their textures
                # (re-init happens on resize / fullscreen toggle / startup
                # config messages). Without this each reinit leaked a whole
                # SDL window — that was the "two visualizer windows" bug.
                for tkey in ("cover_texture", "old_cover_texture",
                             "bg_texture"):
                    rs[tkey] = None
                if rs.get("sdl_window") is not None:
                    try:
                        rs["sdl_window"].destroy()
                    except Exception:
                        pass
                    rs["sdl_window"] = None
                rs["renderer"] = None
                win_title = f"VJVision 可视化输出  v{_version}"
                if want_fullscreen:
                    # Borderless fullscreen at the monitor's CURRENT native
                    # desktop resolution (no display-mode switch, no GPU
                    # upscaling, vsync works). Requires per-monitor DPI
                    # awareness (set above) or SDL falls back to 640x480.
                    window = video.Window(win_title, fullscreen_desktop=True)
                else:
                    window = video.Window(
                        win_title,
                        size=(rs["win_w"], rs["win_h"]),
                        resizable=True,
                    )
                rs["sdl_window"] = window
                # Force present-vsync. The vsync=1 flag below is only a
                # REQUEST: if SDL auto-selects a driver without vsync
                # support (or silently falls back to its software
                # driver), present() never blocks on the monitor
                # refresh and the picture tears — clock.tick(60) cannot
                # fix that. Pin a driver whose present path is
                # guaranteed tear-free: direct3d11 (flip-model; vsync
                # works in BOTH windowed and borderless-fullscreen),
                # then legacy direct3d, then SDL's default accelerated
                # driver. SDL_HINT_RENDER_DRIVER is read at
                # CreateRenderer time, so setting it per-attempt here is
                # enough; a failed attempt leaves no renderer bound and
                # the same window can be retried.
                renderer = None
                last_rend_exc = None
                active_drv = None
                for drv in (["direct3d11", "direct3d"]
                            if os.name == "nt" else []) + [None]:
                    if drv:
                        os.environ["SDL_HINT_RENDER_DRIVER"] = drv
                    else:
                        os.environ.pop("SDL_HINT_RENDER_DRIVER", None)
                    try:
                        renderer = video.Renderer(
                            window, accelerated=1, vsync=1)
                        active_drv = drv
                        break
                    except Exception as exc:  # driver missing / refused
                        last_rend_exc = exc
                        renderer = None
                if renderer is None:
                    raise last_rend_exc or RuntimeError(
                        "no accelerated renderer available")
                rs["renderer"] = renderer
                sw, sh = window.size
                rs["screen_w"], rs["screen_h"] = int(sw), int(sh)
                # On high-DPI displays the drawable surface has more
                # pixels than the window's point size. Pin the renderer's
                # logical coordinate space to the point size so our draws
                # fill the whole window (otherwise content renders only in
                # the top-left fraction and the rest is black).
                rs["renderer"].logical_size = (int(sw), int(sh))
                # Verify present-vsync empirically. pygame-ce 2.5.8's
                # Renderer exposes no driver/flags info, so measure the
                # behaviour itself: with vsync active, present() blocks
                # until the monitor's next refresh (~16.7 ms @60 Hz,
                # ~8.3 ms @120 Hz, ~4.2 ms @240 Hz); without vsync a
                # bare clear+present returns in well under a millisecond
                # and the picture tears. Checked ONCE per process — the
                # driver and vsync state are identical on later reinits
                # (resize/fullscreen), so drag-resizing isn't stalled.
                drv_label = active_drv or "auto"
                if not rs.get("vsync_checked"):
                    try:
                        rs["renderer"].draw_color = (0, 0, 0, 255)
                        n_probe = 10
                        t0 = time.perf_counter()
                        for _ in range(n_probe):
                            rs["renderer"].clear()
                            rs["renderer"].present()
                        per_frame = (time.perf_counter() - t0) / n_probe
                        # 3.5 ms threshold: even a 240 Hz refresh blocks
                        # 4.2 ms; an unsynced present is sub-millisecond.
                        vsync_on = per_frame >= 0.0035
                        rs["vsync_checked"] = True
                        rs["vsync_active"] = vsync_on
                    except Exception as exc:
                        log.warning("vsync probe failed: %s", exc)
                        vsync_on = False
                else:
                    vsync_on = bool(rs.get("vsync_active"))
                if vsync_on:
                    log.info(
                        "GPU renderer initialised: driver=%s vsync=ON "
                        "size=%dx%d", drv_label, int(sw), int(sh))
                else:
                    log.warning(
                        "GPU renderer initialised: driver=%s vsync=OFF "
                        "(present may tear) size=%dx%d",
                        drv_label, int(sw), int(sh))
                # Software fallback path below is skipped.
                screen = None
            except Exception as exc:
                log.warning("GPU renderer unavailable (%s) — falling back to software", exc)
                rs["renderer"] = None
                # fall through to set_mode software path

        # --- Software path (set_mode + surface blitting) ---
        if rs["renderer"] is None:
            screen = None
            last_exc = None
            if want_fullscreen:
                size, flags = (0, 0), pygame.FULLSCREEN
            else:
                size, flags = (rs["win_w"], rs["win_h"]), pygame.RESIZABLE
            for use_display in (True, False):
                try:
                    screen = _try(size, flags, use_display)
                    break
                except TypeError:
                    last_exc = None
                    continue
                except pygame.error as exc:
                    last_exc = exc
                    continue

            if screen is None and want_fullscreen:
                try:
                    screen = pygame.display.set_mode((0, 0), pygame.FULLSCREEN)
                except pygame.error as exc:
                    last_exc = exc

            if screen is None:
                screen = pygame.display.set_mode(
                    (rs["win_w"], rs["win_h"]), pygame.RESIZABLE)
                if want_fullscreen:
                    log.warning("Fullscreen unavailable (%s) — staying windowed",
                                last_exc)
                    rs["fullscreen"] = False
                    SETTINGS.visual.fullscreen = False
                    try:
                        state.status = "Fullscreen unavailable - windowed"
                        state.status_until = time.monotonic() + 4.0
                    except NameError:
                        pass

            rs["screen"] = screen
            rs["screen_w"], rs["screen_h"] = screen.get_size()

        # Windowed: the actual surface size is ground truth (SDL may
        # adjust the requested size on restore). Persist it into BOTH the
        # protected restore-size state and SETTINGS, so the next fullscreen
        # toggle always returns to a real windowed geometry.
        if not rs["fullscreen"]:
            actual_w, actual_h = rs["screen_w"], rs["screen_h"]
            if actual_w >= 200 and actual_h >= 150:
                rs["win_w"] = actual_w
                rs["win_h"] = actual_h
                SETTINGS.visual.window_width = actual_w
                SETTINGS.visual.window_height = actual_h

        # Unbind the IME from this window so F/F11/Esc hotkeys work even
        # while a Chinese IME is active (otherwise the IME swallows letter
        # keys during composition). Also stop text-input mode defensively.
        try:
            pygame.key.stop_text_input()
        except Exception:
            pass
        _disable_window_ime()

        # Font sizes scale modestly with screen height so text stays legible.
        # Respect the user's font choice; fall back to auto CJK if unset.
        font_name = _pick_font(vcfg.font_name)
        title_pt = max(22, int(rs["screen_h"] * 0.045))
        sub_pt = max(14, int(rs["screen_h"] * 0.028))
        rs["font"] = pygame.font.SysFont(font_name, title_pt, bold=True)
        rs["sub_font"] = pygame.font.SysFont(font_name, sub_pt)
        rs["status_font"] = pygame.font.SysFont(font_name, 16)

        # Re-derive cover assets at the new layout's size.
        layout = _compute_layout(rs["screen_w"], rs["screen_h"])
        _reload_cover_assets(rs["current_cover_path"], layout)
        # Re-scale the standby image for the new resolution too.
        _reload_standby()
        log.info("Visualizer display=%s size=%sx%s fullscreen=%s gpu=%s",
                 display_index, rs["screen_w"], rs["screen_h"],
                 rs["fullscreen"], rs["renderer"] is not None)

    _reinit_display()

    clock = pygame.time.Clock()
    vcfg = SETTINGS.visual

    state = VisualState(
        style=vcfg.spectrum_style,
        rotation_speed=vcfg.rotation_speed,
        beat_reactive=vcfg.beat_reactive,
    )

    def _get_text_surf(layout) -> "pygame.Surface":
        """Rendered track-info block, cached by content + size + font.

        Font rendering (especially large bold CJK) is noticeably costly
        and the text only changes on track change, so render the three
        lines into an SRCALPHA surface once and reuse it. The caller
        adjusts ``set_alpha`` on the returned (cached) surface for
        fades; the normal path resets it to 255.
        """
        tw, th = layout.text_rect.width, layout.text_rect.height
        key = (state.title, state.artist, state.album,
               id(rs["font"]), id(rs["sub_font"]), tw, th)
        if rs["text_cache"] is None or rs["text_cache_key"] != key:
            surf = pygame.Surface((max(1, tw), max(1, th)), pygame.SRCALPHA)
            _draw_text(
                surf, state.title, state.artist, state.album,
                pygame.Rect(0, 0, tw, th), rs["font"], rs["sub_font"],
            )
            rs["text_cache"] = surf
            rs["text_cache_key"] = key
        return rs["text_cache"]

    def _apply_demo(on: bool) -> None:
        """Toggle demo mode on/off (driven by the control panel, not a
        hotkey).

        Demo mode displays a rotating random album cover so GPU
        rotation / rendering can be verified without any audio input.
        Turning it off returns to the standby screen.
        """
        rs["demo_mode"] = bool(on)
        if on:
            import glob as _glob
            import random as _random
            _covers = _glob.glob(os.path.join(
                os.path.dirname(os.path.dirname(__file__)),
                "cache", "covers", "*.*"))
            demo_cover = _random.choice(_covers) if _covers else None
            state.old_title = state.title
            state.old_artist = state.artist
            state.old_album = state.album
            state.old_cover_path = state.cover_path
            state.title = "GPU Demo"
            state.artist = "Hardware Acceleration Test"
            state.album = "Demo mode"
            state.cover_path = demo_cover
            state.fade_active = True
            state.fade_progress = 0.0
            rs["old_dominant_colors"] = list(rs["dominant_colors"])
            rs["standby_fadeout"] = bool(rs.get("standby_surf"))
            if demo_cover and demo_cover != rs["current_cover_path"]:
                rs["current_cover_path"] = demo_cover
                layout = _compute_layout(
                    rs["screen_w"], rs["screen_h"])
                _reload_cover_assets(demo_cover, layout)
            log.info("Demo mode ON (cover=%s)", demo_cover)
        else:
            state.title = ""
            state.artist = ""
            state.album = ""
            state.cover_path = None
            state.fade_active = False
            state.fade_progress = 0.0
            rs["current_cover_path"] = None
            rs["standby_fadeout"] = False
            rs["dominant_colors"] = (
                list(rs["standby_colors"]) if rs.get("standby_colors")
                else [(40, 40, 60), (70, 50, 90), (30, 60, 80)])
            state.angle = 0.0
            log.info("Demo mode OFF")

    running = True
    while running:
        # --- drain all queued messages -------------------------------
        while True:
            try:
                msg = queue.get_nowait()
            except QueueEmpty:
                break
            mtype = msg.get("type")
            if mtype == "quit":
                running = False
            elif mtype == "spectrum":
                raw = np.asarray(msg["bins"], dtype=np.float32)
                # Drop bin #0 — it straddles DC offset (0 Hz) which always
                # reads high from soundcard ground-loop pickup.  Real audio
                # can't exist at 0 Hz, so removing the whole slot (not just
                # zeroing it) keeps the bar grid evenly spaced.
                state.bins = raw[1:] if raw.size > 1 else raw
                state.peak = float(msg.get("peak", 0.0))
            elif mtype == "track":
                # Cross-fade transition: if we already have visible content,
                # snapshot it and fade old→new over ~0.6s instead of cutting
                # instantly. Skipped if this is the very first track shown
                # (no previous content to fade from).
                new_title = msg.get("title", "") or ""
                new_artist = msg.get("artist", "") or ""
                new_album = msg.get("album", "") or ""
                new_cover = msg.get("cover")
                is_resync = bool(msg.get("_resync"))
                is_tentative = bool(msg.get("tentative", False))

                has_previous = bool(state.cover_path or state.title)
                is_same_track = (
                    new_cover == rs["current_cover_path"]
                    and new_title == state.title
                    and new_artist == state.artist
                )

                # viz was just restarted and the matcher is pushing the
                # current track back — skip the fade so we land on the
                # right song instantly instead of lingering on standby.
                if is_resync:
                    state.fade_active = False
                    state.fade_progress = 0.0
                    rs["standby_fadeout"] = False
                    rs["cover_square"] = None    # force re-load cover
                elif has_previous and not is_same_track:
                    # Snapshot current state → "old" slots for cross-fade.
                    state.old_title = state.title
                    state.old_artist = state.artist
                    state.old_album = state.album
                    state.old_cover_path = state.cover_path
                    state.old_colors = tuple(rs["dominant_colors"])
                    state.fade_active = True
                    state.fade_progress = 0.0
                    # Keep fading bg colors and old cover surface too.
                    rs["old_dominant_colors"] = list(rs["dominant_colors"])
                    rs["old_cover_square"] = rs["cover_square"]
                    rs["old_cover_texture"] = rs.get("cover_texture")
                    # Rotation caches are for the current artwork; reset
                    # so the fade re-renders from the new angle.
                    rs["old_cover_rot_cache"] = None
                    rs["new_cover_rot_cache"] = None
                    rs["old_cover_disp_cache"] = None
                    rs["new_cover_disp_cache"] = None
                elif not has_previous and rs.get("standby_surf") is not None:
                    # Very first track while the standby image is up —
                    # dissolve out of the standby screen: the image fades
                    # out and its bg colours morph into the new cover
                    # palette over ~0.6 s instead of a hard cut.
                    state.fade_active = True
                    state.fade_progress = 0.0
                    rs["standby_fadeout"] = True
                    rs["old_dominant_colors"] = list(rs["dominant_colors"])
                elif not has_previous:
                    # Very first track AND no standby image — still do a
                    # soft fade-in from the default flowing background so
                    # the cover + text ease in instead of slamming onto
                    # a black screen.
                    state.fade_active = True
                    state.fade_progress = 0.0
                    rs["old_dominant_colors"] = list(rs["dominant_colors"])

                state.title = new_title
                state.artist = new_artist
                state.album = new_album
                state.cover_path = new_cover
                state.tentative = is_tentative

                if new_cover != rs["current_cover_path"]:
                    rs["current_cover_path"] = new_cover
                    layout = _compute_layout(rs["screen_w"], rs["screen_h"])
                    _reload_cover_assets(new_cover, layout)

            elif mtype == "viz_reset":
                """Soft reset: clear all on-screen track content."""
                state.title = ""
                state.artist = ""
                state.album = ""
                state.cover_path = None
                state.fade_active = False
                state.fade_progress = 0.0
                state.status = "Reset"
                state.status_until = time.monotonic() + 2.0
                # Clear cover assets so we show the placeholder circle.
                rs["current_cover_path"] = None
                rs["cover_square"] = None
                rs["cover_bg"] = None
                rs["standby_fadeout"] = False
                # Back to the standby screen: restore the standby image
                # palette (or the default dark palette if no image).
                rs["dominant_colors"] = (
                    list(rs["standby_colors"]) if rs.get("standby_colors")
                    else [(40, 40, 60), (70, 50, 90), (30, 60, 80)])
                # Reset rotation angle too so it starts clean.
                state.angle = 0.0
            elif mtype == "settings":
                if "style" in msg and msg["style"] in SPECTRUM_STYLES:
                    state.style = msg["style"]
                if "rotation_speed" in msg:
                    state.rotation_speed = float(msg["rotation_speed"])
                if "beat_reactive" in msg:
                    state.beat_reactive = bool(msg["beat_reactive"])
                if "bg_mode" in msg:
                    SETTINGS.visual.bg_mode = str(msg["bg_mode"])
                    # Drop cached backgrounds and (re)build the blurred
                    # cover surface for the now-selected mode.
                    rs["bg_cache"] = None
                    layout = _compute_layout(rs["screen_w"], rs["screen_h"])
                    _reload_cover_assets(rs["current_cover_path"], layout)
                if "fullscreen" in msg:
                    new_fs = bool(msg["fullscreen"])
                    if new_fs != rs["fullscreen"]:
                        rs["fullscreen"] = new_fs
                        SETTINGS.visual.fullscreen = new_fs
                        _reinit_display()
                if "font_name" in msg:
                    new_font = str(msg["font_name"])
                    if new_font != SETTINGS.visual.font_name:
                        SETTINGS.visual.font_name = new_font
                        _reinit_display()
                if "standby_image" in msg:
                    new_img = str(msg.get("standby_image") or "")
                    if new_img != str(SETTINGS.visual.standby_image or ""):
                        SETTINGS.visual.standby_image = new_img
                        _reload_standby()
                if "demo_mode" in msg:
                    _apply_demo(bool(msg["demo_mode"]))
            elif mtype == "status":
                state.status = msg.get("text", "")
                state.status_until = time.monotonic() + 5.0

        # --- pygame events -----------------------------------------
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                # Esc exits FULLSCREEN first (media-player behaviour);
                # it only quits the app when already windowed.
                if rs["fullscreen"]:
                    rs["fullscreen"] = False
                    SETTINGS.visual.fullscreen = False
                    _reinit_display()
                else:
                    running = False
            elif event.type == pygame.KEYDOWN and event.key in (
                pygame.K_F11, pygame.K_f
            ):
                # Toggle fullscreen (also reflected back to the debug UI via
                # the shared SETTINGS, but we update locally right away).
                rs["fullscreen"] = not rs["fullscreen"]
                SETTINGS.visual.fullscreen = rs["fullscreen"]
                _reinit_display()
            elif event.type == pygame.VIDEORESIZE:
                # Fullscreen transitions post VIDEORESIZE events carrying
                # the monitor's full desktop resolution.  Ignore them
                # completely — they must not overwrite the windowed size
                # we restore to (that bug caused the 2nd fullscreen→
                # windowed toggle to come back at the wrong resolution).
                if rs["fullscreen"]:
                    continue
                w, h = int(event.w), int(event.h)
                # De-duplicate: SDL posts coalesced/no-op resize events
                # around mode toggles; only react to a REAL size change.
                if abs(w - rs["screen_w"]) <= 4 and \
                   abs(h - rs["screen_h"]) <= 4:
                    continue
                # User actually resized the windowed surface.
                rs["win_w"], rs["win_h"] = w, h
                SETTINGS.visual.window_width = w
                SETTINGS.visual.window_height = h
                _reinit_display()

        # --- update animation --------------------------------------
        dt = clock.tick(60) / 1000.0
        rs["frame_no"] += 1

        # FPS logging (every ~2s) to diagnose performance issues.
        if rs["frame_no"] % 120 == 1:
            log.info("FPS=%.1f frame_time=%.1fms render=%dx%s",
                     clock.get_fps(), dt * 1000,
                     rs["screen_w"], rs["screen_h"])

        # --- parent liveness check (every ~0.5s) --------------------
        # If the main process died (e.g. user closed the console window)
        # the shutdown hooks in main.py never ran, so we self-terminate
        # instead of leaving an orphan pygame window on screen.
        if rs["frame_no"] % 30 == 0:
            if not _is_process_alive(_parent_pid):
                log.info(
                    "Parent process (pid=%d) gone — exiting visualizer.",
                    _parent_pid,
                )
                running = False
        speed = state.rotation_speed
        if state.beat_reactive and state.peak > 0:
            speed *= (1.0 + min(2.0, state.peak * 3.0))
        state.angle = (state.angle + speed * 360.0 * dt) % 360.0

        # --- cross-fade progress ---
        # Advance fade_progress toward 1.0 over the fade duration.
        # The standby→first-track dissolve is deliberately slower
        # (~1.5 s, a gentle reveal) while normal track changes stay a
        # snappy 0.6 s. Once done, snap to end state and clean up.
        if state.fade_active:
            # Standby reveal spans: image fade-out (1.5 s) + gap (0.5 s)
            # + cover/text fade-in (1.5 s). Normal track changes use the
            # snappy 0.6 s cross-fade.
            fade_dur = (
                STANDBY_FADE_DURATION * 2.0 + STANDBY_GAP
                if rs.get("standby_fadeout") else state.fade_duration)
            state.fade_progress = min(
                1.0, state.fade_progress + dt / fade_dur
            )
            if state.fade_progress >= 1.0:
                state.fade_active = False
                # Clean up old cover surface so GC can free it.
                rs["old_cover_square"] = None
                # Standby dissolve finished — image no longer drawn.
                rs["standby_fadeout"] = False

        # --- draw --------------------------------------------------
        screen = rs["screen"]
        screen_w = rs["screen_w"]
        screen_h = rs["screen_h"]
        fade_p = state.fade_progress if state.fade_active else 0.0

        # Recompute layout every frame - cheap, and survives any resize /
        # fullscreen toggle without per-asset invalidation.
        layout = _compute_layout(screen_w, screen_h)

        # Standby = before the first track is recognised: show ONLY the
        # flowing background plus an optional centered standby image (no
        # cover, no spectrum, no text). ``fade_from_standby`` marks the
        # ~0.6 s dissolve once the first track arrives.
        standby = (state.title == "")
        fade_from_standby = bool(rs.get("standby_fadeout") and state.fade_active)

        # Two-stage reveal timing (only during the standby dissolve):
        #   t in [0, 1.5)      image fades out (img_fade 1 → 0)
        #   t in [1.5, 2.0)    gap — flowing background only
        #   t in [2.0, 3.5]    cover + text fade in (content_p 0 → 1)
        # ``img_fade`` multiplies the standby image alpha;
        # ``content_p`` drives cover / text / bg-colour reveal.
        if fade_from_standby:
            _total = STANDBY_FADE_DURATION * 2.0 + STANDBY_GAP
            _t = fade_p * _total
            img_fade = max(0.0, 1.0 - _t / STANDBY_FADE_DURATION)
            content_p = min(1.0, max(
                0.0,
                (_t - STANDBY_FADE_DURATION - STANDBY_GAP)
                / STANDBY_FADE_DURATION))
        else:
            img_fade = 1.0
            content_p = 1.0

        gpu = rs.get("renderer") is not None

        # --- Background ---
        # During cross-fade, mix old dominant colours with new ones so the
        # flowing background smoothly shifts between track-specific palettes
        # instead of jumping. Very noticeable on fast track changes.
        if SETTINGS.visual.bg_mode == "flow":
            t_now = time.monotonic()
            energy = 0.0
            if len(state.bins) > 0:
                energy = float(np.mean(state.bins[:max(1, len(state.bins) // 4)]))
            if state.fade_active:
                # Linearly interpolate each colour channel between old and new.
                new_colors = rs["dominant_colors"]
                old_colors = rs["old_dominant_colors"] or new_colors
                # During the standby reveal the palette morphs together
                # with the incoming cover (content_p); normal track
                # changes cross-fade at the overall fade progress.
                mix_p = content_p if rs.get("standby_fadeout") else fade_p
                mixed_colors = []
                # Pad shorter list so we don't crash on length mismatch.
                n = max(len(old_colors), len(new_colors))
                for i in range(n):
                    oc = old_colors[i % len(old_colors)]
                    nc = new_colors[i % len(new_colors)]
                    mixed_colors.append(tuple(
                        int(oc[c] * (1 - mix_p) + nc[c] * mix_p)
                        for c in range(3)
                    ))
                bg_colors = mixed_colors
            else:
                bg_colors = rs["dominant_colors"]
            # The field morphs with 60-100 s periods, so regenerating
            # the (expensive) full-screen surface every frame is wasted
            # work — 14 ms/frame at 4K. Regenerate at ~20 fps and blit
            # the cached surface on the other frames; the large cover
            # rotation is scheduled on different frames (see cover
            # section) so no single frame pays both costs.
            cache_ok = (
                rs.get("bg_texture") is not None
            ) if gpu else (
                rs["bg_cache"] is not None
                and rs["bg_cache"].get_size() == (screen_w, screen_h)
            )
            if not cache_ok or rs["frame_no"] % 3 == 0:
                rs["bg_cache"] = _make_flowing_bg(
                    bg_colors, screen_w, screen_h, t_now, energy, gpu=gpu,
                )
                if gpu:
                    # Upload the small wave-field as a GPU texture; the
                    # renderer scales it to full screen for free.
                    try:
                        from pygame._sdl2 import Texture
                        # Old texture is GC'd when overwritten.
                        rs["bg_texture"] = Texture.from_surface(
                            rs["renderer"], rs["bg_cache"])
                    except Exception as exc:
                        log.warning("bg texture upload failed: %s", exc)
            if gpu:
                rs["renderer"].clear()
                if rs.get("bg_texture") is not None:
                    rs["bg_texture"].draw(dstrect=(0, 0, screen_w, screen_h))
            else:
                screen.blit(rs["bg_cache"], (0, 0))
        elif rs["cover_bg"] is not None:
            rs["bg_cache"] = None
            if gpu:
                try:
                    from pygame._sdl2 import Texture
                    rs["bg_texture"] = Texture.from_surface(
                        rs["renderer"], rs["cover_bg"])
                except Exception:
                    rs["bg_texture"] = None
                rs["renderer"].clear()
                if rs["bg_texture"] is not None:
                    rs["bg_texture"].draw(dstrect=(0, 0, screen_w, screen_h))
            else:
                screen.blit(rs["cover_bg"], (0, 0))
        else:
            rs["bg_cache"] = None
            if gpu:
                rs["renderer"].draw_color = (20, 20, 30, 255)
                rs["renderer"].clear()
            else:
                screen.fill((20, 20, 30))

        # --- Cover (cross-fade during track changes; hidden in standby) ---
        if standby or (fade_from_standby and content_p <= 0.0):
            pass   # standby screen / fade-out + gap: no cover yet
        elif state.fade_active and rs["old_cover_square"] is not None:
            old_alpha = int((1.0 - fade_p) * 255)
            new_alpha = int(fade_p * 255)
            # SDL2 origin is relative to the dstrect's top-left corner,
            # NOT absolute screen coords — use half the cover size.
            rot_origin = (layout.cover_rect.width // 2,
                          layout.cover_rect.height // 2)
            cover_rect = layout.cover_rect

            if gpu and rs.get("old_cover_texture") is not None:
                # GPU: draw both covers with hardware rotation + alpha.
                old_tex = rs["old_cover_texture"]
                old_tex.alpha = old_alpha
                old_tex.draw(dstrect=cover_rect, angle=state.angle,
                             origin=rot_origin)
                new_tex = rs.get("cover_texture")
                if new_tex is not None:
                    new_tex.alpha = new_alpha
                    new_tex.draw(dstrect=cover_rect, angle=state.angle,
                                 origin=rot_origin)
            else:
                # CPU cross-fade (kept for the software fallback path).
                cover_px = int(rs["old_cover_square"].get_width())
                display_scale = layout.cover_rect.width / cover_px
                frame_mod = rs["frame_no"] % 3
                rotate_old = cover_px <= 640 or frame_mod == 1
                rotate_new = cover_px <= 640 or frame_mod == 2
                if rotate_old or rs["old_cover_rot_cache"] is None:
                    rs["old_cover_rot_cache"] = pygame.transform.rotozoom(
                        rs["old_cover_square"], state.angle, 1.0,
                    )
                    if display_scale != 1.0:
                        _r = rs["old_cover_rot_cache"]
                        rs["old_cover_disp_cache"] = pygame.transform.smoothscale(
                            _r, (int(_r.get_width() * display_scale),
                                 int(_r.get_height() * display_scale)))
                    else:
                        rs["old_cover_disp_cache"] = None
                if rotate_new or rs["new_cover_rot_cache"] is None:
                    rs["new_cover_rot_cache"] = (
                        pygame.transform.rotozoom(
                            rs["cover_square"], state.angle, 1.0,
                        ) if rs["cover_square"] is not None else None
                    )
                    if display_scale != 1.0 and rs["new_cover_rot_cache"] is not None:
                        _r = rs["new_cover_rot_cache"]
                        rs["new_cover_disp_cache"] = pygame.transform.smoothscale(
                            _r, (int(_r.get_width() * display_scale),
                                 int(_r.get_height() * display_scale)))
                    else:
                        rs["new_cover_disp_cache"] = None
                old_rot = (rs["old_cover_disp_cache"]
                           if rs["old_cover_disp_cache"] is not None
                           else rs["old_cover_rot_cache"])
                new_rot = (rs["new_cover_disp_cache"]
                           if rs["new_cover_disp_cache"] is not None
                           else rs["new_cover_rot_cache"])
                old_rot.set_alpha(old_alpha)
                screen.blit(old_rot, old_rot.get_rect(center=center).topleft)
                if new_rot is not None:
                    new_rot.set_alpha(new_alpha)
                    screen.blit(new_rot, new_rot.get_rect(center=center).topleft)
                rs["old_cover_rot_cache"].set_alpha(255)
                if rs["new_cover_rot_cache"] is not None:
                    rs["new_cover_rot_cache"].set_alpha(255)
        elif rs["cover_square"] is not None:
            if gpu and rs.get("cover_texture") is not None:
                # GPU: single draw call with hardware rotation. The angle
                # advances every frame and the GPU rotates the texture for
                # free — no CPU rotozoom, no smoothscale, at any res.
                tex = rs["cover_texture"]
                if fade_from_standby:
                    tex.alpha = int(content_p * 255)
                else:
                    tex.alpha = 255
                tex.draw(dstrect=layout.cover_rect, angle=state.angle,
                         origin=(layout.cover_rect.width // 2,
                                 layout.cover_rect.height // 2))
            else:
                # The cover square is rendered at a capped internal
                # resolution (MAX_COVER_RENDER_PX) so rotozoom stays fast.
                cover_px = int(rs["cover_square"].get_width())
                display_scale = layout.cover_rect.width / cover_px
                rotate_now = cover_px <= 640 or rs["frame_no"] % 3 == 1
                if rotate_now or rs["cover_rot_cache"] is None:
                    rs["cover_rot_cache"] = pygame.transform.rotozoom(
                        rs["cover_square"], state.angle, 1.0
                    )
                    if display_scale != 1.0:
                        _rot = rs["cover_rot_cache"]
                        dw = max(1, int(_rot.get_width() * display_scale))
                        dh = max(1, int(_rot.get_height() * display_scale))
                        rs["cover_disp_cache"] = pygame.transform.smoothscale(
                            _rot, (dw, dh))
                    else:
                        rs["cover_disp_cache"] = None
                rotated = (
                    rs["cover_disp_cache"]
                    if rs["cover_disp_cache"] is not None
                    else rs["cover_rot_cache"]
                )
                if fade_from_standby:
                    rotated.set_alpha(int(content_p * 255))
                else:
                    rotated.set_alpha(255)
                rect = rotated.get_rect(center=layout.cover_rect.center)
                screen.blit(rotated, rect.topleft)
        elif not fade_from_standby:
            r = layout.cover_rect.width // 2
            if gpu:
                import math
                cx, cy = layout.cover_rect.center
                n = 32
                rs["renderer"].draw_color = PALETTE_PLACEHOLDER + (255,)
                for i in range(n):
                    a0 = 2 * math.pi * i / n
                    a1 = 2 * math.pi * (i + 1) / n
                    p0 = (cx + int(r * math.cos(a0)),
                          cy + int(r * math.sin(a0)))
                    p1 = (cx + int(r * math.cos(a1)),
                          cy + int(r * math.sin(a1)))
                    rs["renderer"].draw_line(p0, p1)
            else:
                pygame.draw.circle(
                    screen, PALETTE_PLACEHOLDER,
                    layout.cover_rect.center, r, 4,
                )

        # --- Standby image (centered; fades out on first track) ---
        standby_img = rs.get("standby_surf")
        if standby_img is not None and img_fade > 0.0 and (
                standby or fade_from_standby):
            standby_img.set_alpha(int(img_fade * 255))
            img_rect = standby_img.get_rect(
                center=(screen_w // 2, screen_h // 2))
            if gpu:
                from pygame._sdl2 import Texture
                sb_tex = Texture.from_surface(rs["renderer"], standby_img)
                sb_tex.alpha = int(img_fade * 255)
                sb_tex.draw(dstrect=img_rect)
            else:
                screen.blit(standby_img, img_rect.topleft)
            standby_img.set_alpha(255)

        # --- Spectrum (hidden on the standby screen and during the
        # image fade-out + gap; appears with the cover/text reveal) ---
        if not standby and not (fade_from_standby and content_p <= 0.0):
            if gpu:
                # Render spectrum to a temp TRANSPARENT surface, then
                # upload as a texture and alpha-blend it over the flowing
                # background. (An opaque surface here would paint the whole
                # spectrum panel black and hide the background.)
                sp = layout.spectrum_rect
                spec_surf = pygame.Surface((sp.width, sp.height),
                                           pygame.SRCALPHA)
                if state.style == "bar":
                    if state.peak_holds.shape[0] != state.bins.shape[0]:
                        state.peak_holds = np.zeros_like(state.bins)
                    if state.peak_timers.shape[0] != state.bins.shape[0]:
                        state.peak_timers = np.zeros(state.bins.shape[0], dtype=np.int32)
                    _draw_bar(spec_surf, state.bins,
                              pygame.Rect(0, 0, sp.width, sp.height),
                              peak_holds=state.peak_holds,
                              peak_timers=state.peak_timers)
                elif state.style == "wave":
                    _draw_wave(spec_surf, state.bins,
                               pygame.Rect(0, 0, sp.width, sp.height))
                elif state.style == "mirror":
                    _draw_mirror(spec_surf, state.bins,
                                 pygame.Rect(0, 0, sp.width, sp.height))
                from pygame._sdl2 import Texture
                spec_tex = Texture.from_surface(rs["renderer"], spec_surf)
                spec_tex.draw(dstrect=sp)
            else:
                if state.style == "bar":
                    if state.peak_holds.shape[0] != state.bins.shape[0]:
                        state.peak_holds = np.zeros_like(state.bins)
                    if state.peak_timers.shape[0] != state.bins.shape[0]:
                        state.peak_timers = np.zeros(state.bins.shape[0], dtype=np.int32)
                    _draw_bar(screen, state.bins, layout.spectrum_rect,
                              peak_holds=state.peak_holds,
                              peak_timers=state.peak_timers)
                elif state.style == "wave":
                    _draw_wave(screen, state.bins, layout.spectrum_rect)
                elif state.style == "mirror":
                    _draw_mirror(screen, state.bins, layout.spectrum_rect)

        # --- Info text (cross-fade during track changes; hidden in standby) ---
        def _draw_text_surf(surf, alpha):
            """Blit a text surface (GPU texture or CPU blit)."""
            if gpu:
                from pygame._sdl2 import Texture
                tt = Texture.from_surface(rs["renderer"], surf)
                tt.alpha = alpha
                tt.draw(dstrect=layout.text_rect)
            else:
                surf.set_alpha(alpha)
                screen.blit(surf, layout.text_rect.topleft)

        if standby or (fade_from_standby and content_p <= 0.0):
            pass
        elif fade_from_standby:
            text_surf = _get_text_surf(layout)
            _draw_text_surf(text_surf, int(content_p * 255))
        elif state.fade_active and (state.old_title or state.old_artist or state.old_album):
            text_w = layout.text_rect.width
            text_h = layout.text_rect.height
            old_text_surf = pygame.Surface((text_w, text_h), pygame.SRCALPHA)
            _draw_text(
                old_text_surf,
                state.old_title, state.old_artist, state.old_album,
                pygame.Rect(0, 0, text_w, text_h),
                rs["font"], rs["sub_font"],
            )
            new_text_surf = _get_text_surf(layout)
            _draw_text_surf(old_text_surf, int((1 - fade_p) * 255))
            _draw_text_surf(new_text_surf, int(fade_p * 255))
        else:
            text_surf = _get_text_surf(layout)
            if state.tentative:
                pulse = 0.45 + 0.55 * (0.5 + 0.5 * math.sin(time.time() * 3.9))
                alpha = int(pulse * 255)
            else:
                alpha = 255
            _draw_text_surf(text_surf, alpha)

        # Note: the top-left recognition status overlay ("Matching…",
        # "No match", "Listening…", "Mixing…") was removed at the user's
        # request — it's noise on the performance screen.  Operational
        # messages (e.g. fullscreen fallback) are still written to the log.

        if gpu:
            rs["renderer"].present()
        else:
            pygame.display.flip()

    pygame.quit()
