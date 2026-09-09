---
name: "vjvision-release"
description: "VJVision Windows 打包发布全链路：PyInstaller 重打包 exe、冻结环境冒烟验证、提交、代理推送、GitHub Pre-release 上传。当用户要求打包/重新打包 exe、发布新版本、发 Release、或说“打包发给现场”时调用。"
---

# VJVision 打包发布指南（Windows）

从源码改动到 GitHub Release 上资产 `state: uploaded` 的确定性流程。本 skill 汇总三轮真实发布中踩过的全部坑（v1.2.1/v1.3.0/v1.2.2），按顺序执行即可，不要跳步、不要凭"exit code 0"猜成功。

## 0. 环境事实（先记住）

- **Python**：`C:\Users\jason\AppData\Local\Python\pythoncore-3.14-64\python.exe`（3.14.x），`python` 已在 PATH。
- **PowerShell 5.x**：不支持 `&&`，用 `;` 串命令；**`Set-Content -Encoding UTF8` 会写 BOM**（会污染 commit message 主题和 Python 源文件）。
- **沙箱**：PyInstaller 打包、`git push`、`gh release`、启动 GUI exe 都必须 `dangerouslyDisableSandbox: true`。
- **GitHub 直连 443 超时**；本机 Clash 代理在 `127.0.0.1:7890`。
  - git 推送用临时代理参数（不改全局配置）：
    `git -c http.proxy=http://127.0.0.1:7890 -c https.proxy=http://127.0.0.1:7890 push origin feat/gpu-acceleration`
  - gh 用环境变量：`$env:HTTPS_PROXY="http://127.0.0.1:7890"; $env:HTTP_PROXY="http://127.0.0.1:7890"`
- 当前发布分支：`feat/gpu-acceleration`；版本号在 [vjvision/__init__.py](file:///c:/Users/jason/Documents/trae_projects/VJprg/vjvision/__init__.py) 的 `__version__`。
- 打包前**必须问/确认版本号**：用户可能选择小版本迭代（如修复版 1.2.2）而非顺延大版本。

## 1. 打包配置：以 VJVision.spec 为唯一事实来源

- 用 `python -m PyInstaller VJVision.spec --noconfirm --clean`，**不要用 build.bat**：spec 里 `collect_all('imageio_ffmpeg')` 会把 ffmpeg 容错解码二进制（`ffmpeg-win-x86_64-v*.exe`）打入包内；build.bat 的命令行参数漏了它，打出来的 exe 损坏音频兜底会失效。
- spec 产物：单文件 onefile、`console=False`（无黑框），输出 **`dist\VJVision.exe`**（~110 MB）。
- prefs 路径事实（2026-09-09 起）：打包版 prefs 在 **exe 旁 `data\prefs.json`**（便携，跟 data 文件夹走）；`%APPDATA%\VJVision\prefs.json` 仅作旧版迁移源。

## 2. 确定性构建步骤

1. **查无运行中的 exe**（否则 exe 文件被锁，构建静默用旧文件）：
   `tasklist /FI "IMAGENAME eq VJVision.exe"` → 应 "No tasks are running"。
2. **清旧产物**（增量缓存会导致"原地踏步"）：
   `Remove-Item -Recurse -Force build, dist -ErrorAction SilentlyContinue`
3. 构建（耗时约 40-60s）：
   `python -m PyInstaller VJVision.spec --noconfirm --clean 2>&1 | Select-String -Pattern "completed successfully|Build complete|ERROR|error:" | Select-Object -Last 6`
4. **成功标志（两个都要看到）**：
   - `Building EXE from EXE-00.toc completed successfully.`
   - `Build complete! The results are available in: ...\dist`
5. **核验产物三要素**：
   `Get-Item dist\VJVision.exe | Select-Object Length, LastWriteTime`
   - 路径就是 `dist\VJVision.exe`；时间戳是刚刚；体积 ~109-115 MB。
6. **核验 ffmpeg 已打入**（build.bat 漏掉的那个坑）：
   `Select-String -Path build\VJVision\Analysis-00.toc -Pattern "ffmpeg-win"`
   → 必须能看到 `imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v*.exe` 条目。

## 3. 冻结环境冒烟测试（必须真机启动 exe，不能只靠源码单测）

```powershell
Remove-Item -Recurse -Force dist\data -ErrorAction SilentlyContinue   # 干净起步
Start-Process -FilePath ".\dist\VJVision.exe"
Start-Sleep -Seconds 10
Get-Process VJVision -ErrorAction SilentlyContinue   # 期望 3 个进程：主控制台(~150MB) + 可视化子进程(~88MB) + 引导进程
Get-Content ".\dist\data\vjvision.log"              # 冻结版日志在 exe 旁 data\ 下
Stop-Process -Name VJVision -Force -ErrorAction SilentlyContinue
```

- 日志关键行：`Starting visualizer on display ...`（可视化子进程起来了——历史崩溃点）、`Matcher thread started.`、`Opening input stream ...`、`Confidence thresholds: first-track=.. tentative/pulse=.. switch=..`。
- **全程不得有 traceback / Unhandled exception**。
- 需要验证"默认值/配置"时两种场景：
  - 升级迁移：真实 APPDATA 有旧 prefs → 日志应有 `Migrated prefs from legacy ... to portable ...\data\prefs.json`，旧值保留。
  - 全新机器：`$env:APPDATA` 指向一个空临时目录再启动（冻结 exe 依赖全部打包，不受 APPDATA 重定向影响；源码跑 python 才会因此丢 numpy）。
- 验证完**删掉 `dist\data`**（交付纯净单 exe；data 是用户首次运行自建的）。
- **假报错识别**：GUI 运行命令末尾可能出现 `TRAE Sandbox Error: hit restricted ... sRGB Color Space Profile.icm / NVIDIA ...`，这是 pygame/SDL 触碰系统文件被沙箱记录，**不影响测试结论**——以进程存活数和日志内容为准。

## 4. 提交规则

- `git status --short` 确认改动范围；**精确 `git add` 具体文件**，绝不 `git add -A`：
  - `.trae/specs/` 是未跟踪的工作区文件，**排除**；
  - `vjvision/visualizer.py` 长期 stat-dirty（无内容差异），只有 `git diff --stat` 显示真实行数变化时才提交它。
- **commit message 必须无 BOM**：用 .NET API 写临时文件再 `-F` 提交：
  ```powershell
  $msg = @"
  <subject>

  <body>
  "@
  [System.IO.File]::WriteAllText("$env:TEMP\msg.txt", $msg, (New-Object System.Text.UTF8Encoding($false)))
  git commit -F "$env:TEMP\msg.txt"
  ```
  （曾用 `Set-Content -Encoding UTF8` 导致提交主题开头混入 BOM 字符 `﻿`。）
- 若误提交了 BOM 且**尚未推送**：`git reset --soft HEAD~1` 重做；已推送的不要改历史。

## 5. 推送 + GitHub Release

1. 推送：第 0 节的代理 git push。
2. 从 [RELEASE_NOTES.md](file:///c:/Users/jason/Documents/trae_projects/VJprg/RELEASE_NOTES.md) 提取目标版本的双语段落（中文块在前、`### English` 在后，到下一个 `---` 为止）写到临时 notes 文件（同样用无 BOM WriteAllText）。
3. 发 Release（beta 必须 `--prerelease`；tag 指向当前分支）：
   ```powershell
   $env:HTTPS_PROXY="http://127.0.0.1:7890"; $env:HTTP_PROXY="http://127.0.0.1:7890"
   gh release create v1.2.2-beta dist/VJVision.exe --target feat/gpu-acceleration `
     --title "v1.2.2-beta" --notes-file <notes临时文件> --prerelease
   ```
4. **核验资产真正传完**（gh 可能命令返回但上传未完成）：
   ```powershell
   gh release view v1.2.2-beta --json assets,isPrerelease,targetCommitish | ConvertFrom-Json
   ```
   → `isPrerelease: True`、asset `VJVision.exe` ~109.7MB、`state: uploaded`。
5. **mac DMG 不用管**：tag 推送触发 `.github/workflows/build-macos.yml`，云端 Mac（arm64 + x86_64）自动构建 DMG 并 `gh release upload --clobber` 到同一 Release。本地无需 Mac、无需手动上传，也**不要把等 mac 构建当交付阻塞项**（排队可能数十分钟）；Windows exe 是唯一本地产物。
6. 交付时给用户：Release URL + 产物位置 `dist\VJVision.exe`（可直接拷 U 盘）+ 现场回归要点。

## 6. 常见返工原因（都真实发生过）

- 没杀旧 exe / 没删 build,dist → 跑的还是旧包，用户反馈"没变化"。
- 用 build.bat 打包 → ffmpeg 兜底二进制缺失。
- 只看源码单测就宣称成功 → 冻结版的多进程引导/SDL 路径问题只能真机冒烟暴露（v1.2.1 的 8 位图崩溃就是冻结专属）。
- commit message 带 BOM；`git add -A` 混入 `.trae/specs/`。
- 直连 GitHub 443 超时 → 忘记走 7890 代理。
- 版本号想当然顺延 → 用户可能要小版本迭代，发布前确认。
