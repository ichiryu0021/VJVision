"""Audio fingerprinting + matching via :mod:`dejavu` plus a small SQLite index
that maps dejavu's ``song_id`` to the on-disk file path of the matched track.

dejavu only stores ``song_name`` (the string we passed when fingerprinting).
We keep ``CACHE_DIR/song_paths.sqlite`` to map ``song_id -> file_path`` so we
can later read tags & cover art from the right file.
"""
from __future__ import annotations

import logging
import multiprocessing as _mp
import os
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from .config import SETTINGS, CACHE_DIR, FINGERPRINTS_DB, SONG_PATHS_DB

log = logging.getLogger(__name__)

# Files this worker process decoded via the ffmpeg tolerant-decode
# fallback (libsndfile rejected them — e.g. a truncated/corrupt FLAC).
# Maps absolute file path -> human note.  Per-process: indexing runs in
# spawned workers, so the main process reads the note carried back in
# the worker result tuple, not this dict.
_fallback_notes: dict = {}


# --- Windows: suppress black console windows for multiprocessing workers ---
# ``mp.set_executable(sys.executable)`` alone is not enough on Windows — the
# spawn-ed worker processes still allocate a console via CreateProcess.  We
# monkeypatch ``_winapi.CreateProcess`` to OR in CREATE_NO_WINDOW so indexing
# workers (up to 12 of them) don't flash a black cmd window each.
import sys as _sys
if _sys.platform == "win32":
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
        log.debug("Applied CREATE_NO_WINDOW patch to _winapi.CreateProcess")
    except Exception as _exc:
        log.warning("Could not apply CREATE_NO_WINDOW patch: %s", _exc)


# (done, total, message) -> None
ProgressCb = Callable[[int, int, str], None]

# Per-worker FingerprintDB instance — set once by ``_init_worker`` when
# each pool child spawns, then reused for every song that worker handles.
# This replaces the old "new FingerprintDB() per song" pattern, which
# paid the full dejavu-init + hash-cache-rebuild cost for every file.
_worker_db: Optional["FingerprintDB"] = None


def _init_worker() -> None:
    """Pool initializer — build ONE FingerprintDB per worker process.

    Runs once at worker spawn (not once per song).  The instance lives
    for the lifetime of the worker and is reused across all songs it
    processes, eliminating the dejavu bootstrap overhead that used to
    dominate per-song indexing time.
    """
    global _worker_db
    _worker_db = FingerprintDB()


def _register_sqlite_backend() -> None:
    """Register our SQLite dejavu backend in dejavu's DATABASES registry.

    Idempotent; safe to call from every process (main + index workers).
    Also imports the module normally so PyInstaller bundles it (the
    registry entry itself is loaded via importlib at runtime).
    """
    import dejavu.config.settings as _djv_settings
    from .dejavu_sqlite import SQLiteDatabase  # noqa: F401 (bundling + sanity)

    _djv_settings.DATABASES["sqlite"] = (
        "vjvision.dejavu_sqlite", "SQLiteDatabase")


@dataclass
class MatchResult:
    matched: bool
    file_path: Optional[str]
    confidence: float
    offset: float
    song_id: Optional[int]
    raw: dict


class FingerprintDB:
    """Wraps dejavu + a local SQLite index of file paths."""

    TARGET_SR = 44100  # Must match audio_capture's sr — critical for hash alignment.

    def __init__(self) -> None:
        # --- Python 3.13+ audioop shim ---
        # Python 3.14 removed stdlib ``audioop``; pydub (transitively
        # imported by dejavu) falls back to ``import pyaudioop as audioop``,
        # but the PyPI ``pyaudioop`` 0.1.0 is an EMPTY stub on some installs.
        # We inject a numpy-based drop-in so pydub/dejavu imports don't
        # crash — we don't actually use pydub for decoding (soundfile
        # fallback handles everything), the shim just needs to be importable.
        self._install_audioop_shim()

        from dejavu import Dejavu  # lazy import so config errors surface clearly
        # --- Patch 1: soundfile fallback + resample-to-44100 ---
        self._install_soundfile_fallback()
        # --- Patch 2: reduce fingerprint density (90k → ~10k hashes/song) ---
        self._optimize_dejavu_params()

        # Use the portable SQLite backend (single file in the data dir,
        # copyable between PCs) instead of a MySQL server.
        _register_sqlite_backend()
        dejavu_config = {
            "database": {"path": str(FINGERPRINTS_DB)},
            "database_type": "sqlite",
            # None = index the ENTIRE song so DJ can start from any point.
            "fingerprint_limit": None,
        }
        self._djv = Dejavu(dejavu_config)
        self._db_path = FINGERPRINTS_DB

        # Cache of file_sha1 → song_id for the main process only.  Built
        # once from dejavu's songs table; updated after every insert so
        # _find_song_by_hash doesn't have to do a full table scan per
        # file (O(n²) over 2000+ songs otherwise).
        self._song_hash_cache: dict[str, int] = {}
        self._rebuild_hash_cache()

        self._sqlite = sqlite3.connect(
            str(SONG_PATHS_DB), check_same_thread=False
        )
        self._sqlite.row_factory = sqlite3.Row
        self._init_schema()
        # Cancel flag: when True, index_library/index_files aborts early.
        self._cancel_flag = False

        # Repair dejavu-store ↔ path-index drift on startup.  If songs
        # were half-cleared or workers crashed mid-way the two stores
        # disagree — sync now so "is this file already indexed?" checks
        # are accurate.
        self._missing_hashes = self._sync_stores()

    @staticmethod
    def _resample_channels(channels: list, from_sr: int, to_sr: int) -> tuple[list, int]:
        """Resample every channel to ``to_sr`` if needed.

        Returns (resampled_channels, to_sr).  When ``from_sr == to_sr``
        the original lists are returned unchanged (zero overhead).

        Uses ``scipy.signal.resample_poly`` (polyphase filter) which is
        5-10x faster than ``resample`` (FFT-based) for large arrays.

        NOTE: The main decode path no longer calls this — soundfile's
        ``samplerate=`` argument resamples in libsndfile's C code.  This
        method is kept as a fallback only.
        """
        if from_sr == to_sr:
            return channels, from_sr
        import numpy as np
        from scipy import signal

        log.info("  Resample: %d → %d Hz (polyphase, %d channels)",
                 from_sr, to_sr, len(channels))
        out = []
        for ch in channels:
            resampled = signal.resample_poly(ch, to_sr, from_sr)
            resampled = np.clip(resampled, -32768, 32767).astype(np.int16)
            out.append(resampled)
        return out, to_sr

    @classmethod
    def _install_audioop_shim(cls) -> None:
        """Inject a numpy-based drop-in ``audioop`` module into ``sys.modules``.

        Python 3.13+ removed stdlib ``audioop``.  dejavu's decoder imports
        pydub, which needs 11 ``audioop.*`` functions.  We shim them with
        trivial numpy implementations so dejavu imports succeed; we never
        actually use pydub for decoding (the soundfile fallback handles
        that) so the shim's fidelity doesn't matter.
        """
        import sys, types as _types

        if "audioop" in sys.modules:
            return  # already present (real module or prior shim)

        import numpy as np

        audioop = _types.ModuleType("audioop")

        def _buf_to_np(buf, width):
            arr = np.frombuffer(buf, dtype=np.int16 if width == 2 else np.int8)
            return arr.astype(np.float64)

        def _np_to_buf(arr, width):
            arr = np.clip(arr, -32768, 32767)
            return arr.astype(np.int16 if width == 2 else np.int8).tobytes()

        audioop.lin2lin = lambda fm, fw, tw: fm  # pass-through is fine for our use
        audioop.tomono = lambda fr, fw, l, r: fr
        audioop.tostereo = lambda fm, fw, l, r: fm
        audioop.bias = lambda fr, fw, b: (_buf_to_np(fr, fw) + b).astype(np.int16).tobytes()
        audioop.mul = lambda fr, fw, m: (_buf_to_np(fr, fw) * m).astype(np.int16).tobytes()
        audioop.add = lambda f1, f2, fw: (_buf_to_np(f1, fw) + _buf_to_np(f2, fw)).astype(np.int16).tobytes()
        audioop.ratecv = lambda fr, fw, frate, trate, ch, w, o: (fr, 0, 0, 0, 0, 0)
        audioop.avg = lambda fr, fw: int(np.mean(np.abs(_buf_to_np(fr, fw))))
        audioop.max = lambda fr, fw: int(np.max(np.abs(_buf_to_np(fr, fw))))
        audioop.rms = lambda fr, fw: float(np.sqrt(np.mean(_buf_to_np(fr, fw) ** 2)))
        audioop.reverse = lambda fr, fw: fr[::-1]

        sys.modules["audioop"] = audioop
        log.debug("audioop shim installed (numpy-based, Python 3.14 compat)")

    @classmethod
    def _install_soundfile_fallback(cls) -> None:
        """Replace dejavu.logic.decoder.read with a soundfile-only decoder.

        Previously this wrapped dejavu's pydub-based decoder and fell
        back to soundfile on failure.  That meant EVERY file paid for a
        failed pydub+ffmpeg decode attempt first (pydub can't handle
        24-bit FLAC and most of our library is 24-bit).  We now skip
        pydub entirely — soundfile (libsndfile) decodes FLAC/WAV/MP3/OGG/AIFF natively —
        no pydub/ffmpeg needed.  Resampling to 44100 Hz is done with
        scipy.signal.resample_poly (polyphase filter), which is 5-10x
        faster than the old FFT-based scipy.signal.resample.

        Resampling to 44100 Hz is CRITICAL: FFT-based fingerprints
        encode frequency-bin indices that depend on the sampling rate.
        A 48 kHz source + 44.1 kHz capture would NEVER match otherwise.
        """
        import dejavu.logic.decoder as _decoder
        import soundfile as sf
        from hashlib import sha1

        TARGET = cls.TARGET_SR

        def _patched_read(file_name: str, limit=None):
            """Decode via soundfile, then resample to TARGET_SR if needed.

            soundfile's ``samplerate=`` arg only works for RAW files, so we
            read at the file's native rate and resample via
            ``scipy.signal.resample_poly`` (polyphase, 5-10x faster than
            the old FFT-based ``resample``).

            If libsndfile rejects the whole file (a truncated/corrupt
            download raises "flac decoder lost sync" etc.), we fall back
            to ffmpeg, which skips broken frames and decodes everything
            else — see :meth:`_decode_via_ffmpeg`.
            """
            try:
                info = sf.info(file_name)
                sr = info.samplerate
                stop_frames = int(limit * sr) if limit else -1

                data, _ = sf.read(
                    file_name, dtype="int16", always_2d=False,
                    stop=stop_frames,
                )
            except Exception as sf_exc:
                # libsndfile is unforgiving: one bad frame near EOF makes
                # it refuse the ENTIRE file.  ffmpeg is tolerant, so use
                # it as the fallback path (native rate, same downstream
                # resample pipeline → consistent fingerprints).
                data, sr = cls._decode_via_ffmpeg(
                    file_name, limit,
                    sf_error=f"{type(sf_exc).__name__}: {sf_exc}",
                )

            if data.ndim == 1:
                channels = [data]
            else:
                channels = [data[:, ch].copy() for ch in range(data.shape[1])]

            # Resample to TARGET_SR (polyphase filter in scipy).
            channels, sr = cls._resample_channels(channels, sr, TARGET)

            # File SHA1 — same algorithm as dejavu's decoder.unique_hash.
            # Hash the ORIGINAL file regardless of decode path, so a
            # ffmpeg-recovered song keeps a stable identity.
            s = sha1()
            with open(file_name, "rb") as fh2:
                while True:
                    buf = fh2.read(2**20)
                    if not buf:
                        break
                    s.update(buf)
            return channels, int(sr), s.hexdigest().upper()

        _decoder.read = _patched_read

    @staticmethod
    def _find_ffmpeg() -> Optional[str]:
        """Locate an ffmpeg executable.

        Order: system ffmpeg on PATH (user-controlled) → ffmpeg shipped
        next to the executable → the static binary bundled via the
        ``imageio-ffmpeg`` pip package (guarantees the tolerant-decode
        fallback works on machines — incl. macOS — without ffmpeg
        installed; in PyInstaller builds the package data is collected
        by VJVision.spec).
        """
        import shutil
        exe = shutil.which("ffmpeg")
        if exe:
            return exe
        # ffmpeg(.exe) shipped next to the executable.
        try:
            cand = os.path.join(
                os.path.dirname(os.path.abspath(_sys.executable)),
                "ffmpeg.exe" if os.name == "nt" else "ffmpeg",
            )
            if os.path.isfile(cand):
                return cand
        except Exception:
            pass
        # Static binary bundled with imageio-ffmpeg.
        try:
            import imageio_ffmpeg
            cand = imageio_ffmpeg.get_ffmpeg_exe()
            if cand and os.path.isfile(cand):
                return cand
        except Exception:
            pass
        return None

    @classmethod
    def _decode_via_ffmpeg(cls, file_name: str, limit, sf_error: str):
        """Tolerant decode for files libsndfile rejects.

        ffmpeg skips broken frames ("invalid residual", "lost sync") and
        decodes the rest instead of refusing the whole file.  We decode
        to a temporary 16-bit WAV at the file's NATIVE sample rate, then
        return ``(data, sr)`` in exactly the shape the healthy soundfile
        path produces — the caller runs the same channel-split and
        polyphase-resample pipeline afterwards.

        Raises RuntimeError with an actionable bilingual message when
        ffmpeg is missing or also fails.  Never returns partial silence.
        """
        import subprocess
        import tempfile
        import soundfile as sf

        name = os.path.basename(file_name)
        ff = cls._find_ffmpeg()
        if not ff:
            raise RuntimeError(
                f"音频文件可能已损坏（soundfile 解码失败：{sf_error}），"
                f"且系统中未找到 ffmpeg 进行容错解码。请安装 ffmpeg 后重试，"
                f"或用转码工具（foobar2000/格式工厂等）将该文件重新转码后"
                f"再加入曲库。 | file may be corrupted and ffmpeg was not "
                f"found on PATH"
            )

        tmp_fd, tmp_path = tempfile.mkstemp(prefix="vjff_", suffix=".wav")
        os.close(tmp_fd)
        try:
            cmd = [ff, "-hide_banner", "-v", "error", "-y",
                   "-i", file_name, "-vn"]
            if limit:
                cmd += ["-t", str(limit)]
            cmd += ["-f", "wav", "-acodec", "pcm_s16le", tmp_path]
            kwargs = dict(capture_output=True, text=True, timeout=900)
            if os.name == "nt":
                kwargs["creationflags"] = _CREATE_NO_WINDOW
            try:
                r = subprocess.run(cmd, **kwargs)
            except FileNotFoundError:
                raise RuntimeError(
                    f"音频文件可能已损坏（{sf_error}），ffmpeg 无法启动。"
                    f" | ffmpeg disappeared at runtime")
            except subprocess.TimeoutExpired:
                raise RuntimeError(
                    f"ffmpeg 解码超时（文件可能过大或损坏严重）：{name}"
                    f" | ffmpeg decode timed out")
            if r.returncode != 0:
                tail = (r.stderr or "").strip()[-400:]
                raise RuntimeError(
                    f"音频文件损坏严重，soundfile 与 ffmpeg 均无法解码"
                    f"（ffmpeg: {tail}）。请重新获取或转码该文件后重试。"
                    f" | both soundfile and ffmpeg failed to decode"
                )
            data, sr = sf.read(tmp_path, dtype="int16", always_2d=False)
            if data is None or len(data) == 0:
                raise RuntimeError(
                    f"ffmpeg 解码结果为空（文件可能损坏严重）：{name}"
                    f" | ffmpeg produced no audio"
                )
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

        # Note for the main process — worker log handlers don't reach
        # the main log file, so the worker result tuple carries this.
        _fallback_notes[os.path.abspath(file_name)] = (
            f"soundfile failed ({sf_error}); decoded via ffmpeg fallback"
        )
        log.warning("ffmpeg fallback decode OK for %s: %s", name, sf_error)
        return data, int(sr)

    @staticmethod
    def _optimize_dejavu_params() -> None:
        """Monkey-patch dejavu config to produce FAR fewer fingerprints.

        Dejavu's defaults generate ~10.800 hashes per song-minute on our
        44.1 kHz FLACs (57 songs = 6.2 million hashes = 744 MB InnoDB +
        247 MB binlog).  That is absurd for a VJ use case where we only
        need to identify ~12-second clips.

        After tuning: overlap 0.5→0.25 (halves windows), fan 5→3 (40 %
        fewer pairs), amp_min 10→15 (kills weak peaks), neighbourhood
        10→15 (fewer peaks).  Net effect: ~85-90 % fewer hashes.

        IMPORTANT: Three layers must be patched because Python binds
        function defaults at DEFINE time, not call time:
          1) dejavu.config.settings module globals (the source)
          2) dejavu.logic.fingerprint module globals (``from X import Y``
             snapshots taken at fingerprint.py import time)
          3) fingerprint() function's __defaults__ tuple (bound when
             the def statement ran — this is what ACTUALLY matters when
             Dejavu.get_file_fingerprints calls fingerprint(ch, Fs=fs))
        """
        import dejavu.config.settings as _s
        import dejavu.logic.fingerprint as _fp

        log.info("Tuning dejavu for DJ library (reducing hash density)")

        # --- Layer 1 & 2: module-level namespaces ---
        # Tuned for DJ libs: ~15k-20k hashes/song (vs dejavu default 100k+)
        # — dense enough to reliably match 8-second capture clips, small
        # enough to keep MySQL writes under control.
        _patch_vals = {
            "DEFAULT_OVERLAP_RATIO": 0.25,    # windows 2× default spacing
            "DEFAULT_FAN_VALUE": 3,            # 40 % fewer pairs than default
            "DEFAULT_AMP_MIN": 15,             # kill low-amplitude peaks
            "PEAK_NEIGHBORHOOD_SIZE": 20,      # bigger = fewer peaks
        }
        for name, val in _patch_vals.items():
            for mod in (_s, _fp):
                if hasattr(mod, name):
                    setattr(mod, name, val)

        # --- Layer 3: fingerprint() function __defaults__ ---
        # fingerprint has signature: (channel_samples, Fs=44100,
        # wsize=4096, wratio=0.5, fan_value=5, amp_min=10)
        # __defaults__ tuple is (44100, 4096, 0.5, 5, 10)
        try:
            fn = _fp.fingerprint
            old = fn.__defaults__  # (Fs, wsize, wratio, fan, amp_min)
            new_defaults = (
                old[0],                     # Fs = 44100 (keep)
                old[1],                     # wsize = 4096 (keep)
                0.25,                       # wratio: 0.5 → 0.25
                3,                          # fan_value: 5 → 3
                15,                         # amp_min: 10 → 15
            )
            fn.__defaults__ = new_defaults
            log.info("  Patched fingerprint() __defaults__: %s → %s",
                     old, new_defaults)
        except Exception as exc:
            log.warning("Could not patch fingerprint.__defaults__: %s", exc)

        # Verify
        try:
            fn = _fp.fingerprint
            log.info("  fingerprint defaults now: Fs=%s wsize=%s wratio=%s fan=%s amp=%s",
                     *fn.__defaults__)
        except Exception:
            pass

    def _clear_fingerprint_store(self) -> None:
        """Wipe dejavu's fingerprint store (backend-agnostic).

        Uses CommonDatabase.empty() (DROP + recreate tables), which
        resets the schema completely — including the auto-increment
        song_id sequence — then VACUUMs the SQLite file to reclaim
        disk space.  Raises on failure so callers can abort a re-index.
        """
        self._djv.db.empty()
        conn = sqlite3.connect(str(self._db_path), timeout=30.0)
        try:
            conn.execute("VACUUM")
            conn.close()
        except Exception:
            try:
                conn.close()
            except Exception:
                pass
            raise
        log.info("clear_all: fingerprint store emptied + VACUUMed")
        self._rebuild_hash_cache()  # now empty

    # -- schema -----------------------------------------------------------
    def _init_schema(self) -> None:
        self._sqlite.execute(
            """
            CREATE TABLE IF NOT EXISTS songs (
                song_id           INTEGER PRIMARY KEY,
                file_path         TEXT NOT NULL UNIQUE,
                fingerprinted_at  INTEGER
            )
            """
        )
        self._sqlite.commit()

    # -- sync & introspection -------------------------------------------
    def _sync_stores(self) -> list[str]:
        """Cross-reference dejavu's song table against the path index.

        **Critical fix** for the "some songs never match even though the
        UI says they're prepared" bug.  Occurs when a previous
        ``clear_all`` or crashed worker left fingerprint rows but path
        mappings (or vice versa) half-empty.

        This method:
          1. Queries dejavu's store for all song_ids that are
             fingerprinted.
          2. Deletes path-index rows whose song_id is NOT in dejavu
             (orphaned mappings from a previous partial clear).
          3. Returns the list of file_paths whose hashes are missing
             from dejavu so the caller can re-index them.

        Called once during :meth:`__init__` to repair drift before
        recognition starts.
        """
        # --- 1. Get dejavu song_ids ---
        try:
            store_ids: set[int] = set()
            rows = self._djv.db.get_songs() or []
            for r in rows:
                if isinstance(r, dict):
                    sid = r.get("song_id")
                elif isinstance(r, (tuple, list)) and r:
                    sid = r[0]
                else:
                    continue
                try:
                    store_ids.add(int(sid))
                except (TypeError, ValueError):
                    continue
            log.info("_sync_stores: dejavu store has %d songs", len(store_ids))
        except Exception as exc:
            log.warning("_sync_stores: dejavu query failed: %s", exc)
            return []

        # --- 2. Get path-index entries ---
        cur = self._sqlite.execute(
            "SELECT song_id, file_path FROM songs ORDER BY song_id"
        )
        sqlite_rows = [(int(r["song_id"]), r["file_path"]) for r in cur.fetchall()]
        sqlite_ids = {sid for sid, _ in sqlite_rows}

        orphaned_sqlite = sqlite_ids - store_ids   # path index has, dejavu doesn't
        missing_hashes = []

        if orphaned_sqlite:
            log.warning(
                "_sync_stores: %d orphaned path-index entries "
                "(song_ids not in dejavu): %s",
                len(orphaned_sqlite), sorted(orphaned_sqlite)[:20],
            )
            # Delete orphans from the index, but collect the file paths
            # so we can re-index them below.
            for sid, fpath in sqlite_rows:
                if sid in orphaned_sqlite:
                    missing_hashes.append(fpath)
            # Batch-delete orphans.
            placeholders = ",".join("?" * len(orphaned_sqlite))
            self._sqlite.execute(
                f"DELETE FROM songs WHERE song_id IN ({placeholders})",
                sorted(orphaned_sqlite),
            )
            self._sqlite.commit()
            log.info("_sync_stores: cleaned %d orphaned path-index rows",
                     len(orphaned_sqlite))

        # --- 3. Reverse check: dejavu has songs the index doesn't know ---
        missing_in_sqlite = store_ids - sqlite_ids
        if missing_in_sqlite:
            log.warning(
                "_sync_stores: %d dejavu songs missing from path index "
                "(path lookup will fail): song_ids=%s",
                len(missing_in_sqlite), sorted(missing_in_sqlite)[:20],
            )

        return missing_hashes

    def list_indexed_files(self) -> list[dict]:
        cur = self._sqlite.execute(
            "SELECT song_id, file_path FROM songs ORDER BY file_path"
        )
        return [dict(r) for r in cur.fetchall()]

    def cancel_indexing(self) -> None:
        """Signal any running index_files/index_library to abort."""
        self._cancel_flag = True

    def stats(self) -> dict:
        cur = self._sqlite.execute("SELECT COUNT(*) AS c FROM songs")
        return {"songs": cur.fetchone()["c"]}

    @staticmethod
    def _walk_audio_files(root: Path) -> list[Path]:
        # soundfile 0.14 natively decodes all five of these — no ffmpeg
        # or pydub needed.  mp3 support is relatively new in soundfile
        # (added via bundled libsndfile), hence the historical ".flac/.wav only"
        # restriction that we're lifting here.
        exts = {".flac", ".wav", ".mp3", ".aiff", ".aif", ".ogg"}
        out: list[Path] = []
        for p in root.rglob("*"):
            if p.is_file() and p.suffix.lower() in exts:
                out.append(p)
        return sorted(out)

    # -- indexing ---------------------------------------------------------
    def _fingerprint_one(self, path_str: str):
        """Compute fingerprints for a single file.

        **CPU-only — does NOT touch the database.**  The fingerprint
        hashes are returned to the caller (the main process), which
        performs the SQLite inserts.  This mirrors dejavu's own
        ``fingerprint_directory`` design and is the only way to safely
        run N workers against a single SQLite file: SQLite allows only
        one writer at a time, so having 12 workers each call
        ``fingerprint_file`` (compute + write) leads to "database is
        locked" errors once the store grows past a few hundred songs.

        Returns ``(song_name, hashes, file_hash, note)`` on success
        (note is None, or a message when the ffmpeg tolerant-decode
        fallback was used) or ``(None, None, None, error_msg)`` on error.
        """
        try:
            from dejavu import Dejavu
            from dejavu.logic import decoder
            # Call get_file_fingerprints directly with print_output=False.
            # _fingerprint_worker passes print_output=True, and in a
            # multiprocessing worker the stdout pipe is never drained —
            # the print buffer fills and the worker deadlocks.
            song_name = decoder.get_audio_name_from_path(path_str)
            hashes, file_hash = Dejavu.get_file_fingerprints(
                path_str, self._djv.limit, print_output=False,
            )
            # Carry back a note if the decoder had to use the ffmpeg
            # fallback (worker-process logs don't reach the main log).
            note = _fallback_notes.pop(os.path.abspath(path_str), None)
            return song_name, hashes, file_hash, note
        except Exception as exc:
            # Return the error message so the MAIN process can log it —
            # worker-process log.error() is often not captured by the
            # main log file under multiprocessing, which made FLAC
            # decode failures (e.g. 24-bit FLAC on old pydub path)
            # silently invisible to users.
            err = f"{type(exc).__name__}: {exc}"
            return None, None, None, err

    def _rebuild_hash_cache(self) -> None:
        """Build file_sha1 → song_id dict from dejavu's songs table."""
        self._song_hash_cache = {}
        try:
            for r in (self._djv.db.get_songs() or []):
                if isinstance(r, dict):
                    sha1 = r.get("file_sha1")
                    sid = r.get("song_id")
                elif isinstance(r, (tuple, list)) and len(r) >= 4:
                    sha1 = r[3]
                    sid = r[0]
                else:
                    continue
                if isinstance(sha1, (bytes, bytearray)):
                    sha1 = sha1.hex().upper()
                if isinstance(sha1, str) and sid is not None:
                    self._song_hash_cache[sha1.upper()] = int(sid)
        except Exception:
            pass

    def _find_song_by_hash(self, file_hash: str):
        """Return an existing song_id whose file_sha1 matches, else None.

        Uses the in-memory cache (O(1)); cache miss falls back to a DB
        query and repopulates the cache.
        """
        if not isinstance(file_hash, str):
            return None
        target = file_hash.upper()
        if target in self._song_hash_cache:
            return self._song_hash_cache[target]
        # Cache miss — rebuild (e.g. another process inserted).
        self._rebuild_hash_cache()
        return self._song_hash_cache.get(target)

    def _insert_fingerprints(self, song_name: str, hashes, file_hash: str):
        """Write fingerprints to the dejavu store (main-process only).

        Safe because this runs in a single process (the Pool's parent).
        Returns the song_id (new or existing) or None on failure.
        """
        try:
            existing = self._find_song_by_hash(file_hash)
            if existing is not None:
                return existing
            sid = self._djv.db.insert_song(song_name, file_hash, len(hashes))
            self._djv.db.insert_hashes(sid, hashes)
            self._djv.db.set_song_fingerprinted(sid)
            if isinstance(file_hash, str):
                self._song_hash_cache[file_hash.upper()] = int(sid)
            return sid
        except Exception as exc:
            log.error("DB insert failed for %s: %s", song_name, exc)
            return None

    @staticmethod
    def _mp_fingerprint_worker(path_str: str) -> tuple:
        """Multiprocessing worker — runs in its OWN Python process.

        Computes fingerprints only (CPU-bound).  Reuses the per-process
        FingerprintDB created by :func:`_init_worker` (pool initializer)
        instead of building a new one per song — the old pattern paid
        the full dejavu-init + hash-cache-rebuild cost for every file.

        Returns ``(file_path, song_name, hashes, file_hash, None)`` on
        success or ``(file_path, None, None, None, error_msg)`` on
        failure.  The parent process performs all DB writes and logs
        the error (worker logging is unreliable under multiprocessing).
        """
        global _worker_db
        try:
            song_name, hashes, file_hash, err = _worker_db._fingerprint_one(path_str)
            if hashes is None:
                return (path_str, None, None, None, err)
            return (path_str, song_name, hashes, file_hash, None)
        except Exception as exc:
            err = f"{type(exc).__name__}: {exc}"
            return (path_str, None, None, None, err)

    def index_files(
        self,
        paths: list[str],
        progress: Optional[ProgressCb] = None,
        workers: Optional[int] = None,
    ) -> int:
        """Parallel fingerprint of a specific file list.

        Subprocesses each have their own FingerprintDB + MySQL connection;
        the main process only writes to SQLite (which can't be shared
        across processes).
        """
        total = len(paths)
        if progress:
            progress(0, total, f"Preparing {total} file(s)")
        self._cancel_flag = False  # reset cancel from any previous run

        known_paths = {r["file_path"] for r in self.list_indexed_files()}

        # Filter out already-known files before dispatching to workers
        todo = [p for p in paths if p not in known_paths]
        skip_count = total - len(todo)

        new_count = 0
        done = 0
        fail_count = 0

        if not todo:
            if progress:
                progress(total, total, f"All {total} already prepared")
            return 0

        n_workers = workers or self._auto_workers()
        log.info("index_files: dispatching %d jobs → %d workers", len(todo), n_workers)

        t0 = time.time()
        with _mp.Pool(n_workers, initializer=_init_worker) as pool:
            result_iter = pool.imap_unordered(
                FingerprintDB._mp_fingerprint_worker, todo, chunksize=4)
            new_count, failed_paths, cancelled = self._drain_index_results(
                pool, result_iter, len(todo), skip_count, total, progress, t0)
        if cancelled:
            return new_count

        # Long tracks need big contiguous FFT arrays (up to ~250 MB
        # complex128); when all workers hit those allocations at the same
        # moment the pool raises MemoryError even though each song fits
        # easily on its own. Retry leftovers one at a time.
        retry_skip = skip_count + len(todo) - len(failed_paths)
        recovered = self._retry_failed_serial(
            failed_paths, retry_skip, total, progress)
        new_count += recovered
        fail_count = len(failed_paths) - recovered

        total_elapsed = time.time() - t0
        if progress:
            progress(
                total, total,
                f"Prepared {new_count} new of {total} "
                f"({skip_count} skipped, {fail_count} failed"
                + (f", {recovered} recovered in low-memory pass" if recovered else "")
                + f") in {total_elapsed:.1f}s",
            )
        return new_count

    def _drain_index_results(
        self, pool, result_iter, n_jobs: int, skip_count: int,
        total: int, progress: Optional[ProgressCb], t0: float,
    ) -> tuple[int, list[str], bool]:
        """Consume ``imap_unordered`` worker results: write DB rows, report.

        Returns ``(new_count, failed_paths, cancelled)``. Main-process-only
        SQLite writes stay serialised here; worker subprocesses never touch
        the DB.
        """
        new_count = 0
        done = 0
        last_t = t0
        failed_paths: list[str] = []
        for path_str, song_name, hashes, file_hash, err in result_iter:
            if self._cancel_flag:
                pool.terminate()
                if progress:
                    progress(done + skip_count, total, "Cancelled by user")
                log.info("indexing: cancelled at %d/%d", done + skip_count, total)
                return new_count, failed_paths, True
            done += 1
            now = time.time()
            elapsed_total = now - t0
            elapsed_this = now - last_t
            last_t = now
            per_done = elapsed_total / done
            eta = per_done * (n_jobs - done)
            fname = Path(path_str).name
            if hashes is not None:
                sid = self._insert_fingerprints(song_name, hashes, file_hash)
                if sid is not None:
                    new_count += 1
                    self._write_sqlite(path_str, sid)
                    status = "Done"
                    if err:  # ffmpeg fallback note from the worker
                        log.warning("Recovered via ffmpeg fallback: %s (%s)",
                                    fname, err)
                else:
                    failed_paths.append(path_str)
                    status = "Failed"
            else:
                failed_paths.append(path_str)
                status = "Failed"
                # Log the actual exception (returned from the worker)
                # so failures are diagnosable — the old code swallowed
                # these and users only saw "Failed: name.flac" with
                # no hint WHY (e.g. 24-bit FLAC decode errors).
                log.error("Failed to fingerprint %s: %s", fname, err or "unknown error")
            msg = (f"{status}: {fname} "
                   f"(+{elapsed_this:.1f}s, {per_done:.1f}s/avg, "
                   f"ETA {eta:.0f}s)")
            if progress:
                progress(done + skip_count, total, msg)
        return new_count, failed_paths, False

    def _retry_failed_serial(
        self, failed_paths: list[str], skip_count: int,
        total: int, progress: Optional[ProgressCb],
    ) -> int:
        """Single-worker retry pass for files that failed in parallel.

        Peak memory drops to one song's worth, so transient MemoryError
        failures (12 workers allocating FFT arrays simultaneously) recover
        without user action. Returns the number of recovered files.
        """
        if not failed_paths or self._cancel_flag:
            return 0
        log.info("Low-memory retry: %d failed file(s) → 1 worker", len(failed_paths))
        recovered = 0
        done = 0
        with _mp.Pool(1, initializer=_init_worker) as pool:
            result_iter = pool.imap_unordered(
                FingerprintDB._mp_fingerprint_worker, failed_paths, chunksize=1)
            for path_str, song_name, hashes, file_hash, err in result_iter:
                if self._cancel_flag:
                    pool.terminate()
                    if progress:
                        progress(skip_count + done, total, "Cancelled by user")
                    log.info("low-memory retry: cancelled at %d/%d",
                             done, len(failed_paths))
                    return recovered
                done += 1
                fname = Path(path_str).name
                ok = False
                if hashes is not None:
                    sid = self._insert_fingerprints(song_name, hashes, file_hash)
                    if sid is not None:
                        self._write_sqlite(path_str, sid)
                        recovered += 1
                        ok = True
                        log.info("Recovered in low-memory pass: %s", fname)
                if not ok:
                    log.error("Still failing in low-memory pass: %s: %s",
                              fname, err or "unknown error")
                if progress:
                    progress(
                        skip_count + done, total,
                        f"Low-memory retry {done}/{len(failed_paths)} "
                        f"({'recovered' if ok else 'still failing'}): {fname}")
        log.info("Low-memory retry recovered %d/%d file(s)",
                 recovered, len(failed_paths))
        return recovered

    def index_library(
        self,
        progress: Optional[ProgressCb] = None,
        workers: Optional[int] = None,
    ) -> int:
        """Parallel fingerprint of every FLAC/WAV under ``music_dir``.

        Returns the number of newly indexed songs.
        """
        files = self._walk_audio_files(Path(SETTINGS.music_dir))
        total = len(files)
        if progress:
            progress(0, total, f"Found {total} FLAC/WAV files")
        self._cancel_flag = False  # reset cancel from any previous run

        known_paths = {r["file_path"] for r in self.list_indexed_files()}
        todo = [str(p) for p in files if str(p) not in known_paths]
        skip_count = total - len(todo)

        if not todo:
            if progress:
                progress(total, total, f"All {total} already indexed")
            return 0

        n_workers = workers or self._auto_workers()
        log.info("index_library: %d songs already indexed, %d new → %d workers",
                 skip_count, len(todo), n_workers)

        if progress:
            progress(skip_count, total,
                     f"Dispatching {len(todo)} new songs to {n_workers} workers")

        t0 = time.time()
        with _mp.Pool(n_workers, initializer=_init_worker) as pool:
            result_iter = pool.imap_unordered(
                FingerprintDB._mp_fingerprint_worker, todo, chunksize=4)
            new_count, failed_paths, cancelled = self._drain_index_results(
                pool, result_iter, len(todo), skip_count, total, progress, t0)
        if cancelled:
            return new_count

        # Memory-pressure leftovers (see index_files) — retry serially.
        retry_skip = skip_count + len(todo) - len(failed_paths)
        recovered = self._retry_failed_serial(
            failed_paths, retry_skip, total, progress)
        new_count += recovered
        fail_count = len(failed_paths) - recovered

        total_elapsed = time.time() - t0
        if progress:
            progress(
                total, total,
                f"Done. {new_count} new, {skip_count} skipped, "
                f"{fail_count} failed"
                + (f" ({recovered} recovered in low-memory pass)" if recovered else "")
                + f". ({total_elapsed:.1f}s)",
            )
        return new_count

    @staticmethod
    def _auto_workers() -> int:
        """Pick a sensible worker count based on CPU cores + available RAM.

        Each worker loads one ~20-120MB track into memory (int16 stereo),
        calls scipy.resample (temporarily doubles to float64), then runs
        dejavu's full-song FFT fingerprint — long tracks allocate a
        contiguous complex128 spectrogram of up to ~250 MB on top of the
        ~115 MB decoded-audio array.  Total per worker is roughly
        600-900 MB peak RSS + ~0.5-1 CPU-second FFT.

        Strategy:
        * Cap at half the logical cores — the main thread and the OS
          also want CPU.
        * Cap at ~800 MB/worker of total RAM — avoid the simultaneous
          allocation spikes that raised MemoryError on 12-worker runs.
        * Always at least 2 so we never run in serial.

        Any files that still fail under parallel memory pressure are
        retried in a single-worker pass (see _retry_failed_serial).
        """
        try:
            cores = os.cpu_count() or 4
        except Exception:
            cores = 4
        try:
            import psutil  # optional — only affects RAM-based clamp
            mem_mb = psutil.virtual_memory().total // (1024 * 1024)
        except Exception:
            mem_mb = None

        cpu_budget = max(2, cores // 2)        # half the cores, min 2
        ram_budget = (mem_mb // 800) if mem_mb else cpu_budget  # 800 MB/worker
        return min(cpu_budget, ram_budget)

    def _write_sqlite(self, file_path: str, song_id: int) -> None:
        """Persist a (song_id → file_path) mapping in the main-process SQLite.

        Subprocesses must NOT touch this connection — SQLite's file locking
        breaks across processes.  We only INSERT here; ``_fingerprint_one``
        in the subprocess already handled the MySQL insert.
        """
        try:
            self._sqlite.execute(
                "INSERT OR IGNORE INTO songs "
                "(song_id, file_path, fingerprinted_at) "
                "VALUES (?, ?, ?)",
                (song_id, file_path, int(time.time())),
            )
            self._sqlite.commit()
        except Exception as exc:
            log.warning("SQLite write failed for %s → %s: %s", file_path, song_id, exc)

    # -- status ----------------------------------------------------------
    def library_status(self) -> dict:
        """Return total/prepared/pending counts for files under ``music_dir``."""
        files = self._walk_audio_files(Path(SETTINGS.music_dir))
        known_paths = {r["file_path"] for r in self.list_indexed_files()}
        prepared = sum(1 for f in files if str(f) in known_paths)
        return {
            "total": len(files),
            "prepared": prepared,
            "pending": len(files) - prepared,
        }

    # -- clear & force re-index -----------------------------------------
    def clear_all(self) -> None:
        """Wipe ALL fingerprints from the dejavu store and path index.

        Used before a full re-index (e.g. after fingerprint-parameter
        changes between runs that left the database in an inconsistent
        state).

        Steps:
          1. DROP + recreate dejavu's tables (``empty()``) — resets the
             schema and the auto-increment song_id sequence.
          2. VACUUM the fingerprint file to reclaim disk space.
          3. Wipe the song_id -> file_path index + VACUUM it too.
        """
        # 1+2. dejavu store (SQLite file in the portable build).
        self._clear_fingerprint_store()

        # --- 3. Clear path index ---
        try:
            self._sqlite.execute("DELETE FROM songs")
            self._sqlite.commit()
            # VACUUM to reclaim the freed pages (SQLite doesn't shrink
            # the file automatically after DELETE).
            self._sqlite.execute("VACUUM")
            log.info("clear_all: path index cleared + VACUUMed")
        except Exception as exc:
            log.warning("clear_all: SQLite clear failed: %s", exc)
            raise

    def force_reindex(self, progress: Optional["ProgressCb"] = None) -> int:
        """clear_all() → index_library().  Returns number of newly indexed songs."""
        log.info("force_reindex: clearing all stored fingerprints...")
        self.clear_all()
        if progress:
            progress(0, 1, "Database cleared.  Re-analyzing all songs...")
        return self.index_library(progress=progress)

    def _sync_from_dejavu(self) -> None:
        """Populate SQLite with entries from dejavu's MySQL songs table.

        dejavu stores songs in MySQL with:
          song_id       - auto-incremented primary key
          song_name     - the file name we passed at fingerprint time (NOT full path)
          file_sha1     - SHA1 hash of the audio file

        Since song_name is only a short file name (e.g. "01. fripSide — Decade"),
        it's NOT a usable file_path and we cannot recover the original file
        path from it alone. This method therefore only updates SQLite if we
        already have a valid full path for a given song_id (from previous
        _fingerprint_one calls that succeeded).

        The real file_path -> song_id mapping is created by _fingerprint_one
        at ingest time. This method is intentionally conservative and will
        never INSERT rows because that would require guessing paths.
        """
        pass  # Intentionally a no-op - see docstring above.

    # -- matching ---------------------------------------------------------
    def match_from_array(
        self,
        samples: "np.ndarray",
        input_sr: int = 44100,
    ) -> MatchResult:
        """Run dejavu directly on in-memory stereo samples — no file I/O.

        Takes ``samples`` as a (N, channels) float32 array (same layout as
        :meth:`AudioCapture.snapshot`) and feeds the two channels straight
        into dejavu's fingerprint pipeline. Bypasses WAV write + WAV read
        + file-hash calculation entirely, shaving ~100-200ms off every
        recognition cycle.

        ``input_sr`` is the sample rate of the captured audio.  Audio
        devices may capture at 48000 Hz even though we index at 44100 Hz;
        if so we resample to TARGET_SR here so the FFT frequency bins line
        up with the indexed hashes.  Without this, a 48 kHz capture fed to
        dejavu as if it were 44.1 kHz would NEVER match.
        """
        import numpy as np
        from scipy import signal

        t0 = time.time()

        # --- 0. Peak-normalise the capture clip -------------------------
        # USB / loopback captures can be very quiet (well below full
        # scale).  dejavu's peak-picker needs spectral peaks above
        # ``amp_min`` dB — if the time-domain signal is tiny, no peaks
        # survive and we get "no hashes generated" even though music IS
        # playing.  Normalising to ~95% of full scale gives the peak
        # picker enough headroom to find peaks in quiet passages.
        # This only affects the fingerprint path; the spectrum display
        # uses the original (un-normalised) levels.
        samples = np.asarray(samples, dtype=np.float32)
        peak = float(np.max(np.abs(samples)))
        if peak > 1e-6:
            samples = samples * (0.95 / peak)

        # --- 1. Convert float32 stereo (N, 2) → int16 channel arrays ---
        # dejavu's fingerprint() expects int16 PCM samples, one 1-D array
        # per channel. Same format MicrophoneRecognizer uses internally.
        if samples.ndim == 1:
            # Mono input — duplicate to stereo (cheap, keeps hash count
            # aligned with our stereo-indexed FLAC library).
            clipped = np.clip(samples, -1.0, 1.0)
            left = (clipped * 32767).astype(np.int16)
            right = left.copy()
            channels = [left, right]
        else:
            clipped = np.clip(samples, -1.0, 1.0)
            channels = [
                (clipped[:, ch] * 32767).astype(np.int16)
                for ch in range(clipped.shape[1])
            ]

        # --- 1b. Resample to TARGET_SR if the capture ran at a ---
        # different rate (e.g. 48 kHz device vs 44.1 kHz index).
        if input_sr != self.TARGET_SR:
            channels, _ = self._resample_channels(
                channels, input_sr, self.TARGET_SR,
            )

        # --- 2. Generate fingerprints for all channels ---
        hashes: set = set()
        for channel in channels:
            channel_hashes, _ = self._djv.generate_fingerprints(
                channel, Fs=self.TARGET_SR,
            )
            hashes |= set(channel_hashes)

        if not hashes:
            # Silence or near-silence in the capture window. Skip query.
            log.info("match_from_array: no hashes generated (silence?)")
            return MatchResult(False, None, 0.0, 0.0, None, {})

        # --- 3. Query MySQL + align matches ---
        matches, dedup_hashes, query_time = self._djv.find_matches(list(hashes))
        t_align = time.time()
        final_results = self._djv.align_matches(matches, dedup_hashes, len(hashes))
        align_time = time.time() - t_align

        total_time = time.time() - t0

        if not final_results:
            log.info("match_from_array: no matches (total_time=%.2fs, query=%.2fs)",
                     total_time, query_time)
            return MatchResult(False, None, 0.0, 0.0, None, {})

        # CRITICAL FIX: dejavu's align_matches sorts by ALIGNED HASH COUNT,
        # not by confidence ratio.  A long 伴奏 track with more total hashes
        # will rank above the real vocal track even when confidence ratio
        # is lower.  Re-sort by input_confidence DESCENDING so we pick the
        # BEST-MATCHING track, not the most-hashes-matching track.
        final_results.sort(
            key=lambda r: float(r.get("input_confidence", 0) or 0),
            reverse=True,
        )

        best = final_results[0]
        song_id = best.get("song_id")
        song_name = best.get("song_name")
        if isinstance(song_name, (bytes, bytearray)):
            song_name = song_name.decode("utf-8", errors="replace")
        confidence = float(best.get("input_confidence", 0) or 0)
        offset = float(best.get("offset_seconds", 0) or 0)

        # Normalise song_id to int.
        try:
            song_id = int(song_id) if song_id is not None else None
        except (TypeError, ValueError):
            log.warning("match_from_array: non-int song_id %r", song_id)
            song_id = None

        file_path: Optional[str] = None
        if song_id is not None:
            cur = self._sqlite.execute(
                "SELECT file_path FROM songs WHERE song_id = ?", (song_id,)
            )
            row = cur.fetchone()
            if row:
                file_path = row["file_path"]
            else:
                file_path = self._resolve_path_from_store(song_id)

        # NOTE: this is NOT the acceptance threshold — it's only the gate
        # for whether dejavu hands back a candidate at all.  The matcher
        # applies its own noise floor (TENTATIVE_CONFIDENCE) and confirm
        # floor (MIN_ACCEPT_CONFIDENCE).  Set this low so real-but-diluted
        # matches (multi-version songs, quiet captures) still reach the
        # matcher's tentative-display logic instead of being silently
        # dropped here.
        MIN_CONFIDENCE = 0.05
        matched = bool(file_path and confidence >= MIN_CONFIDENCE)

        log.info(
            "dejavu match (in-mem): song_id=%s path=%r confidence=%.3f "
            "offset=%.1fs matched=%s [total=%.2fs query=%.2fs align=%.3fs]",
            song_id, file_path, confidence, offset, matched,
            total_time, query_time, align_time,
        )
        if len(final_results) > 1:
            for r in final_results[1:4]:
                sn = r.get("song_name")
                if isinstance(sn, (bytes, bytearray)):
                    sn = sn.decode("utf-8", errors="replace")
                log.info("  also got: song_id=%s name=%r conf=%.3f",
                         r.get("song_id"), sn,
                         float(r.get("input_confidence", 0) or 0))

        return MatchResult(matched, file_path, confidence, offset, song_id, {
            "total_time": total_time,
            "query_time": query_time,
            "align_time": align_time,
            "fingerprint_time": total_time - query_time - align_time,
            "fingerprints_total": len(hashes),
            "results": final_results,
        })

    def match(self, wav_path: str) -> MatchResult:
        """Run dejavu on ``wav_path`` and resolve the matched file path."""
        from dejavu.logic.recognizer.file_recognizer import FileRecognizer

        try:
            recognizer = FileRecognizer(self._djv)
            raw = recognizer.recognize_file(wav_path)
        except Exception as exc:
            log.error("dejavu recognize raised: %s", exc)
            return MatchResult(False, None, 0.0, 0.0, None, {})

        if not raw:
            log.warning("dejavu returned empty result for %s", wav_path)
            return MatchResult(False, None, 0.0, 0.0, None, {})

        # ``raw`` is shaped like:
        #   {"total_time":..., "fingerprint_time":...,
        #    "results": [{"song_id":..., "song_name":...,
        #                 "input_confidence":..., "offset_seconds":...,
        #                 ...}]}
        results = raw.get("results") or []
        if not results:
            log.info("dejavu: no matches (total_time=%.2fs)",
                     raw.get("total_time", 0))
            return MatchResult(False, None, 0.0, 0.0, None, dict(raw))

        best = results[0]
        song_id = best.get("song_id")
        song_name = best.get("song_name")
        # dejavu sometimes returns song_name / file_sha1 as bytes.
        if isinstance(song_name, (bytes, bytearray)):
            song_name = song_name.decode("utf-8", errors="replace")
        confidence = float(best.get("input_confidence", 0) or 0)

        # Normalise song_id to int - dejavu may return numpy int / str.
        try:
            song_id = int(song_id) if song_id is not None else None
        except (TypeError, ValueError):
            log.warning("dejavu returned non-int song_id: %r", song_id)
            song_id = None

        file_path: Optional[str] = None
        if song_id is not None:
            cur = self._sqlite.execute(
                "SELECT file_path FROM songs WHERE song_id = ?", (song_id,)
            )
            row = cur.fetchone()
            if row:
                file_path = row["file_path"]
            else:
                # SQLite doesn't know this song_id yet - fall back to
                # dejavu's MySQL songs table to resolve the path.
                file_path = self._resolve_path_from_store(song_id)

        offset = float(best.get("offset_seconds",
                               best.get("offset", 0)) or 0)
        # dejavu returns confidence in [0, 1].  Default dejavu hashes
        # ~100k per song so full-song matches give 0.5–1.0; with our
        # reduced hash density (~15k/song) and only an 8-second capture
        # clip, real matches land around 0.05–0.20 and false positives
        # cluster below 0.03.  0.08 is a safe floor that catches real
        # matches while still rejecting random hash collisions.
        # NOTE: this is NOT the acceptance threshold — it's only the gate
        # for whether dejavu hands back a candidate at all.  The matcher
        # applies its own noise floor (TENTATIVE_CONFIDENCE) and confirm
        # floor (MIN_ACCEPT_CONFIDENCE).  Set this low so real-but-diluted
        # matches (multi-version songs, quiet captures) still reach the
        # matcher's tentative-display logic instead of being silently
        # dropped here.
        MIN_CONFIDENCE = 0.05
        matched = bool(file_path and confidence >= MIN_CONFIDENCE)
        log.info(
            "dejavu match: song_id=%s song_name=%r path=%r confidence=%.3f offset=%.1fs matched=%s",
            song_id, song_name, file_path, confidence, offset, matched,
        )
        if len(results) > 1:
            for r in results[1:4]:
                sn = r.get("song_name")
                if isinstance(sn, (bytes, bytearray)):
                    sn = sn.decode("utf-8", errors="replace")
                log.info("  also got: song_id=%s name=%r conf=%.3f",
                         r.get("song_id"), sn,
                         float(r.get("input_confidence", 0) or 0))
        return MatchResult(matched, file_path, confidence, offset, song_id, dict(raw))

    def _resolve_path_from_store(self, song_id: int) -> Optional[str]:
        """Fallback: the path index has no row for ``song_id``.

        dejavu stores only the file *name* (not a full path), so a
        missing mapping cannot be reconstructed reliably.  We simply
        re-check the index (the drift repair at startup usually
        prevents this) and return whatever is found.
        """
        try:
            cur = self._sqlite.execute(
                "SELECT file_path FROM songs WHERE song_id = ?",
                (song_id,),
            )
            row = cur.fetchone()
            if row:
                return row["file_path"]
        except Exception as exc:
            log.debug("_resolve_path_from_store failed: %s", exc)
        return None

    def close(self) -> None:
        try:
            self._sqlite.close()
        except Exception:
            pass
