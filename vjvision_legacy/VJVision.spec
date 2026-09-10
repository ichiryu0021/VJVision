# -*- mode: python ; coding: utf-8 -*-
import sys as _sys
from PyInstaller.utils.hooks import collect_data_files
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = []
hiddenimports = ['vjvision.dejavu_sqlite', 'dejavu', 'dejavu.logic', 'dejavu.logic.fingerprint', 'dejavu.logic.recognizer', 'dejavu.database_handler', 'dejavu.third_party', 'scipy', 'scipy.signal', 'customtkinter', 'sounddevice', 'soundfile', 'pygame', 'mutagen', 'imageio_ffmpeg']
datas += collect_data_files('customtkinter')
tmp_ret = collect_all('sounddevice')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('soundfile')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
# Bundled static ffmpeg binary (tolerant-decode fallback) — the executable
# ships inside the package data, so collect everything.
tmp_ret = collect_all('imageio_ffmpeg')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='VJVision',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)

# macOS: wrap the binary into a proper VJVision.app bundle.  ``BUNDLE``
# only exists in the spec namespace when PyInstaller runs on macOS, so
# guard it — the same spec file is used for the Windows .exe build.
if _sys.platform == "darwin":
    app = BUNDLE(
        exe,
        name='VJVision.app',
        icon=None,
        bundle_identifier='com.ichiryu0021.vjvision',
    )
