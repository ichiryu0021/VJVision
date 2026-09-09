"""VJVision: real-time audio-reactive visualizer for DJ sets.

Top-level package re-exports the most useful entry points.
"""
from __future__ import annotations

__version__ = "1.3.0-beta"

# Re-export Settings singleton (used by every module).
from . import config  # noqa: F401  (ensures config side-effects run first)
