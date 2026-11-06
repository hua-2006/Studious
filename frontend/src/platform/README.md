# 平台层（platform seam）

所有与操作系统耦合的代码只允许出现在这个目录。这条规则不是风格问题，是构建约束：

- `backend/` 有一条硬约束「零平台头依赖」（`backend/src` 里扫不到 `windows.h` / `X11/*` / `wayland-*`）。
- QML 与 `AppBackend` 只认这里的接口，永远不 `#include` 平台头。
- 于是「换平台」= 改这个目录，别处一行不动。

## 文件

| 文件 | 作用 | 平台头 |
|---|---|---|
| `Types.h` | 纯数据类型：`CaptureRect` / `CaptureResult` / `CapabilityReport` / `OcrWord` / `HotkeySpec` / `OverlayOptions` … | ❌ 一个都没有 |
| `IPlatform.h` | 能力接口 + 三个静态工厂函数 | ❌（只前向声明 `QWindow`） |
| `NullPlatform.*` | 空实现：全部 `Unsupported`，调用方据此降级 | ❌ |
| `PlatformFactory.*` | `createPlatform()` 按 `Q_OS_*` 分支；`initProcessDpi()` / `runtimeAssetSuffix()` / `serverExecutableName()` 也在这里 | ❌ |
| `Desktop.*` | 托盘图标、开机自启、单实例、正常退出 | ✅ `windows.h` + `shellapi.h`（`#ifdef Q_OS_WIN` 内） |
| `ProcessGuard.*` | 子进程孤儿兜底（Job Object / 未来的 `PR_SET_PDEATHSIG`） | ✅ `windows.h`（同上） |

**规矩**：本目录下只有 `*.cpp` 可以出现平台头；`*.h` 里只允许 `void *` 句柄和前向声明。这条已经被 `ProcessGuard.h` 的写法验证过一遍。

## 平台能力现状

| 能力 | Windows | Linux / 类 Unix | 说明 |
|---|---|---|---|
| 抓屏 / 框选 | ⬜ 未实现 | ⬜ 未实现 | 接口就绪；计划 GDI→DXGI，Linux 走 X11 / Wayland portal |
| 全局热键 | ⬜ 未实现 | ⬜ 未实现 | 复用 `Desktop` 的消息窗口挂 `WM_HOTKEY`；Linux 走 XGrabKey / portal |
| 结果浮层 | ⬜ 未实现 | ⬜ 未实现 | `createOverlay()` 返回 `QWindow*`（Qt Quick 下用 `QQuickWindow` 画） |
| OCR | ⬜ 未实现 | ⬜ 未实现 | 计划 ONNX（det + rec），与平台无关，一张模型三平台通用 |
| 托盘图标 | ✅ `Shell_NotifyIcon` | ⬜ 未实现（`trayAvailable()==false`，界面自动置灰） | 手搓，不链 QtWidgets |
| 开机自启 | ✅ `HKCU\...\Run` | ✅ `$XDG_CONFIG_HOME/autostart/studious.desktop` | 两套都真实可用 |
| 单实例 | ✅ `QLocalServer` | ✅ 同左（Unix socket） | 先 ping 再 `listen`，绝不先 `removeServer` |
| 孤儿进程兜底 | ✅ Job Object + `KILL_ON_JOB_CLOSE` | ⬜ 未实现（等 Linux 运行时资产落地） | 见 `ProcessGuard.cpp` |

## 静态入口（去掉各处硬编码）

```
IPlatform::runtimeAssetSuffix()    // "win-x64" / "linux-x64" / "macos-arm64"
IPlatform::serverExecutableName()  // "llama-server.exe" / "llama-server"
IPlatform::initProcessDpi()        // 必须在 QGuiApplication 构造之前调用
```

`LocalEngine` 曾经把 `runtime.llama.cpu.win-x64` 和 `llama-server.exe` 写死，Linux 上永远报「缺运行时」；
现在这两个字符串全部由上面两个函数拼出来（`LocalEngine.cpp` 的 `cpuRuntimeId()` / `vulkanRuntimeId()` /
`serverExeName()`），资产 id 与 manifest 里的 `runtime.llama.*.<平台后缀>` 自动对齐。

## 加一个平台要做什么

1. 新建 `WinPlatform.cpp` / `X11Platform.cpp`（实现 `IPlatform`，平台头只写在 `.cpp`）。
2. 在 `PlatformFactory.cpp` 的 `createPlatform()` 里加一支 `#ifdef`。
3. 在 `NullPlatform` 已有的「不支持就返回 Unsupported」语义上，逐项把 `capabilities()` 改成 `available = true`。
4. 界面**不需要改**：功能入口读 `capabilities()`，不支持就置灰并显示 `reason`。

## 已知遗留

- manifest 里 `runtime.llama.vulkan.linux-x64` 的 `urls` 是空的（占位条目），装了也没东西可下；
  `runtime.llama.cpu.linux-x64` 干脆没有。Linux 本地 LLM 要能用，得先补这两条真地址 + 真 sha256。
- onnxruntime 只 vendored 了 Windows 版；Linux 需要 `libonnxruntime.so` 交给 `backend/CMakeLists.txt` 的 `find_library`。
- Wayland 下的抓屏只能走 PipeWire portal（每次询问用户），实时翻译在 Wayland 上建议不做。
