---
name: "vjvcplus-build"
description: "Build and test workflow for the VJVCPlus C++ project (CMake + MSVC v143, vendored SQLite/FFT, WASAPI capture). Invoke when compiling VJVCPlusCpp, adding new .cpp files, changing CMakeLists, or running index/query/devices/listen CLI tests; also when sandbox errors block the build directory."
---

# VJVCPlus C++ 构建与测试流程

VJVCPlus（C++ 重写版音频指纹识别）的本机构建/测试流程。源码在独立目录，build 产物必须放在本工作区（沙箱限制）。

## 环境事实（本机）

- 源码：`C:\Users\jason\Documents\trae_projects\VJVCPlusCpp`（Write 工具可写；Shell 大部分文件操作被沙箱拦截）
- CMake 便携版：`C:\cmake-4.4.3-windows-x86_64\bin\cmake.exe`（**不在系统 PATH**，每次调用前必须临时加 PATH）
- 工具链：MSVC v143 14.44（VS 2022 BuildTools）+ Windows SDK 26100，generator 用 `"Visual Studio 17 2022" -A x64`
- 依赖全部 vendor：`third_party/pffft/pffft.cpp`（自实现 radix-2 FFT）+ `third_party/sqlite3_extract/sqlite-amalgamation-3490100`。**不要走 vcpkg**（PowerShell Core 下载会超时）。
- **Qt 6.8.3**（M3+ 可视化/面板）：aqtinstall 装在 `C:\Users\jason\Qt\6.8.3\msvc2022_64`。configure 加 `-DCMAKE_PREFIX_PATH=C:/Users/jason/Qt/6.8.3/msvc2022_64`；**运行 vjvcplus.exe 时 PATH 必须含该目录 `bin`**，否则 Qt6 DLL/QML 插件找不到。opengl32sw 兜底包缺包可忽略（有独显）。CMake 选项 `VJVC_WITH_QT=ON/OFF`。

## 关键约束（踩过的坑）

1. **build 目录必须在 VJprg 下**：`C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build`。在 `VJVCPlusCpp\build` 下 configure 会被沙箱拒写 `CMakeCache.txt.tmp`；`dangerouslyDisableSandbox: true` 对 CMake 子进程的 build 写入也无效。
2. **改了 CMakeLists.txt 必须手动重新 configure**：工程设了 `CMAKE_SUPPRESS_REGENERATION`（否则 build 时自动重配会触发沙箱拦截）。增删源文件、改链接库后，先重跑 configure 再 build。
3. **新增 .cpp 文件**：同时做两件事——文件写入 `VJVCPlusCpp\src\...`，并把路径加进 `CMakeLists.txt` 的 `add_executable(vjvcplus ...)`，然后重新 configure。
4. **链接 Windows 系统库**：WASAPI/COM 需要 `ole32`（已在 target_link_libraries 里）；后续用到 `winmm`/`ksuser` 等同样在此追加。
5. **后台跑 exe 并重定向 stdout 到文件**：必须无缓冲输出（main() 里已加 `setvbuf(stdout, nullptr, _IONBF, 0)`），否则 taskkill 强杀时缓冲丢失、日志为空。
6. **PowerShell profile 报错**（`powershell-profile-snapshot.ps1 cannot be loaded ... ExecutionPolicy`）是宿主环境噪音，忽略即可，不影响命令结果。
7. **宽路径**：真实曲库路径含中文/全角字符，std::filesystem 窄字符 API 走 ANSI 会乱码。所有文件 I/O 经 `util/path_util.h`（`utf8ToWide` + `_wfopen_s` / `fs::u8path`）。`toUtf8()` 不要用 `p.u8string()`（MSVC C++17/20 的 char8_t 行为不一致），走 `wideToUtf8(p.wstring())`。path_util.h 已强制 `NOMINMAX`，避免 windows.h 的 min/max 宏污染 `std::min`。
8. **dr_flac 回调**：dr_flac 无宽字符 API，必须用 read/seek/tell 回调 + `_wfopen_s` 的 FILE*。seek 回调必须正确映射全部三种 origin——dr_flac 初始化时会发 `seek(0, DRFLAC_SEEK_END)` + tell 探测文件大小，把 END 误当 CUR 会导致**所有 FLAC open 失败**。dr_mp3 有现成 `drmp3_init_file_w`。实现宏 `DR_FLAC_IMPLEMENTATION`/`DR_MP3_IMPLEMENTATION` 各自只能在一个 .cpp 定义。
9. **并行构建偶发失败**：`-- /m` 下自定义生成步骤（moc/qrc stamp）偶发退出码 268435466，去掉 `/m` 串行重跑即可。
10. **D:\ 盘测试**：沙箱不允许 D:\ 读写，索引真实曲库（`D:\DJMusLib`）时 shell 必须加 `dangerouslyDisableSandbox: true`；测试 DB 仍放 VJprg 下。

## 标准命令

配置（CMakeLists 变更后或首次）：

```powershell
$env:PATH = "C:\cmake-4.4.3-windows-x86_64\bin;" + $env:PATH
cmake -G "Visual Studio 17 2022" -A x64 `
  -S C:\Users\jason\Documents\trae_projects\VJVCPlusCpp `
  -B C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build
```

增量构建（只改 .cpp/.h 时直接跑这个，无需重配）：

```powershell
$env:PATH = "C:\cmake-4.4.3-windows-x86_64\bin;" + $env:PATH
cmake --build C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build --config Release -- /m
```

产物：`C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build\Release\vjvcplus.exe`

## CLI 测试

```powershell
$exe = "C:\Users\jason\Documents\trae_projects\VJprg\vjvcplus_build\Release\vjvcplus.exe"
& $exe list      <db>                     # 列出已索引歌曲
& $exe index     <dir> <db>               # 递归索引 .wav/.flac/.mp3
& $exe query     <audio> <db>             # 单次查询（同样支持 wav/flac/mp3，自查询应 confidence=1.000）
& $exe devices                            # 枚举音频端点（回采在前）
& $exe listen    <db> [device_idx]        # 实时回采识别（Ctrl+C 退出）
& $exe viz       <db> [device_idx] [screen_idx]  # Qt Quick 全屏可视化 + IPC 广播
& $exe            # 无参 = M4 控制面板（GUI，等同 panel 命令）；help 打 CLI 用法
```

- 测试用 WAV：`C:\Users\jason\Music\rekordbox\Sampler\GROOVE CIRCUIT\PRESET\4-Floor Breaks Kit\*.wav`（24-bit/44.1k/stereo，约 13s；解析器支持 16/24/32-bit）。
- 测试 DB 放 VJprg 下，如 `vjvcplus_test.db`。
- index 只接受**目录**参数（单文件要放进单独目录）。

## 实时回采实测套路

listen 是阻塞循环，用 Start-Process 后台跑 + 定时 taskkill，输出重定向到 VJprg 下的日志文件：

```powershell
# 注意：System.Media.SoundPlayer 只播 16-bit PCM；24-bit WAV 需先用 Python/wave 转 16-bit
$p = Start-Process $exe -ArgumentList "listen `"$db`"" `
     -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
     -PassThru -NoNewWindow
Start-Sleep -Seconds 40
taskkill /PID $p.Id /F
Get-Content $log
```

验收基线：循环播放已索引曲目时，12s 窗/4s hop 的 tick 应连续 MATCH 同一首歌；未索引曲目应 0 hits / NO MATCH；无效设备 index 应持续 "device lost, reconnecting" 而不崩溃。

## 指纹管线自检（改动 fp/ 后）

1. 重新 index 测试目录，确认 hashes 数与改动前同量级；
2. 同文件自查询必须 `confidence=1.000`（hash 生成在 index/query 两侧必须一致——曾因入库 `UPPER()`、查表用小写导致 0 hits）；
3. 只索引 A 曲、查 B 曲，必须 `DB hits: 0`（零假阳性）。

## Qt / QML 要点（M3+）

- **运行 viz 前 PATH 加 Qt bin**：`$env:PATH = "C:\Users\jason\Qt\6.8.3\msvc2022_64\bin;" + $env:PATH`，否则报"找不到 Qt6Gui.dll"或 QML 插件静默加载失败。
- **改 QML 不需要重 configure**：qrc 内文件由 AUTORCC 依赖跟踪，普通 build 即可；改 `CMakeLists.txt` / 新增 `Q_OBJECT` 类才需重新 configure。
- **QML 报错排查**：引擎错误走 `[qml]` 前缀输出到 stderr（QtVizSink 已接 `QQmlApplicationEngine::warnings`）；"failed to load QML" 时看这些行。
- **QML 属性命名避开 FINAL**：自定义 property 不能叫 `baseline`/`barH` 等与 QQuickItem 内置 FINAL 属性同名（报 "Cannot override FINAL property"），用语义前缀如 `barBaselineY`。
- **跨线程到 QML**：worker 线程只发 Qt 信号（`sigTrack/sigSpectrum/sigStatus`），AutoConnection 自动变 Queued；不要在 worker 线程直接碰 QObject 成员。
- **QML disk cache 写 AppData 被沙箱拦**是无害警告（`AppData\Local\vjvcplus\cache\qmlcache`），Qt 回退内存缓存，不影响运行。
- **IPC 协议**：命名管道 `\\.\pipe\vjvcplus_viz`，行分隔 JSON（track/spectrum/status 三种）；参考客户端 `VJVCPlusCpp\tools\ipc_test_client.py`，用 Python `open(r'\\.\pipe\vjvcplus_viz')` 直接读。
- viz 是 GUI 全屏程序：用 Start-Process 启动 + taskkill 结束；窗口出现在用户屏幕上，视觉效果需用户目验（CLI 只能验证 stdout 的 `[viz] CONFIRMED` 和无 `[qml]` 报错）。
- **QML console.log 默认不进 stderr**（GUI 子系统只走 OutputDebugString）：qt_viz.cpp 已装 qInstallMessageHandler 转发（tag `[qml-log]`），Start-Process `-RedirectStandardError` 即可收到。
- **MultiEffect（Qt 6.8，本机 qmltypes 核实）四连坑**：① maskSource 必须是纹理源（`layer.enabled:true` 的 item / ShaderEffectSource / Image），普通 Rectangle 采到空纹理整块透明；② **被 MultiEffect/ShaderEffectSource 采样的 `visible:false` 子树里 Image 永远停在 status=Loading(1)**——图片异步解码只对可见 item 跑；纯几何 mask（白色矩形）隐藏无妨；③ `clip:true` 在 Qt6 只按矩形边界裁、**不认 Rectangle 的 radius**；④ 属性名是 **`autoPaddingEnabled`**（不是 autoPadding），也没有标量 `padding`（写错直接 failed to load component），手动 padding 用 `paddingRect: Qt.rect(l,t,r,b)`；`autoPaddingEnabled:false`+自定义 paddingRect 实测会产生屏上巨弧阴影伪影，勿用。**已目验的圆形图片范式**：可见 `Image{ layer.enabled:true; layer.effect: MultiEffect{ maskEnabled:true; maskSource: coverMask } }`（Image 可见→解码正常，屏幕只画蒙版结果）+ 独立 `coverMask`（visible:false/layer.enabled 白圆）+ 底层独立阴影 MultiEffect（1.28× 尺寸、默认 autoPaddingEnabled、只 shadowEnabled，真实圆盖住内缩副本）。
- **锚点不能用 ternary-undefined 清除**：`anchors.left: cond ? x : undefined` 的 undefined 被忽略，旧锚点残留会把 item 拉成屏宽。横竖屏布局用纯 x/y 绑定（width/height/vmin 比例），不要条件 anchors。
- **多屏**：`vjvcplus screens` 枚举（index/name/geometry/DPR/primary）；`viz <db> [dev] <screen_idx>` 选屏，-1=主屏。用户普遍投第二屏且可能竖屏，布局一律相对绑定。
- **截图目验**：System.Drawing CopyFromScreen 存 PNG（Read 工具可看图）；viz 被遮挡时 user32 ShowWindow(SW_MAXIMIZE=3)+SetForegroundWindow 前置，进程 MainWindowHandle 取窗口。识别需播放后 15–50s。
- **夜间长时间目验防睡眠**：起一个隐藏 PowerShell 持锁 `SetThreadExecutionState(0x80000000 -bor 0x1 -bor 0x2 -bor 0x40)`（ES_CONTINUOUS|SYSTEM|DISPLAY|AWAYMODE），结束测完 kill；但用户反映该锁在他机器上不一定生效，最可靠是让用户在电源选项里手动关睡眠/关屏。

## M4 控制面板（Qt Widgets）要点

- 面板/会话/持久化三件套：`src/ui/control_panel.{h,cpp}`（QWidget 主面板）、`src/ui/prefs.{h,cpp}`（exe 旁 `vjvcplus_prefs.json`）、`src/viz/viz_controller.{h,cpp}`（封装 QtVizSink+IpcVizSink+MulticastSink+worker 线程；CLI viz 与面板共用）。面板进程必须用 **QApplication**（不是 QGuiApplication），CMake `find_package(... Widgets)` + 链 `Qt6::Widgets`；改这些要重新 configure。无参启动默认面板。
- viz 全屏后盖住面板：**Esc 退出 viz**（QML Shortcut→`viz.requestClose()`→closeRequested→queued `VizController::stop()`），面板保留；CLI viz 下 Esc 直接退进程。
- **三大生命周期坑（都已踩过并修复，勿回退）**：① 索引 `std::thread` 重赋前必须先 join 旧句柄，否则已结束但 joinable 的移动赋值触发 `std::terminate`（二次索引静默闪退）；worker lambda 必须整体 try/catch；② 索引发现文件时 `fs::path(entry.path()).make_preferred()` 归一原生分隔符，否则 JSON prefs 的前向斜杠目录拼出混合路径，song_paths 精确匹配失效→重复入库；③ `VizController::stop()` 必须全幂等短路（Esc/aboutToQuit/析构三重调用，重发 sessionStopped 重入 quit 会 0xC00000FD 栈溢出+WerFault）。
- **UI Automation 自测**（用户不在场时唯一可行的面板自动化）：PowerShell `Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes`，窗口标题随语言切换（中文 "VJVCPlus 控制面板" / 英文 "VJVCPlus Control Panel"），`RootElement.FindFirst(Children, NameProperty条件)`；按钮 `InvokePattern.Invoke()` 可后台点（viz 全屏盖住也能点 Stop）；QDoubleSpinBox 用 `RangeValuePattern.SetValue`；**Qt 下拉弹窗 SelectionItemPattern.Select 不提交**，要 Expand 后对 ListItem.GetClickablePoint() 用 `SetCursorPos`+`mouse_event(2/4)` 真点；ComboBox 未展开时子项为 0 是正常（懒填充）；关窗用 WindowPattern.Close()；`Start-Process -PassThru` 的 `.ExitCode` 验退出码，崩溃时另有 WerFault 进程；截图仍用 System.Drawing CopyFromScreen。
- 面板自身不打印会话日志（logMessage 进 QPlainTextEdit）；QML/qInstallMessageHandler 的 `[qml]` 行仍走 stderr（-RedirectStandardError 收）。

