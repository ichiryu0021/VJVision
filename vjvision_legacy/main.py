"""Entry point for VJVision.

Spawns three actors:
    1. Visualizer process  (pygame on the 2nd display, managed by VisualizerManager)
    2. Matcher thread      (audio capture + dejavu + metadata, in-process)
    3. Debug UI            (CustomTkinter on the 1st display, in main thread)

Queues:
    cmd_queue   UI -> Matcher    (start, stop, settings, device, ...)
    viz_queue   Matcher -> Viz   (spectrum, track, settings, status, quit)
    ui_queue    Matcher -> UI    (log, track, index_progress, ...)
"""
from __future__ import annotations

import logging
import multiprocessing as mp
import queue as _queue
import sys
import threading
import time
from logging.handlers import RotatingFileHandler
from typing import Optional

from vjvision import config as cfg
from vjvision.debug_ui import DebugUI
from vjvision.matcher import MatcherThread
from vjvision.visualizer import run as run_visualizer

log = logging.getLogger("vj")


# --- Windows: suppress black console windows for ALL spawned child processes ---
# Must run at module import time (before freeze_support / any Process spawn)
# so it covers the visualizer process too, not just indexing workers.
# We monkeypatch ``_winapi.CreateProcess`` to OR in CREATE_NO_WINDOW.
if sys.platform == "win32":
    try:
        import _winapi as _winapi_mod
        _CREATE_NO_WINDOW = 0x08000000
        _orig_create_process = _winapi_mod.CreateProcess

        def _no_window_create_process(app_name, cmd_line, proc_attrs,
                                       thread_attrs, inherit_handles,
                                       creation_flags, env, cwd, startup_info):
            return _orig_create_process(
                app_name, cmd_line, proc_attrs, thread_attrs,
                inherit_handles, creation_flags | _CREATE_NO_WINDOW,
                env, cwd, startup_info,
            )

        _winapi_mod.CreateProcess = _no_window_create_process
    except Exception:
        pass


class VisualizerManager:
    """Manages the lifecycle of the pygame visualizer child process.

    The visualizer is a separate spawn-ed process because pygame/SDL
    cannot be fork-ed safely on Windows. This class wraps creation,
    restart (kill + respawn), and soft reset (send 'reset' message)
    behind a simple API that both the DebugUI buttons and the matcher
    thread can call.

    Thread-safety: restart()/reset()/start() can be invoked concurrently
    from the UI thread's restart worker, the matcher thread, and the
    matcher's "VizRestart" worker. All lifecycle operations are guarded
    by a reentrant lock (restart calls stop + start).
    """

    def __init__(
        self,
        mp_ctx: mp.context.BaseContext,
        viz_queue: "_queue.Queue",
        display_index: int,
    ) -> None:
        self._ctx = mp_ctx
        self._viz_queue = viz_queue
        self._display_index = display_index
        self.proc: Optional[mp.Process] = None
        self._last_start_ts: float = 0.0
        self._lock = threading.RLock()

    @property
    def alive(self) -> bool:
        return self.proc is not None and self.proc.is_alive()

    def _send_control(self, msg: dict) -> bool:
        """Best-effort send of a control command to the visualizer.

        A full queue means the visualizer process is stalled: drop a
        batch of stale frames to make room for the command (spectrum /
        status frames are disposable). Also tolerates a dead child —
        a broken pipe raises OSError, which we log instead of crashing
        the caller.
        """
        mtype = msg.get("type", "?")
        try:
            self._viz_queue.put_nowait(msg)
            return True
        except _queue.Full:
            dropped = 0
            for _ in range(64):
                try:
                    self._viz_queue.get_nowait()
                    dropped += 1
                except _queue.Empty:
                    break
            try:
                self._viz_queue.put_nowait(msg)
                log.warning("viz_queue full — dropped %d stale msg(s) to send %s",
                            dropped, mtype)
                return True
            except (_queue.Full, OSError) as exc:
                log.warning("viz control send '%s' failed after drain: %s",
                            mtype, exc)
                return False
        except OSError as exc:   # BrokenPipeError / closed pipe — child gone
            log.warning("viz control send '%s' failed (pipe): %s", mtype, exc)
            return False

    def _drain_queue(self) -> int:
        """Remove every message currently buffered in the viz queue.

        Returns the number of messages dropped. A stale ``quit`` (or any
        other control message) left in the queue pipe by a previous
        process would be drained by the FRESH process on its very first
        frame — a leftover ``quit`` then makes the new window exit
        instantly (the "restart flashes and dies" bug that happened
        after closing the visualizer window manually).
        """
        drained = 0
        try:
            while True:
                self._viz_queue.get_nowait()
                drained += 1
        except _queue.Empty:
            pass
        except OSError:
            pass
        return drained

    def start(self) -> None:
        """Spawn the visualizer process. Safe to call even if one is
        already running (it will be terminated first)."""
        with self._lock:
            if self.alive:
                log.info("Visualizer already running — restarting...")
                self.stop()
            # Purge messages the previous process never consumed so the
            # new one starts from a clean queue (see _drain_queue). Two
            # passes: mp.Queue flushes puts into the pipe from a feeder
            # thread asynchronously, so re-drain after a tiny wait.
            drained = self._drain_queue()
            time.sleep(0.05)
            drained += self._drain_queue()
            if drained:
                log.info("Drained %d stale viz message(s) before start",
                         drained)
            log.info("Starting visualizer on display %d", self._display_index)
            self.proc = self._ctx.Process(
                target=run_visualizer,
                args=(self._viz_queue, self._display_index),
                name="Visualizer",
                daemon=True,
            )
            self.proc.start()
            self._last_start_ts = time.monotonic()

    def stop(self, timeout: float = 3.0) -> None:
        """Terminate the visualizer process gracefully (quit message
        first, then SIGKILL if needed). Never raises — shutdown paths
        must complete even if the child already died."""
        with self._lock:
            if self.proc is None:
                return
            # Only send quit to a LIVE child. If it already exited (user
            # closed the window, SDL crash), the message would stay in
            # the queue pipe with no consumer; the next spawned process
            # would then read that stale quit on its first frame and exit
            # instantly (the restart-flash bug). start() drains the queue
            # as a second line of defence.
            if self.proc.is_alive():
                self._send_control({"type": "quit"})
            try:
                self.proc.join(timeout=timeout)
                if self.proc.is_alive():
                    log.warning("Visualizer did not exit in %.1fs — terminating",
                                timeout)
                    self.proc.terminate()
                    self.proc.join(timeout=1.0)
                    if self.proc.is_alive():
                        # Last resort: kill() (SIGKILL-equivalent on Win).
                        try:
                            self.proc.kill()
                            self.proc.join(timeout=1.0)
                        except Exception as exc:
                            log.warning("Visualizer kill failed: %s", exc)
            except Exception as exc:
                log.warning("Error while stopping visualizer: %s", exc)
                try:
                    self.proc.terminate()
                    self.proc.join(timeout=1.0)
                except Exception:
                    pass
            self.proc = None

    def restart(self) -> None:
        """Kill the visualizer process and respawn a fresh one."""
        with self._lock:
            log.info("=== Restarting visualizer ===")
            self.stop()
            # Small gap so Windows cleans up the old pygame window before
            # we start a new one (avoids "SDL already initialized" races).
            time.sleep(0.3)
            self.start()

    def reset(self) -> None:
        """Soft reset — tell the visualizer to clear state without
        killing the process itself. Faster than restart but only clears
        on-screen content; doesn't fix a crashed SDL surface."""
        with self._lock:
            if not self.alive:
                log.info("Visualizer not running — starting fresh")
                self.start()
                return
            if self._send_control({"type": "viz_reset"}):
                log.info("Sent viz_reset to visualizer")


def _setup_logging() -> None:
    fmt = "%(asctime)s %(levelname)-7s %(name)s: %(message)s"
    handlers = [
        logging.StreamHandler(sys.stdout),
        RotatingFileHandler(
            cfg.LOG_FILE, maxBytes=512_000, backupCount=2, encoding="utf-8",
        ),
    ]
    logging.basicConfig(level=logging.INFO, format=fmt, handlers=handlers)


def _install_crash_handlers() -> None:
    """Make every uncaught exception land in the log file.

    In windowed/frozen builds there is no console, so a crash otherwise
    vanishes silently ("crash, no log"): stderr goes nowhere.  We route
    both main-thread and worker-thread uncaught exceptions through the
    logger (RotatingFileHandler => cache/vjvision.log) and show a small
    error dialog for fatal main-thread crashes so the user knows what
    happened.
    """

    def _persist(exc_type, exc, tb, where: str) -> None:
        log.error("Uncaught exception%s:", where, exc_info=(exc_type, exc, tb))

    def _sys_hook(exc_type, exc, tb) -> None:
        if issubclass(exc_type, KeyboardInterrupt):
            sys.__excepthook__(exc_type, exc, tb)
            return
        _persist(exc_type, exc, tb, "")
        # Fatal main-thread crash - the window is about to die; show one
        # dialog so silent "flash and vanish" exits leave a clue on screen.
        try:
            import tkinter.messagebox as _mb
            _mb.showerror(
                "VJVision",
                f"程序遇到未处理的错误，详细信息已写入日志文件：\n"
                f"cache/vjvision.log\n\n{exc_type.__name__}: {exc}",
            )
        except Exception:
            pass

    def _thread_hook(args) -> None:
        _persist(args.exc_type, args.exc_value, args.exc_traceback,
                 f" in thread {args.thread.name!r}")

    sys.excepthook = _sys_hook
    threading.excepthook = _thread_hook


def main() -> int:
    # Required for PyInstaller-frozen apps: spawn-ed child processes
    # (visualizer process, dejavu indexing pool workers) re-launch this
    # exe with multiprocessing arguments; freeze_support() hands control
    # to them instead of running a second UI instance.
    mp.freeze_support()

    # Force multiprocessing to use the CURRENT executable for spawning
    # child processes:
    #   dev mode   → pythonw.exe (no console)
    #   frozen exe → VJVision.exe (windowed, no console)
    # Without this, Windows falls back to python.exe which pops a black
    # console window per spawned process — and indexing can spawn
    # 12 workers simultaneously, so 12 black windows flash on screen.
    mp.set_executable(sys.executable)

    _setup_logging()
    _install_crash_handlers()

    # Load persisted user prefs (device choice, music dir, visual
    # settings) before constructing anything so the UI opens with the
    # user's last state. Done here (not at module import time) so that
    # the spawn-ed visualizer child — which re-imports this module as
    # __mp_main__ — doesn't rely on an import side effect; the child
    # loads prefs explicitly in visualizer.run().
    cfg.load_prefs()

    # Use multiprocessing with 'spawn' on Windows so the pygame child process
    # gets a fresh interpreter (avoids SDL fork issues).
    ctx = mp.get_context("spawn")

    cmd_queue: "_queue.Queue" = ctx.Queue()
    viz_queue: "_queue.Queue" = ctx.Queue(maxsize=2000)
    ui_queue: "_queue.Queue" = ctx.Queue()

    # 1) Visualizer process — wrapped in VisualizerManager for easy restart/reset.
    viz_mgr = VisualizerManager(ctx, viz_queue, cfg.SETTINGS.visualizer_display)
    viz_mgr.start()

    # 2) Matcher thread (lives in our process so it can share AudioCapture).
    matcher = MatcherThread(cmd_queue, viz_queue, ui_queue, viz_mgr)
    matcher.start()

    # 3) Debug UI (main thread).
    try:
        ui = DebugUI(cmd_queue, ui_queue, viz_mgr)
        ui.run()
    except KeyboardInterrupt:
        log.info("Interrupted by user.")
    finally:
        log.info("Shutting down...")
        # Each shutdown step is isolated: a failure in one must not
        # skip the others (especially saving prefs at the end).
        try:
            cmd_queue.put_nowait({"type": "quit"})
        except Exception:
            pass
        try:
            matcher.stop()
        except Exception as exc:
            log.warning("matcher.stop() raised: %s", exc)
        try:
            matcher.join(timeout=3)
            if matcher.is_alive():
                log.warning("Matcher thread did not exit in 3s — it is a "
                            "daemon thread and will end with the process")
        except Exception as exc:
            log.warning("matcher.join() raised: %s", exc)
        try:
            viz_mgr.stop()
        except Exception as exc:
            log.warning("viz_mgr.stop() raised: %s", exc)
        try:
            cfg.save_prefs()
        except Exception as exc:
            log.warning("save_prefs() raised: %s", exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
