@echo off
REM ================================================================
REM  VJVision 打包脚本（Windows onefile — 单 exe 便携版）
REM  ---------------------------------------------------------------
REM  依赖：Python 3.14 + PyInstaller (`pip install pyinstaller`)
REM  产物：dist-onefile\VJVision.exe（仅 1 个文件，80MB）
REM  使用：拷这一个 exe 到 U 盘，双击即可。
REM        exe 旁会自动创建 data\ 存 prefs / 指纹库 / 日志 / 封面缓存
REM  注意：首次运行无 prefs → 自动 fallback 到 device=0（默认麦克风）
REM        重新打包前请先关闭所有 VJVision.exe 进程
REM ================================================================

setlocal enabledelayedexpansion

echo.
echo === VJVision build (onefile) ===
echo.

where python >nul 2>&1 || (echo [ERROR] python not on PATH & exit /b 1)

REM 检查 PyInstaller
python -c "import PyInstaller" >nul 2>&1 || (
    echo [INFO] PyInstaller not installed, installing...
    python -m pip install pyinstaller || (echo [ERROR] pip install failed & exit /b 1)
)

echo [INFO] Cleaning previous build...
if exist build rmdir /s /q build
if exist dist-onefile rmdir /s /q dist-onefile

echo [INFO] Starting PyInstaller (onefile, this takes ~1 min)...
python -m PyInstaller ^
    --clean --noconfirm ^
    --onefile --windowed ^
    --name "VJVision" ^
    --collect-data customtkinter ^
    --collect-all sounddevice ^
    --collect-all soundfile ^
    --hidden-import vjvision.dejavu_sqlite ^
    --hidden-import dejavu ^
    --hidden-import dejavu.logic ^
    --hidden-import dejavu.logic.fingerprint ^
    --hidden-import dejavu.logic.recognizer ^
    --hidden-import dejavu.database_handler ^
    --hidden-import dejavu.third_party ^
    --hidden-import scipy ^
    --hidden-import scipy.signal ^
    --hidden-import customtkinter ^
    --hidden-import sounddevice ^
    --hidden-import soundfile ^
    --hidden-import pygame ^
    --hidden-import mutagen ^
    --distpath dist-onefile ^
    --workpath build ^
    main.py

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!  See build output above.
    exit /b 1
)

echo.
echo === SUCCESS ===
echo Single-EXE output: dist-onefile\VJVision.exe
echo.
echo Usage: copy THIS ONE FILE to a USB stick. Double-click to run.
echo The first run creates a sibling "data\" folder automatically.
echo Analyze songs, then copy the exe + data\ folder together —
echo that's your portable VJ package.
echo.
echo Trade-off (onefile vs onedir):
echo   + Single file, trivial to distribute
echo   - Startup is ~3 seconds slower (self-extract to temp)
echo   - Antivirus may flag (common false-positive)
echo.
pause
