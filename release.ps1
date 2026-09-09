# ================================================================
#  VJVision 自动发布脚本
#  ---------------------------------------------------------------
#  流程：升版本号 → 构建 exe → 提交 → 推送 → 创建 GitHub Release
#        （上传 exe，附带 Release Notes）
#  依赖：Python + PyInstaller，gh CLI 已登录
#  用法：
#    .\release.ps1 -Version 1.1.4 -Notes "修复 xxx"
#    .\release.ps1   # 交互式输入版本号和说明
# ================================================================
param(
    [string]$Version,
    [string]$Notes
)

$ErrorActionPreference = "Stop"

# --- 工作区必须干净 ---
$dirty = git status --porcelain
if ($dirty) {
    Write-Host "[ERROR] 工作区有未提交改动，请先提交或 stash" -ForegroundColor Red
    $dirty
    exit 1
}

# --- 读取当前版本 ---
$initFile = Join-Path $PSScriptRoot "vjvision\__init__.py"
$content = Get-Content $initFile -Raw
if ($content -match '__version__\s*=\s*"([^"]+)"') {
    $curVer = $Matches[1]
} else {
    Write-Host "[ERROR] 无法读取当前版本号" -ForegroundColor Red
    exit 1
}
Write-Host "当前版本: $curVer" -ForegroundColor Cyan

# --- 输入新版本 ---
if (-not $Version) {
    $Version = Read-Host "请输入新版本号 (例如 1.1.4)"
}
if (-not $Version) {
    Write-Host "[ERROR] 版本号不能为空" -ForegroundColor Red
    exit 1
}

# --- 输入 Release Notes ---
if (-not $Notes) {
    Write-Host "请输入 Release Notes (输入空行结束):" -ForegroundColor Yellow
    $lines = @()
    while ($true) {
        $line = Read-Host
        if ($line -eq "") { break }
        $lines += $line
    }
    $Notes = $lines -join "`n"
}
if (-not $Notes) {
    Write-Host "[ERROR] Release Notes 不能为空" -ForegroundColor Red
    exit 1
}

# --- 更新版本号 ---
$newContent = $content -replace '__version__\s*=\s*"[^"]+"', "__version__ = `"$Version`""
# Write UTF-8 WITHOUT BOM (PowerShell 5's Set-Content adds a BOM which
# pollutes Python source files).
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($initFile, $newContent, $utf8NoBom)
Write-Host "版本号已更新为 $Version" -ForegroundColor Green

# --- 构建 exe ---
Write-Host "[1/4] 清理旧构建..." -ForegroundColor Cyan
Remove-Item -Recurse -Force build, dist -ErrorAction SilentlyContinue

Write-Host "[2/4] PyInstaller 构建中 (~1 分钟)..." -ForegroundColor Cyan
# PyInstaller writes progress to stderr, which PowerShell treats as errors
# under strict mode.  Temporarily relax error handling during the build.
$prevEAP = $ErrorActionPreference
$ErrorActionPreference = "Continue"
python -m PyInstaller VJVision.spec --noconfirm --clean
$buildExit = $LASTEXITCODE
$ErrorActionPreference = $prevEAP
if ($buildExit -ne 0) {
    Write-Host "[ERROR] 构建失败" -ForegroundColor Red
    exit 1
}
$exePath = Join-Path $PSScriptRoot "dist\VJVision.exe"
if (-not (Test-Path $exePath)) {
    Write-Host "[ERROR] 未找到产物: $exePath" -ForegroundColor Red
    exit 1
}
$exeSize = [math]::Round((Get-Item $exePath).Length / 1MB, 1)
Write-Host "构建完成: dist\VJVision.exe ($exeSize MB)" -ForegroundColor Green

# --- 提交并推送 ---
Write-Host "[3/4] 提交并推送到 GitHub..." -ForegroundColor Cyan
git add vjvision/__init__.py
git commit -m "release: v$Version"
# Push the CURRENT branch (active development happens on feature branches;
# master is not always the release target).
git push origin HEAD

# --- 创建 GitHub Release ---
Write-Host "[4/4] 创建 GitHub Release..." -ForegroundColor Cyan
$tag = "v$Version"
$notesFile = [System.IO.Path]::GetTempFileName()
Set-Content -Path $notesFile -Value "## v$Version`n`n$Notes" -Encoding UTF8

gh release create $tag $exePath --title "v$Version" --notes-file $notesFile --prerelease
Remove-Item $notesFile -Force

Write-Host ""
Write-Host "=== 发布完成 ===" -ForegroundColor Green
Write-Host "版本: v$Version"
Write-Host "Release: https://github.com/ichiryu0021/VJVision/releases/tag/$tag"
