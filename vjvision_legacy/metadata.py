"""Tag + cover-art extraction using :mod:`mutagen`.

For each matched track we read title/artist/album and dump the embedded
cover art to ``CACHE_DIR/covers``. The visualizer process can then load the
image by path - we cannot pass raw bytes across processes cheaply at 30 FPS.
"""
from __future__ import annotations

import hashlib
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .config import COVER_CACHE

log = logging.getLogger(__name__)


@dataclass
class Track:
    file_path: str
    title: str
    artist: str
    album: str
    cover_path: Optional[str]   # absolute path to cached image, or None


def _save_cover_bytes(data: bytes) -> Optional[str]:
    if not data:
        return None
    digest = hashlib.sha1(data).hexdigest()[:16]
    ext = ".jpg"
    if data.startswith(b"\x89PNG"):
        ext = ".png"
    elif data.startswith(b"\xff\xd8\xff"):
        ext = ".jpg"
    elif data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        ext = ".webp"
    out = COVER_CACHE / f"{digest}{ext}"
    if not out.exists():
        try:
            out.write_bytes(data)
        except OSError as exc:
            log.warning("Could not cache cover: %s", exc)
            return None
    return str(out)


def extract_track(file_path: str) -> Track:
    """Read tags + cover art from a FLAC/WAV file."""
    path = Path(file_path)
    try:
        from mutagen import File as MutagenFile

        audio = MutagenFile(str(path))
    except Exception as exc:
        log.error("Failed to open %s: %s", path, exc)
        return Track(str(path), path.stem, "", "", None)

    if audio is None:
        return Track(str(path), path.stem, "", "", None)

    tags = getattr(audio, "tags", None)

    def vorbis_get(key: str) -> str:
        try:
            v = audio.get(key) if hasattr(audio, "get") else None
            return v[0] if v else ""
        except Exception:
            return ""

    def id3_get(frame_id: str) -> str:
        if not tags:
            return ""
        try:
            f = tags.get(frame_id)
            return str(f) if f else ""
        except Exception:
            return ""

    title = vorbis_get("title") or id3_get("TIT2") or path.stem
    artist = vorbis_get("artist") or id3_get("TPE1") or ""
    album = vorbis_get("album") or id3_get("TALB") or ""

    # ---- Cover art ----------------------------------------------------
    cover_bytes: Optional[bytes] = None

    # FLAC exposes a ``pictures`` list.
    pics = getattr(audio, "pictures", None)
    if pics:
        for pic in pics:
            if getattr(pic, "type", 0) == 3:   # front cover
                cover_bytes = pic.data
                break
        if not cover_bytes:
            cover_bytes = pics[0].data

    # ID3 APIC frames (used by some WAV files).
    if not cover_bytes and tags is not None:
        try:
            for tag in tags.getall("APIC"):
                cover_bytes = getattr(tag, "data", None)
                if cover_bytes:
                    break
        except Exception:
            pass

    cover_path = _save_cover_bytes(cover_bytes) if cover_bytes else None
    return Track(str(path), title, artist, album, cover_path)
