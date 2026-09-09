"""Matcher thread - the orchestration hub.

Responsibilities:
* Owns an :class:`AudioCapture` instance (USB soundcard input).
* Polls a ``cmd_queue`` for control messages from the debug UI.
* Periodically snapshots the captured audio and runs dejavu on it.
* When a match is found, reads tags + cover art via :mod:`mutagen` and pushes
  a ``track`` message to the visualizer process (and the UI).
* Forwards every spectrum callback (~30 Hz) to the visualizer process.
"""
from __future__ import annotations

import logging
import queue as _queue
import threading
import time
from pathlib import Path
from typing import Optional

from .audio_capture import AudioCapture
from .config import SETTINGS, save_prefs
from .fingerprint import FingerprintDB
from .i18n import t
from .metadata import Track, extract_track

log = logging.getLogger(__name__)


class MatcherThread(threading.Thread):
    """Glues audio capture, dejavu, metadata and the visualizer together."""

    def __init__(
        self,
        cmd_queue: "_queue.Queue",
        viz_queue: "_queue.Queue",
        ui_queue: "_queue.Queue",
        viz_mgr=None,
    ) -> None:
        super().__init__(daemon=True, name="Matcher")
        self.cmd = cmd_queue
        self.viz = viz_queue
        self.ui = ui_queue
        self.viz_mgr = viz_mgr
        self._stop_event = threading.Event()
        self._capture: Optional[AudioCapture] = None
        self._fp: Optional[FingerprintDB] = None
        # Boot-time monitor opens on the saved device (AudioCapture
        # falls back to the same SETTINGS value when given None).
        self._device_index: Optional[int] = SETTINGS.audio_device
        self._capture_running = False
        self._last_match_ts = 0.0
        self._current_track_path: Optional[str] = None
        # Track-change confirmation filter: require N consecutive hits on
        # the same song_id before we actually switch the displayed track.
        # Kills false positives from brief noise or DJ transitions.
        self._pending_path: Optional[str] = None
        self._pending_hits: int = 0
        # --- DJ mix detection ---
        # During a cross-fade, dejavu matches flip back and forth between
        # the outgoing and incoming tracks (both have similar confidence).
        # We track the last few song_ids so we can detect this "bouncing"
        # pattern and hold the current display until the mix settles.
        self._match_song_history: list[int] = []  # recent matched song_ids
        self._in_mix: bool = False
        # --- Tentative (low-confidence) display ---
        # Multi-version songs (e.g. "Play on!" CN/JP/KR/EN/Inst) split the
        # matching hash count, so the real version often lands below the
        # 0.30 confirm threshold.  We still show it after 2 consecutive
        # hits as a "tentative" preview; once confidence climbs above
        # 0.30 it gets confirmed.  _tentative_path is what's on screen;
        # _current_track_path stays None until real confirmation, so the
        # verification / mix logic still treats the slot as "unlocked".
        self._tentative_path: Optional[str] = None
        self._tentative_hits: int = 0
        # Consecutive tentative hits to a song OTHER than the locked one.
        # When this reaches the break threshold we assume the user has
        # genuinely switched songs (not just a competing version trading
        # the top slot) and release the lock.
        self._tentative_break_count: int = 0
        self._tentative_break_path: Optional[str] = None
        # Periodic verification scheduling: we add random jitter to the
        # match interval so checks aren't rhythmically predictable.
        self._match_jitter_deadline: float = 0.0
        # Cumulator for batched prepare_files messages (UI sends in 500-file
        # chunks to stay under the multiprocessing pipe's 64KB buffer limit).
        self._prepare_pending: list[str] = []

    # -- helpers ----------------------------------------------------------
    def _log(self, text: str, level: str = "info") -> None:
        getattr(log, level, log.info)(text)
        self._send_ui({"type": "log", "text": text})

    def _viz_restart_and_resync(self) -> None:
        """Restart the visualizer, then push the current track back to it.

        A freshly-spawned visualizer process has no memory of what was
        playing — it starts at the standby screen.  After the restart
        completes we re-extract the track metadata and send a ``track``
        message so the new instance jumps straight to the current song
        instead of lingering on standby until the next recognition hit.
        """
        self.viz_mgr.restart()
        if self._current_track_path is not None:
            try:
                from .metadata import extract_track
                t = extract_track(self._current_track_path)
                self._send_viz({
                    "type": "track",
                    "title": t.title,
                    "artist": t.artist,
                    "album": t.album,
                    "cover": t.cover_path,
                    "_resync": True,   # hint for the visualizer
                })
                log.info("Re-synced current track to restarted viz: %s", t.title)
            except Exception as exc:
                log.warning("viz resync failed: %s", exc)

    def _send_viz(self, msg: dict) -> None:
        """Forward a message to the visualizer process.

        Drops silently if the visualizer process is dead — we don't want
        to fill a dead queue with spectrum messages and eventually block
        the matcher thread (which would freeze recognition).
        """
        # Hard queue-full is still a drop-and-warn, same as before.
        # But if the process itself has died we skip putting entirely.
        if self.viz_mgr is not None and not self.viz_mgr.alive:
            return
        try:
            self.viz.put_nowait(msg)
        except _queue.Full:
            # Never block here — the matcher thread must keep running
            # even if the visualizer is slow or dead.
            pass

    def _send_ui(self, msg: dict) -> None:
        try:
            self.ui.put_nowait(msg)
        except _queue.Full:
            log.warning("ui_queue full - dropping %s", msg.get("type"))

    # -- spectrum forwarder (called from the audio thread) ---------------
    def _on_spectrum(self, bins, peak) -> None:
        # Convert numpy array to plain list so multiprocessing pickles cheaply.
        self._send_viz({
            "type": "spectrum",
            "bins": bins.tolist(),
            "peak": float(peak),
        })

    # -- fingerprint DB lifecycle ---------------------------------------
    def _ensure_fp(self) -> None:
        if self._fp is not None:
            return
        self._repair_thread: Optional[threading.Thread] = None
        try:
            self._fp = FingerprintDB()
            stats = self._fp.stats()
            missing = getattr(self._fp, "_missing_hashes", [])
            self._log(
                f"dejavu connected. {stats['songs']} songs indexed. "
                f"DB sync found {len(missing)} missing-hash files."
            )
            self._send_ui({"type": "index_done", "songs": stats["songs"]})

            # Auto-repair orphaned songs: SQLite had path entries but MySQL
            # had no fingerprints.  Run them through index_files silently
            # in a background thread so recognition isn't blocked.
            if missing:
                self._log(
                    f"Auto-repairing {len(missing)} orphaned tracks "
                    f"(including songs like 'Stars Align' that exist on disk "
                    f"but weren't in the DB)...", "warning"
                )
                def _repair_worker():
                    try:
                        from .config import save_prefs
                        repaired = self._fp.index_files(
                            missing, progress=None,
                        )
                        self._log(
                            f"Auto-repair done: {repaired}/{len(missing)} "
                            f"orphaned tracks now fully indexed."
                        )
                        save_prefs()  # ensure post-repair state is captured
                        self._send_ui({"type": "library_status",
                                       **self._fp.library_status()})
                    except Exception as exc:
                        self._log(f"Auto-repair failed: {exc}", "error")
                self._repair_thread = threading.Thread(
                    target=_repair_worker, daemon=True, name="DBRepair",
                )
                self._repair_thread.start()
        except Exception as exc:
            self._log(f"dejavu init failed: {exc}", "error")
            self._fp = None

    # -- audio capture lifecycle ----------------------------------------
    # Two distinct modes share one InputStream:
    #   * MONITOR  - stream open, level meter live, spectrum + recognition
    #                OFF.  Starts automatically at boot (and on every
    #                device change) so the operator can verify signal
    #                levels BEFORE going live.
    #   * CAPTURE  - monitor + spectrum to the visualizer + dejavu
    #                recognition loop.  Toggled by the Start/Stop buttons;
    #                stopping capture returns to monitor mode (the stream
    #                stays open, no audio-device re-open click).
    def _reconfigure_capture(self, device_index) -> bool:
        """(Re)build the AudioCapture object and reopen the stream.

        The stream always comes up in MONITOR mode; callers that want
        recognition (``_capture_running``) re-enable it via
        :meth:`_start_capture`.

        Returns True if a capture object exists afterwards (stream may
        still be closed — e.g. no device present). Never raises: a
        machine with no audio hardware/drivers must degrade to
        "metering off, app still runs", not kill the matcher thread.
        """
        old = self._capture
        try:
            if old is not None:
                try:
                    old.close()
                except Exception:
                    pass
            self._capture = AudioCapture(
                device=device_index, on_spectrum=self._on_spectrum,
            )
            # New object defaults to monitor (spectrum gated off).
            self._capture.spectrum_enabled = self._capture_running
            self._open_stream_monitor()
            return True
        except Exception as exc:
            # AudioUnavailableError (no backend / Pa_Initialize failed /
            # pyaudio not installed) or anything else during build: drop
            # the half-constructed object and tell the operator plainly.
            self._capture = None
            self._capture_running = False
            try:
                from .audio_capture import AudioUnavailableError
                if isinstance(exc, AudioUnavailableError):
                    self._log(f"{t('cap.no_audio')} ({exc})", "error")
                else:
                    self._log(f"{t('cap.init_failed')}: {exc}", "error")
            except Exception:
                log.warning("_reconfigure_capture failed: %s", exc)
            return False

    def _open_stream_monitor(self) -> bool:
        """Open the InputStream for level metering. Errors are non-fatal:
        a missing/busy device at boot just leaves the meter at zero and
        Start Capture will retry."""
        if self._capture is None:
            return False
        try:
            self._capture.start()
            return True
        except Exception as exc:
            self._log(
                t("cap.monitor_unavailable").format(
                    device=self._device_index, error=exc,
                ),
                "error",
            )
            return False

    def _start_monitor(self) -> None:
        """Boot-time monitor: build the capture for the saved device and
        open the stream without starting recognition."""
        if self._capture is None:
            self._reconfigure_capture(self._device_index)
        else:
            self._open_stream_monitor()

    def _start_capture(self) -> None:
        try:
            if self._capture is None:
                if not self._reconfigure_capture(self._device_index):
                    return  # error already logged
            if self._capture is None:
                self._log(t("cap.no_audio"), "error")
                return
            # Stream may have failed to open at boot (device was busy or
            # missing) — retry now.
            if self._capture.current_level().get("active") is False:
                self._capture.start()
            self._capture.spectrum_enabled = True
            self._capture_running = True
            self._send_ui({"type": "capture_status", "text": t("cap.running")})
            self._send_viz({"type": "status", "text": t("viz.capture_started")})
            self._log("Capture started.")
        except Exception as exc:
            self._log(f"{t('cap.start_failed')}: {exc}", "error")
            # Revert the UI toggle (bottom bar) to the idle "start" state
            # so a failed capture doesn't leave a stuck "停止采集" button.
            self._capture_running = False
            self._send_ui({"type": "capture_status",
                           "text": t("cap.monitoring")})

    def _stop_capture(self) -> None:
        # Return to MONITOR mode: recognition + spectrum off, but the
        # stream stays open so the level meter keeps working.
        self._capture_running = False
        if self._capture is not None:
            try:
                self._capture.spectrum_enabled = False
            except Exception:
                pass
        self._send_ui({"type": "capture_status",
                       "text": t("cap.monitoring")})

    def _close_capture(self) -> None:
        """Fully close the InputStream (shutdown only)."""
        if self._capture is not None:
            try:
                self._capture.stop()
            except Exception:
                pass
            self._capture = None
        self._capture_running = False

    # -- indexing --------------------------------------------------------
    def _index_library(self) -> None:
        self._ensure_fp()
        if self._fp is None:
            self._log("Cannot index: dejavu not connected.", "error")
            return

        def progress(done, total, info):
            self._send_ui({
                "type": "index_progress",
                "done": done, "total": total, "info": info,
            })
            if done % 50 == 0:
                self._log(f"[{done}/{total}] {info}")

        def worker():
            self._log("Indexing library - this may take a while...")
            try:
                new = self._fp.index_library(progress)
                stats = self._fp.stats()
                self._send_ui({"type": "index_done", "songs": stats["songs"]})
                self._send_ui({"type": "library_status",
                               **self._fp.library_status()})
                self._log(f"Indexing done: {new} new, total {stats['songs']}.")
            except Exception as exc:
                self._log(f"Indexing failed: {exc}", "error")

        threading.Thread(target=worker, daemon=True, name="Indexer").start()

    def _force_reindex(self) -> None:
        """Clear MySQL + SQLite, then re-index everything from scratch."""
        self._ensure_fp()
        if self._fp is None:
            self._log("Cannot re-analyze: dejavu not connected.", "error")
            return

        # If the startup auto-repair thread is still running, wait for it
        # to finish before we wipe the DB — otherwise the two threads
        # race (repair inserts fingerprints while we TRUNCATE, and the
        # drained connection pool can invalidate repair's cursors).
        repair = getattr(self, "_repair_thread", None)
        if repair is not None and repair.is_alive():
            self._log("Waiting for in-progress auto-repair to finish...")
            repair.join(timeout=120)

        def progress(done, total, info):
            self._send_ui({
                "type": "index_progress",
                "done": done, "total": total, "info": info,
            })
            if done % 50 == 0 or "Cleared" in info or "Re-analyzing" in info:
                self._log(f"[{done}/{total}] {info}")

        def worker():
            self._log("⚠ Force re-analyze: clearing ALL fingerprints...", "warning")
            try:
                new = self._fp.force_reindex(progress)
                stats = self._fp.stats()
                self._send_ui({"type": "index_done", "songs": stats["songs"]})
                self._send_ui({"type": "library_status",
                               **self._fp.library_status()})
                self._log(f"✅ Re-analyze done: {new} new, total {stats['songs']}.")
            except Exception as exc:
                self._log(f"Re-analyze failed: {exc}", "error")

        threading.Thread(target=worker, daemon=True, name="ForceReindexer").start()

    def _prepare_files(self, paths: list[str]) -> None:
        """Semi-automatic preparation: fingerprint just the picked files."""
        if not paths:
            self._log("Nothing to prepare - queue is empty.")
            return
        # Guard against double-clicking "分析队列" which would spawn two
        # concurrent indexing pools (and two sets of black console windows).
        if getattr(self, "_preparing", False):
            self._log(t("prep.already_running"), "warning")
            return
        self._preparing = True
        self._ensure_fp()
        if self._fp is None:
            self._log("Cannot prepare: dejavu not connected.", "error")
            self._preparing = False
            return

        def progress(done, total, info):
            self._send_ui({
                "type": "index_progress",
                "done": done, "total": total, "info": info,
            })
            self._log(f"[{done}/{total}] {info}")

        def worker():
            self._log(f"Preparing {len(paths)} file(s)...")
            try:
                new = self._fp.index_files(paths, progress)
                stats = self._fp.stats()
                self._send_ui({
                    "type": "prep_done",
                    "new": new, "total": len(paths),
                })
                self._send_ui({"type": "index_done", "songs": stats["songs"]})
                self._send_ui({"type": "library_status",
                               **self._fp.library_status()})
                self._log(f"Prepared {new} new of {len(paths)}.")
            except Exception as exc:
                self._log(f"Preparation failed: {exc}", "error")
                self._send_ui({"type": "prep_done", "new": 0,
                               "total": len(paths), "error": str(exc)})
            finally:
                self._preparing = False

        threading.Thread(target=worker, daemon=True, name="Preparer").start()

    def _query_library_status(self) -> None:
        """Push the prepared/pending counts to the UI without re-indexing."""
        self._ensure_fp()
        if self._fp is None:
            return
        try:
            status = self._fp.library_status()
            self._send_ui({"type": "library_status", **status})
        except Exception as exc:
            self._log(f"Status query failed: {exc}", "error")

    # -- matching --------------------------------------------------------
    def _run_match(self) -> None:
        import random

        if not self._capture_running or self._capture is None:
            return
        self._ensure_fp()
        if self._fp is None:
            return
        # Grab the stereo snapshot from the ring buffer. In-memory path —
        # no WAV write, no WAV read, no file hash. Saves ~100-200ms per match.
        try:
            snapshot = self._capture.snapshot()
        except Exception as exc:
            self._log(f"Snapshot failed: {exc}", "error")
            return
        self._send_viz({"type": "status", "text": t("viz.matching")})
        try:
            result = self._fp.match_from_array(
                snapshot, input_sr=self._capture.sr,
            )
        except Exception as exc:
            self._log(f"Match raised: {exc}", "error")
            return

        if not result.matched:
            # No candidate from dejavu (silence or confidence < 0.05).
            # Keep the tentative display if one is active — a transient
            # level dip shouldn't blank the screen.  Only the confirmed
            # pending state is reset.
            self._pending_path = None
            self._pending_hits = 0
            log.info("No match — may need manual track check")
            self._send_viz({"type": "status", "text": t("viz.no_match")})
            return

        # --- Two-tier confidence gate -----------------------------------
        # All three floors are user-tunable in the Debug UI's advanced
        # (red) confidence section; the values below are just defaults.
        # Tier 1 (noise floor): confidence < tentative (0.06) → pure hash
        #   collisions from silence / transients.  Reject outright.
        # Tier 2 (tentative/脉动): tentative (0.06) ≤ confidence < accept
        #   (0.30 for switches, 0.25 for the first track) → likely a real
        #   match but the hash count is diluted (multi-version songs,
        #   quiet capture, etc.).  A DIFFERENT song here starts a pulsing
        #   "tentative" preview; once confidence climbs to the accept floor
        #   it gets hard-confirmed.
        # Tier 3 (confirmed): confidence ≥ accept → normal confirmation
        #   flow (N consecutive hits before switching the display).
        TENTATIVE_CONFIDENCE = SETTINGS.capture.tentative_min_confidence
        # Accept thresholds are user-tunable in the Debug UI's advanced
        # (red) confidence section and persisted via CaptureConfig prefs.
        MIN_ACCEPT_CONFIDENCE = SETTINGS.capture.switch_min_confidence
        # First track uses a lower accept threshold (default 0.25) so the
        # very first song confirms faster — we don't have a confirmed
        # track to "lose" yet, and we've already suppressed the tentative
        # pulse for the first track.  Subsequent tracks use the higher
        # switch threshold (default 0.30) to avoid flicker during quiet
        # passages.
        FIRST_TRACK_MIN_ACCEPT = SETTINGS.capture.first_track_min_confidence
        accept_threshold = (
            FIRST_TRACK_MIN_ACCEPT if self._current_track_path is None
            else MIN_ACCEPT_CONFIDENCE
        )

        if result.confidence < TENTATIVE_CONFIDENCE:
            # --- Pure noise ---
            # Don't clear the tentative lock here — a momentary dip below
            # the noise floor during a quiet passage shouldn't blank the
            # preview.  Only the confirmed-track pending state resets.
            self._pending_path = None
            self._pending_hits = 0
            log.info(
                "Rejecting noise match: %s conf=%.2f < %.2f",
                Path(result.file_path).name, result.confidence,
                TENTATIVE_CONFIDENCE,
            )
            if self._current_track_path is None and self._tentative_path is None:
                self._send_viz({"type": "status", "text": t("viz.listening")})
            return

        if result.confidence < accept_threshold:
            # --- Tentative zone (or first-track below 0.25) ---
            # First-song guard: before any track has been hard-confirmed
            # (current_track_path is None) we must NOT show a pulsing
            # tentative preview from a low-confidence hit.  A stray
            # noise/ambient match below FIRST_TRACK_MIN_ACCEPT would
            # otherwise lock the display onto the wrong song.  Wait
            # silently for a ≥0.25 hit to confirm the first track.
            if self._current_track_path is None:
                self._send_viz({"type": "status", "text": t("viz.listening")})
                log.info(
                    "Ignoring tentative hit for first track: %s conf=%.2f",
                    Path(result.file_path).name, result.confidence,
                )
                return

            # A different song showing up here (even at low confidence)
            # means a cross-fade may be starting.  Trigger the mix pulse
            # immediately instead of waiting for a ≥0.30 hit — otherwise
            # the pulsing display kicks in far too late into the transition.
            if (
                not self._in_mix
                and self._current_track_path is not None
                and result.file_path != self._current_track_path
            ):
                self._in_mix = True
                self._log(
                    "🎚 Mix detected (tentative zone) — different song "
                    "signal. Pulsing current track."
                )
                self._send_viz({"type": "status", "text": t("viz.mixing")})
                try:
                    track = extract_track(self._current_track_path)
                except Exception:
                    track = Track(self._current_track_path, "", "", "", None)
                self._send_viz({
                    "type": "track",
                    "title": track.title,
                    "artist": track.artist,
                    "album": track.album,
                    "cover": track.cover_path,
                    "tentative": True,
                })
                return

            # During a DJ mix we hold the currently-displayed track and let
            # it pulse — low-confidence hits on a different song must NOT
            # flip the display to a tentative preview of the incoming song.
            # The mix stays visually "soft" (pulsing) until a high-confidence
            # confirmed hit settles it.
            if self._in_mix and self._current_track_path is not None:
                if result.file_path == self._current_track_path:
                    # The outgoing track won the mix — stop pulsing and
                    # return to a steady confirmed display.
                    self._in_mix = False
                    self._log("Mix ended — current track re-confirmed.")
                    try:
                        track = extract_track(self._current_track_path)
                    except Exception:
                        track = Track(self._current_track_path, "", "", "", None)
                    self._send_viz({
                        "type": "track",
                        "title": track.title,
                        "artist": track.artist,
                        "album": track.album,
                        "cover": track.cover_path,
                    })
                # Different song during a mix → keep holding the current
                # display (which is already pulsing).
                return

            # If this is the already-confirmed current track, just keep
            # showing it as-is — a confidence dip during a quiet passage
            # must NOT downgrade a confirmed display back to pulsing.
            # Re-send the confirmed track (no tentative flag) in case a
            # competing-version tentative preview is currently on screen.
            if (
                self._current_track_path is not None
                and result.file_path == self._current_track_path
            ):
                # A confirmed hit on the current track ends any active mix
                # (the outgoing track has won) — stop the pulsing.
                if self._in_mix:
                    self._in_mix = False
                    self._log("Mix ended — current track re-confirmed.")
                try:
                    track = extract_track(self._current_track_path)
                except Exception:
                    track = Track(self._current_track_path, "", "", "", None)
                self._send_viz({
                    "type": "track",
                    "title": track.title,
                    "artist": track.artist,
                    "album": track.album,
                    "cover": track.cover_path,
                })
                return

            # Once a tentative preview is on screen, LOCK to that song —
            # don't flip-flop between competing versions every query.
            # Multi-version songs (vocal / Inst / CN / JP …) constantly
            # trade the top slot in the 0.06-0.30 band, so without the
            # lock the display bounces.  We hold the first tentative
            # song until a ≥0.30 hit arrives (either confirming it or
            # replacing it with a different confirmed song).
            #
            # Edge case: the user genuinely switches to a different song
            # whose matches also land in the tentative band.  To avoid
            # the lock holding stale content forever, we count consecutive
            # tentative hits to a *different* song; after 3 in a row we
            # release the lock and switch to the new song.
            TENTATIVE_BREAK = 3

            if self._tentative_path is None:
                # First tentative hit — claim the slot.
                self._tentative_path = result.file_path
                self._tentative_hits = 1
                self._tentative_break_count = 0
                self._tentative_break_path = None
            elif result.file_path == self._tentative_path:
                # Same song still winning — keep it locked.
                self._tentative_hits += 1
                self._tentative_break_count = 0
                self._tentative_break_path = None
            else:
                # Different song in the tentative band.
                if result.file_path == self._tentative_break_path:
                    self._tentative_break_count += 1
                else:
                    self._tentative_break_path = result.file_path
                    self._tentative_break_count = 1

                if self._tentative_break_count >= TENTATIVE_BREAK:
                    # Different song won 3 in a row → genuine track
                    # change, release the lock and switch.
                    log.info(
                        "Tentative lock released: %s won %d consecutive "
                        "tentative hits (was showing %s)",
                        Path(result.file_path).name,
                        self._tentative_break_count,
                        Path(self._tentative_path).name,
                    )
                    self._tentative_path = result.file_path
                    self._tentative_hits = 1
                    self._tentative_break_count = 0
                    self._tentative_break_path = None
                else:
                    # Still below break threshold — hold the lock.
                    log.info(
                        "Tentative lock held: ignoring %s conf=%.2f "
                        "(showing %s, break=%d/%d)",
                        Path(result.file_path).name, result.confidence,
                        Path(self._tentative_path).name,
                        self._tentative_break_count, TENTATIVE_BREAK,
                    )
                    return

            # Show / refresh the pulsing preview.
            try:
                track = extract_track(self._tentative_path)
            except Exception as exc:
                self._log(f"Metadata read failed: {exc}", "error")
                track = Track(self._tentative_path, "", "", "", None)
            self._send_viz({
                "type": "track",
                "title": track.title,
                "artist": track.artist,
                "album": track.album,
                "cover": track.cover_path,
                "tentative": True,
            })
            self._send_ui({
                "type": "track",
                "title": track.title,
                "artist": track.artist,
                "album": track.album,
                "confidence": result.confidence,
            })
            log.info(
                "Tentative display: %s conf=%.2f (hits=%d)",
                track.title, result.confidence, self._tentative_hits,
            )
            return

        # --- Confirmed zone (confidence >= 0.30) ---
        # If we were showing a tentative preview and this high-confidence
        # hit is for the same song, fast-confirm it — the tentative hit
        # already proved it's the top candidate, so one confirmed hit is
        # enough.
        fast_confirm = (
            result.file_path == self._tentative_path
            and self._tentative_hits >= 1
        )
        self._tentative_path = None
        self._tentative_hits = 0
        self._tentative_break_count = 0
        self._tentative_break_path = None

        # --- Record song_id history for mix detection ---
        # We keep the last 4 matched song_ids.  During a DJ cross-fade the
        # matches bounce between the outgoing and incoming tracks, so the
        # history will contain 2+ distinct song_ids that are NOT the
        # currently-displayed track.  That's our "mix in progress" signal.
        try:
            song_id = int(result.song_id)
        except (TypeError, ValueError):
            song_id = -1
        self._match_song_history.append(song_id)
        if len(self._match_song_history) > 4:
            self._match_song_history = self._match_song_history[-4:]

        # --- Track-change confirmation filter ---
        # DJs mix / cross-fade so a single transient misfire on a mid-track
        # noise burst could flip us to the wrong song.  Require N consecutive
        # hits on the same track before we actually switch the display.
        CONFIRM = SETTINGS.capture.match_confirmations

        if result.file_path == self._current_track_path:
            # --- Ongoing verification on the current track ---
            self._pending_path = None
            self._pending_hits = 0
            # Current track confirmed again → we're NOT in a mix (the
            # outgoing track has won / the mix is over).
            if self._in_mix:
                self._in_mix = False
                self._log("Mix ended — current track re-confirmed.")
            return

        # --- Different track detected ---
        # Decide whether we're in the middle of a DJ mix.  A mix is when
        # the last few matches bounce between 2+ songs that are NOT the
        # currently displayed track.  In that case we HOLD the current
        # display rather than flip-flopping.

        # Distinct song_ids in recent history.  During a mix the history
        # contains 2+ different songs with no clear majority (bouncing).
        distinct = set(self._match_song_history)
        # A mix is when recent matches contain 2+ different songs, neither
        # of which has a clear majority (i.e. it's bouncing, not settling).
        if len(distinct) >= 2 and self._current_track_path is not None:
            # Count how many times each song appears in the last 4.
            from collections import Counter
            counts = Counter(self._match_song_history)
            top_song, top_count = counts.most_common(1)[0]
            # If the top song appears ≤ 3 times out of 4, another song has
            # shown up at least once → likely a mix / cross-fade is in
            # progress.  (≤2 was too strict: when the incoming song first
            # appears the history is [A,A,A,B], top=3, so the mix was
            # detected only on the 2nd incoming hit — by which time it was
            # already confirmed, skipping the pulsing display.)
            if top_count <= 3 and not self._in_mix:
                self._in_mix = True
                self._log(
                    f"🎚 Mix detected — matches bouncing between "
                    f"{len(distinct)} songs. Holding current display.",
                )
                self._send_viz({"type": "status", "text": t("viz.mixing")})
                # Re-send the current track with the tentative (pulsing)
                # flag so the display visually signals "mix in progress"
                # without changing which song is shown.
                if self._current_track_path is not None:
                    try:
                        track = extract_track(self._current_track_path)
                    except Exception:
                        track = Track(self._current_track_path, "", "", "", None)
                    self._send_viz({
                        "type": "track",
                        "title": track.title,
                        "artist": track.artist,
                        "album": track.album,
                        "cover": track.cover_path,
                        "tentative": True,
                    })

        if self._in_mix and self._current_track_path is not None and not fast_confirm:
            # During a mix, hold the current display.  Only exit mix mode
            # when the same NEW song wins 2 consecutive matches.
            #
            # A mix makes hash counts noisy (two songs' fingerprints
            # overlap), so we raise the confidence bar for switching —
            # a ≥0.30 hit is a stronger signal that the incoming song has
            # actually taken over than the tentative 0.06 floor, while
            # still being reachable when the new track's fingerprints are
            # diluted by the outgoing track during a long cross-fade.
            # (0.40 was too strict: long mixes often peak at 0.30–0.38.)
            # Follows the user-tunable switch threshold so the "切歌置信度"
            # knob controls switching in and out of mixes consistently.
            MIX_MIN_CONFIDENCE = SETTINGS.capture.switch_min_confidence
            if result.confidence < MIX_MIN_CONFIDENCE:
                log.info(
                    "Mix hold — %s conf=%.2f below mix threshold %.2f",
                    Path(result.file_path).name, result.confidence,
                    MIX_MIN_CONFIDENCE,
                )
                return
            if result.file_path == self._pending_path:
                self._pending_hits += 1
            else:
                self._pending_path = result.file_path
                self._pending_hits = 1

            if self._pending_hits >= 2:
                # New song has won 2 in a row → mix is settling.
                self._in_mix = False
                self._log("Mix settling — new track confirmed.")
                # Fall through to the confirmed-switch logic below.
            else:
                log.info(
                    "Mix hold — pending %s (%d/2), conf=%.2f",
                    Path(result.file_path).name, self._pending_hits,
                    result.confidence,
                )
                return

        # --- Determine confirmation threshold ---
        # First track ever (no current track): confirm immediately with 1
        # hit so the display populates fast at startup.
        if fast_confirm:
            # Already proven stable via 2+ tentative hits — one
            # confirmed hit is enough to lock it in.
            confirm_needed = 1
            self._pending_path = result.file_path
            self._pending_hits = 1
            # Committing to a track exits mix mode (the tentative lock
            # already filtered out bounce noise).
            self._in_mix = False
        elif self._current_track_path is None:
            confirm_needed = 1
        else:
            confirm_needed = CONFIRM

        if result.file_path == self._pending_path:
            self._pending_hits += 1
        else:
            self._pending_path = result.file_path
            self._pending_hits = 1

        if self._pending_hits < confirm_needed:
            log.info(
                "Pending match (%d/%d): %s (conf=%.2f)",
                self._pending_hits, confirm_needed,
                Path(result.file_path).name, result.confidence,
            )
            self._send_viz({
                "type": "status",
                "text": t("viz.pending", cur=self._pending_hits, need=confirm_needed),
            })
            return

        # Confirmed!  Switch the display.
        try:
            track = extract_track(result.file_path)
        except Exception as exc:
            self._log(f"Metadata read failed: {exc}", "error")
            track = Track(result.file_path, "", "", "", None)
        self._current_track_path = result.file_path
        self._pending_path = None
        self._pending_hits = 0

        self._log(f"Match: {track.title} (conf={result.confidence:.2f})")

        self._send_viz({
            "type": "track",
            "title": track.title,
            "artist": track.artist,
            "album": track.album,
            "cover": track.cover_path,
        })
        self._send_ui({
            "type": "track",
            "title": track.title,
            "artist": track.artist,
            "album": track.album,
            "confidence": result.confidence,
        })
        self._log(f"Match: {track.title} (conf={result.confidence:.2f})")

    # -- command dispatch ------------------------------------------------
    def _handle_cmd(self, msg: dict) -> None:
        mtype = msg.get("type")
        if mtype == "quit":
            self._stop_event.set()
        elif mtype == "viz_restart":
            if self.viz_mgr is not None:
                # Run in a background thread — restart() has a sleep(0.3)
                # and we don't want to block the matcher's main loop.
                threading.Thread(
                    target=self._viz_restart_and_resync,
                    daemon=True, name="VizRestart",
                ).start()
                self._log("Restarting visualizer...")
        elif mtype == "viz_reset":
            if self.viz_mgr is not None:
                self.viz_mgr.reset()
                self._log("Resetting visualizer state...")
        elif mtype == "device":
            idx = msg.get("index")
            # The UI re-announces the current device on every device-list
            # refresh (including at boot, right after our monitor stream
            # already opened).  Skip the close/reopen cycle when the
            # selection is unchanged and the stream is already live.
            try:
                already_live = (
                    self._capture is not None
                    and self._capture.current_level().get("active")
                )
            except Exception:
                # Stream died / device vanished mid-session - treat as
                # not live and go through the reconfigure path.
                already_live = False
            if idx == self._device_index and already_live:
                pass
            else:
                self._device_index = idx
                self._reconfigure_capture(idx)
                if self._capture_running:
                    self._start_capture()
        elif mtype == "start":
            self._start_capture()
        elif mtype == "stop":
            self._stop_capture()
        elif mtype == "music_dir":
            SETTINGS.music_dir = Path(msg.get("path", SETTINGS.music_dir))
        elif mtype == "index_library":
            self._index_library()
        elif mtype == "force_reindex":
            self._force_reindex()
        elif mtype == "prepare_files":
            # Legacy single-shot path — kept for backwards compatibility
            # with anything else that might call us.  Goes straight to
            # indexing, no batching.
            self._prepare_files(msg.get("paths", []))
        elif mtype == "prepare_files_batch":
            # Accumulate 500-file chunks from the UI until the finalising
            # ``prepare_files_done`` message arrives below.
            chunk = msg.get("paths", [])
            if chunk:
                self._prepare_pending.extend(chunk)
        elif mtype == "prepare_files_done":
            # All batches received — de-dup, then fire the real index pass.
            if self._prepare_pending:
                unique = list(dict.fromkeys(self._prepare_pending))
                self._prepare_pending = []
                self._prepare_files(unique)
            else:
                self._log("No files received in any batch — nothing to prepare.")
        elif mtype == "cancel_indexing":
            self._prepare_pending = []  # drop any partially-accumulated batches
            if self._fp is not None:
                self._fp.cancel_indexing()
            self._log("[cancel] Indexing cancelled by user.")
        elif mtype == "library_status":
            self._query_library_status()
        elif mtype == "settings":
            # Update our shared SETTINGS so the changes persist on restart.
            if "style" in msg:
                SETTINGS.visual.spectrum_style = msg["style"]
            if "rotation_speed" in msg:
                SETTINGS.visual.rotation_speed = msg["rotation_speed"]
            if "beat_reactive" in msg:
                SETTINGS.visual.beat_reactive = msg["beat_reactive"]
            if "fullscreen" in msg:
                SETTINGS.visual.fullscreen = bool(msg["fullscreen"])
            if "bg_mode" in msg:
                SETTINGS.visual.bg_mode = msg["bg_mode"]
            if "font_name" in msg:
                SETTINGS.visual.font_name = str(msg["font_name"])
            if "standby_image" in msg:
                SETTINGS.visual.standby_image = str(msg["standby_image"])
            # Advanced confidence thresholds (Debug UI red section).
            # Clamp to a sane range so a typo can't brick recognition.
            conf_changed = False
            if "first_track_min_confidence" in msg:
                try:
                    SETTINGS.capture.first_track_min_confidence = min(
                        0.95, max(0.05, float(msg["first_track_min_confidence"])))
                    conf_changed = True
                except (TypeError, ValueError):
                    pass
            if "tentative_min_confidence" in msg:
                try:
                    SETTINGS.capture.tentative_min_confidence = min(
                        0.95, max(0.05, float(msg["tentative_min_confidence"])))
                    conf_changed = True
                except (TypeError, ValueError):
                    pass
            if "switch_min_confidence" in msg:
                try:
                    SETTINGS.capture.switch_min_confidence = min(
                        0.95, max(0.05, float(msg["switch_min_confidence"])))
                    conf_changed = True
                except (TypeError, ValueError):
                    pass
            if conf_changed:
                self._log(
                    f"Confidence thresholds: first-track="
                    f"{SETTINGS.capture.first_track_min_confidence:.2f}, "
                    f"tentative/pulse={SETTINGS.capture.tentative_min_confidence:.2f}, "
                    f"switch={SETTINGS.capture.switch_min_confidence:.2f}"
                )
                save_prefs()
            self._send_viz(msg)   # forward to the visualizer process.

    # -- main loop -------------------------------------------------------
    def run(self) -> None:
        import random as _random
        self._log("Matcher thread started.")
        last_match_ts = 0.0
        last_level_ts = 0.0
        last_viz_check_ts = 0.0
        level_period = 0.1        # 10 fps level updates to the UI
        viz_check_period = 5.0    # check viz process liveness every 5s
        # Start with a small random offset so first match doesn't happen
        # exactly at match_interval after start (avoids predictability).
        self._match_jitter_deadline = _random.uniform(0.5, 2.0)

        # Open the input stream in MONITOR mode right away: the operator
        # must be able to verify signal levels on the selected soundcard
        # BEFORE pressing Start Capture.  Failures are logged but never
        # block the matcher thread.
        try:
            self._start_monitor()
        except Exception as exc:
            self._log(f"Input monitor failed to start: {exc}", "error")

        while not self._stop_event.is_set():
            # Drain the command queue.  A single bad message must never
            # kill the matcher thread (that would silently stop metering
            # and recognition for the whole session) — log and continue.
            try:
                while True:
                    msg = self.cmd.get_nowait()
                    try:
                        self._handle_cmd(msg)
                    except Exception as exc:
                        log.exception("Command %r failed: %s", msg, exc)
                        try:
                            self._log(f"Command failed: {exc}", "error")
                        except Exception:
                            pass
            except _queue.Empty:
                pass

            now = time.monotonic()
            base_interval = SETTINGS.capture.match_interval
            # During a mix (pulsing display) we want to confirm the new
            # track as fast as possible, so halve the recognition interval
            # — more attempts per second means we catch the confidence
            # climb sooner instead of waiting through a long cross-fade.
            if self._in_mix:
                base_interval = max(1.5, base_interval * 0.5)

            # --- Viz liveness check ---
            # If the visualizer died (ESC pressed, SDL crash, etc.), we
            # notify the UI so the operator can restart it via the button.
            if (self.viz_mgr is not None and
                    now - last_viz_check_ts >= viz_check_period):
                last_viz_check_ts = now
                if not self.viz_mgr.alive:
                    self._send_ui({
                        "type": "viz_status",
                        "text": t("viz.exited"),
                    })

            # --- Recognition with jittered interval ---
            # Jitter makes verification timing non-rhythmic (±30% of base)
            # so a DJ's mix transitions don't accidentally synchronize with
            # our match attempts. Also helps catch the 伴奏/原曲 case
            # at different points in the track.
            if self._capture_running:
                elapsed = now - last_match_ts
                if elapsed >= self._match_jitter_deadline:
                    last_match_ts = now
                    # Compute next interval: base ±30% random jitter
                    jittered = base_interval * _random.uniform(0.7, 1.3)
                    self._match_jitter_deadline = jittered
                    try:
                        self._run_match()
                    except Exception as exc:
                        self._log(f"Match loop error: {exc}", "error")

            # Push live input level to the UI ~10 fps so the user can
            # see whether their soundcard is receiving signal.  Works in
            # BOTH modes: monitor (before capture) and capture.
            if self._capture is not None and now - last_level_ts >= level_period:
                last_level_ts = now
                try:
                    self._send_ui({"type": "level",
                                   "capturing": self._capture_running,
                                   **self._capture.current_level()})
                except Exception:
                    pass

            time.sleep(0.05)

        # Cleanup.
        self._close_capture()
        if self._fp is not None:
            try:
                self._fp.close()
            except Exception:
                pass
        self._log("Matcher thread stopped.")

    def stop(self) -> None:
        self._stop_event.set()
