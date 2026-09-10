"""USB-soundcard *and* WASAPI-loopback audio capture.

Built on :mod:`pyaudiowpatch` — a PyAudio superset that transparently
adds ``[Loopback]`` virtual input devices for every WASAPI render
endpoint.  This lets DJs capture either:

* a real input device (microphone, USB mixer input, ...), or
* a playback device (their DAW / local DJ software's speaker output)
  without any virtual-cable middleware.

Internally this module mirrors the original sounddevice-based design:
one ring buffer fed by a PyAudio callback, a level meter, and a
spectrum pipeline.  Only the *transport layer* changed — the consumer
API (:meth:`snapshot`, level/spectrum callbacks) is identical.
"""
from __future__ import annotations

import logging
import threading
import time
from pathlib import Path
from typing import Callable, Optional

import numpy as np
import soundfile as sf

try:
    import pyaudiowpatch as pyaudio
    _HAS_PYAWP = True
except ImportError:  # pragma: no cover
    # Fallback to vanilla pyaudio — no loopback devices will appear but
    # normal input capture still works.
    try:
        import pyaudio
        _HAS_PYAWP = False
    except ImportError:
        pyaudio = None
        _HAS_PYAWP = False

from .config import SETTINGS, TEMP_AUDIO

log = logging.getLogger(__name__)


SpectrumCallback = Callable[[np.ndarray, float], None]


class AudioUnavailableError(RuntimeError):
    """Raised when no audio backend/device can be initialised.

    Machines with no audio hardware (sound card disabled, no drivers,
    audio service stopped) make PortAudio initialisation fail — callers
    must treat this as a non-fatal condition: log a clear message and
    keep the rest of the app running instead of crashing.
    """


class AudioCapture:
    """Thread-safe ring buffer fed by a :class:`pyaudio.Stream`.

    ``pyaudio.PyAudio()`` is created once in :meth:`__init__` and shared
    across the object's lifetime — this avoids the 200-300 ms WASAPI
    initialisation cost on every :meth:`start` call.
    """

    def __init__(
        self,
        device: Optional[int | str] = None,
        on_spectrum: Optional[SpectrumCallback] = None,
    ) -> None:
        if pyaudio is None:
            raise RuntimeError(
                "Neither pyaudiowpatch nor pyaudio is installed — "
                "run: pip install pyaudiowpatch"
            )

        cfg = SETTINGS.capture
        self.device = device if device is not None else SETTINGS.audio_device
        self.sr = cfg.sample_rate
        self.channels = cfg.channels
        self.block = cfg.block_size
        self.match_seconds = cfg.match_seconds
        self.spectrum_fps = cfg.spectrum_fps
        self.spectrum_bins = cfg.spectrum_bins

        # One-time PyAudio instance.  Creating it is expensive on WASAPI
        # (COM + audio session setup) so we keep it alive.  On machines
        # with no audio hardware/drivers Pa_Initialize can fail — surface
        # that as AudioUnavailableError so callers can degrade gracefully
        # instead of taking the whole thread/process down.
        try:
            self._pa = pyaudio.PyAudio()
        except Exception as exc:
            raise AudioUnavailableError(
                f"Audio backend initialisation failed (no audio device "
                f"or driver available?): {exc}"
            ) from exc
        self._pa_owned = True  # so __del__ terminates it

        # Resolve default sample rate + max channels for the chosen device.
        # WASAPI loopbacks reject foreign SRs (PaErrorCode -9997), and some
        # mics only expose 1 input channel even though cfg says 2
        # (PaErrorCode -9998).  Using the device's own defaults avoids both.
        self._effective_sr = self.sr
        self._is_loopback = False
        self._device_channels = self.channels
        if self.device is None:
            # PortAudio picks the system default input — no index to probe.
            log.info("device=None → PortAudio system default input")
        else:
            try:
                dev_info = self._pa.get_device_info_by_index(int(self.device))
                self._effective_sr = int(dev_info.get("defaultSampleRate", self.sr))
                self._is_loopback = bool(dev_info.get("isLoopbackDevice", False))
                max_ch = int(dev_info.get("maxInputChannels", self.channels))
                self._device_channels = max(1, min(self.channels, max_ch))

                if self._effective_sr != self.sr or self._device_channels != self.channels:
                    log.info(
                        "Device %s defaults: SR=%d (cfg=%d)  channels=%d (cfg=%d)",
                        dev_info["name"], self._effective_sr, self.sr,
                        self._device_channels, self.channels,
                    )
                    self.sr = self._effective_sr
                    self.channels = self._device_channels
            except Exception as exc:
                log.warning("Couldn't probe device %s: %s", self.device, exc)

        # Circular buffer — sized at *effective* sample rate.
        self._buf = np.zeros((self.sr * self.match_seconds, self.channels),
                             dtype=np.float32)
        self._write_pos = 0
        self._lock = threading.Lock()

        self._on_spectrum = on_spectrum
        self._last_spectrum_ts = 0.0
        self._spectrum_period = 1.0 / self.spectrum_fps
        self._fft_window = np.hanning(self.block) if self.block else None
        self._spec_ref: float = 0.0
        self._spec_prev: Optional[np.ndarray] = None
        self._noise_floor: float = -60.0

        self._stream: Optional["pyaudio.Stream"] = None
        self._running = threading.Event()

        # Monitor-mode gate — see comment in original sounddevice version.
        self.spectrum_enabled: bool = True

        self._peak: float = 0.0
        self._rms: float = 0.0
        self._peak_decay: float = 0.0
        self._signal_seen: bool = False
        self._clip_count: int = 0

    # -- device enumeration ------------------------------------------------
    @staticmethod
    def list_input_devices() -> list[dict]:
        """Return all input-capable devices **including WASAPI loopbacks**.

        Loopback devices are suffixed with ``（回采）`` so DJs can tell
        them apart from the matching "real" output device.
        """
        if pyaudio is None:
            return []
        try:
            pa = pyaudio.PyAudio()
        except Exception as exc:  # pragma: no cover
            log.error("Failed to create PyAudio: %s", exc)
            return []

        out: list[dict] = []
        try:
            try:
                device_count = pa.get_device_count()
            except Exception as exc:  # pragma: no cover
                log.error("PortAudio get_device_count failed: %s", exc)
                device_count = 0
            for i in range(device_count):
                try:
                    d = pa.get_device_info_by_index(i)
                except Exception:
                    continue
                max_in = int(d.get("maxInputChannels", 0))
                if max_in <= 0:
                    continue
                hai = int(d.get("hostApi", 0))
                try:
                    ha_name = pa.get_host_api_info_by_index(hai)["name"]
                except Exception:
                    ha_name = "unknown"
                is_loop = bool(d.get("isLoopbackDevice", False))
                name = d["name"]
                if is_loop:
                    # Strip the library's own " [Loopback]" tag and add
                    # our own （回采） suffix so DJs instantly understand
                    # what this device does.
                    name = name.replace("[Loopback]", "").strip() + "（回采）"
                out.append({
                    "index": i,
                    "name": name,
                    "host_api": ha_name,
                    "channels": max_in,
                    "default_sr": int(d.get("defaultSampleRate", 44100)),
                    "is_loopback": is_loop,
                })
        finally:
            try:
                pa.terminate()
            except Exception as exc:  # pragma: no cover
                log.warning("PyAudio terminate failed during enum: %s", exc)
        return out

    @staticmethod
    def default_usb_device() -> Optional[int]:
        """Pick the first non-loopback input whose name contains 'USB'."""
        for dev in AudioCapture.list_input_devices():
            if dev.get("is_loopback"):
                continue
            if "usb" in dev["name"].lower():
                return dev["index"]
        return None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        if self._stream is not None:
            return
        self._running.set()
        kind = "loopback" if self._is_loopback else "input"
        log.info(
            "Opening %s stream device=%s sr=%d channels=%d block=%d",
            kind, self.device, self.sr, self.channels, self.block,
        )

        # PyAudio callback signature: (in_data: bytes, frame_count,
        # time_info, status) -> (in_data, paContinue).  We receive
        # int16 bytes from the hardware and convert to float32 here.
        def _pa_callback(in_data, frame_count, time_info, status):
            if status:
                log.debug("pa stream status: %s", status)
            if not self._running.is_set() or in_data is None:
                return (None, pyaudio.paContinue)

            # bytes -> (frames, channels) float32
            arr = np.frombuffer(in_data, dtype=np.int16).astype(np.float32) / 32768.0
            try:
                arr = arr.reshape(frame_count, self.channels)
            except Exception:
                arr = arr[:frame_count * self.channels].reshape(-1, self.channels)

            self._audio_callback(arr)
            return (None, pyaudio.paContinue)

        # Omit input_device_index entirely when no device is selected —
        # int(None) used to raise TypeError here; PortAudio then picks the
        # system default input (and raises a catchable OSError when no
        # default device exists on audio-less machines).
        open_kwargs = dict(
            channels=self.channels,
            rate=self.sr,
            format=pyaudio.paInt16,
            input=True,
            frames_per_buffer=self.block,
            stream_callback=_pa_callback,
        )
        if self.device is not None:
            try:
                open_kwargs["input_device_index"] = int(self.device)
            except (TypeError, ValueError):
                pass
        self._stream = self._pa.open(**open_kwargs)
        self._stream.start_stream()

    def stop(self) -> None:
        self._running.clear()
        if self._stream is not None:
            try:
                self._stream.stop_stream()
                self._stream.close()
            except Exception as exc:
                log.warning("Error closing stream: %s", exc)
        self._stream = None

    def close(self) -> None:
        """Full teardown (also terminates the shared PyAudio instance)."""
        self.stop()
        if self._pa_owned and self._pa is not None:
            try:
                self._pa.terminate()
            except Exception as exc:
                log.warning("Error terminating PyAudio: %s", exc)
            self._pa = None
            self._pa_owned = False

    # -- audio processing (shared with the old sounddevice version) -------
    def _audio_callback(self, indata: np.ndarray) -> None:
        """The *actual* processing pipeline.

        ``indata`` is already (frames, channels) float32 — identical
        to what the old ``sd.InputStream`` callback delivered, so all
        downstream logic stays unchanged.
        """
        mono = indata.mean(axis=1) if self.channels > 1 else indata[:, 0]

        try:
            block_peak = float(np.abs(mono).max()) if mono.size else 0.0
            block_rms = float(np.sqrt((mono * mono).mean())) if mono.size else 0.0
        except Exception:
            block_peak = block_rms = 0.0
        self._peak = block_peak
        self._rms = block_rms
        self._peak_decay = max(self._peak_decay * 0.92, block_peak)
        if block_peak > 0.001:
            self._signal_seen = True
        if block_peak >= 0.99:
            self._clip_count += 1

        with self._lock:
            n = indata.shape[0]
            buf_len = self._buf.shape[0]
            end = self._write_pos + n
            if end <= buf_len:
                self._buf[self._write_pos:end] = indata[:, :self.channels]
            else:
                first = buf_len - self._write_pos
                self._buf[self._write_pos:] = indata[:first, :self.channels]
                self._buf[:n - first] = indata[first:, :self.channels]
            self._write_pos = end % buf_len

        now = time.monotonic()
        if (self.spectrum_enabled and self._on_spectrum
                and (now - self._last_spectrum_ts) >= self._spectrum_period):
            self._last_spectrum_ts = now
            self._emit_spectrum(mono)

    # -- level meter ------------------------------------------------------
    def current_level(self) -> dict:
        # is_active()/is_stopped() can raise once the underlying device is
        # gone (USB soundcard unplugged, audio service stopped mid-stream)
        # — report "inactive" in that case instead of propagating.
        active = False
        if self._stream is not None:
            try:
                active = bool(self._stream.is_active())
            except Exception:
                active = False
        return {
            "peak": max(0.0, min(1.0, self._peak)),
            "rms": max(0.0, min(1.0, self._rms)),
            "peak_hold": max(0.0, min(1.0, self._peak_decay)),
            "signal": self._signal_seen,
            "clips": self._clip_count,
            "active": active,
        }

    SILENCE_THRESHOLD = 0.002
    _MIN_FREQ_HZ = 80.0

    def _emit_spectrum(self, block: np.ndarray) -> None:
        if block.size < 2:
            return
        rms = float(np.sqrt((block * block).mean())) if block.size else 0.0
        peak_amp = float(np.abs(block).max()) if block.size else 0.0
        if peak_amp < self.SILENCE_THRESHOLD:
            if self._spec_prev is not None:
                self._spec_prev = self._spec_prev * 0.92
                bins_out = self._spec_prev.astype(np.float32)
            else:
                bins_out = np.zeros(self.spectrum_bins, dtype=np.float32)
                self._spec_prev = bins_out.copy()
            try:
                self._on_spectrum(bins_out, 0.0)
            except Exception as exc:
                log.error("spectrum callback raised: %s", exc)
            return

        win = self._fft_window[: block.size] if self._fft_window is not None else None
        sig = block * win if win is not None else block
        spec = np.abs(np.fft.rfft(sig))
        spec_db = 20.0 * np.log10(spec + 1e-10)

        n_fft = spec_db.size
        fft_hz = np.linspace(0.0, self.sr / 2, n_fft, dtype=np.float64)

        def hz_to_mel(f):
            return 2595.0 * np.log10(1.0 + f / 700.0)
        def mel_to_hz(m):
            return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

        min_hz = self._MIN_FREQ_HZ
        min_mel = hz_to_mel(min_hz)
        max_mel = hz_to_mel(self.sr / 2.0)
        start_idx = int(np.searchsorted(fft_hz, min_hz))
        if start_idx >= n_fft - 1:
            start_idx = max(3, n_fft // 4)

        mel_edges = np.linspace(min_mel, max_mel,
                                self.spectrum_bins + 1, dtype=np.float64)
        hz_edges = mel_to_hz(mel_edges)
        bin_edges = np.searchsorted(fft_hz, hz_edges).astype(int)
        bin_edges[0] = start_idx
        bin_edges[-1] = n_fft

        bins = np.zeros(self.spectrum_bins, dtype=np.float64)
        for i in range(self.spectrum_bins):
            a, b = bin_edges[i], bin_edges[i + 1]
            if b > a:
                bins[i] = float(spec_db[a:b].mean())
            elif b == a and a < n_fft:
                bins[i] = float(spec_db[a])

        peak_db = bins.max()
        cur_floor = float(np.percentile(bins, 10))
        if not np.isfinite(cur_floor):
            cur_floor = self._noise_floor
        self._noise_floor = self._noise_floor * 0.95 + cur_floor * 0.05

        span = max(1.0, peak_db - self._noise_floor)
        bins_norm = (bins - self._noise_floor) / span
        bins_norm = np.clip(bins_norm, 0.0, 1.0)
        bins_norm = np.nan_to_num(bins_norm, nan=0.0, posinf=1.0, neginf=0.0)

        if self._spec_prev is not None and self._spec_prev.shape == bins_norm.shape:
            rising = bins_norm > self._spec_prev
            alpha = np.where(rising, 0.60, 0.08).astype(np.float32)
            new_vals = alpha * bins_norm + (1.0 - alpha) * self._spec_prev
            bins_norm = new_vals
        self._spec_prev = bins_norm.copy()

        bins_norm = bins_norm.astype(np.float32)

        try:
            self._on_spectrum(bins_norm, rms)
        except Exception as exc:
            log.error("spectrum callback raised: %s", exc)

    # -- consumers ---------------------------------------------------------
    def snapshot(self) -> np.ndarray:
        with self._lock:
            buf = self._buf.copy()
        return np.roll(buf, -self._write_pos, axis=0).astype(np.float32)

    def save_temp_wav(self) -> Path:
        data = self.snapshot()
        clipped = np.clip(data, -1.0, 1.0)
        sf.write(str(TEMP_AUDIO), clipped, self.sr, subtype="PCM_16")
        return TEMP_AUDIO
