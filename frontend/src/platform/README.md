# 平台抽象层（platform seam）

截图 / OCR / 浮层 / 全局热键这四件平台能力的**唯一接缝**。本目录当前只有接口 +
空实现（`NullPlatform`），真实实现留给 `win/` 和 `linux/` 子目录。

```
frontend/src/platform/
├─ Types.h              # 纯数据类型（无平台头）
├─ IPlatform.h          # 接口（无平台头）
├─ NullPlatform.h/.cpp  # 全部 Unsupported 的空实现
├─ PlatformFactory.h/.cpp  # createPlatform() + IPlatform 的三个静态入口
├─ win/.gitkeep         # 以后：DXGI 抓图 / 分层窗口浮层 / RegisterHotKey
├─ linux/.gitkeep       # 以后：X11+XShm / xdg-desktop-portal
└─ README.md
```

## 1. 三条硬约束

**1.1 头文件不许碰平台头。**
`Types.h` 和 `IPlatform.h` 里出现 `windows.h` / `X11` / `xcb` / `dlfcn` / `unistd`
就算违约。只允许 Qt Core / Qt Gui 类型（`QString` `QRect` `QImage` `QVector`
`QStringList`）。

原因：这两个头会被 UI 层 include。一旦把 `windows.h` 拉进来，`min` / `max` 宏
（`NOMINMAX` 没定义时）会把 `std::min` 打死，`UNICODE` 宏会改变 Win32 API 的
重载选择——UI 层会以一种和平台层毫无关系的方式编译失败。

`IPlatform.h` 里的 `QWidget *` 用**前向声明**（`class QWidget;`）而不是
`#include <QWidget>`：这样 `studious_platform` 只链 `Qt6::Core` + `Qt6::Gui`，
不用把 Widgets 拖进平台层。要真正 new 浮层的实现自己在 `.cpp` 里 include。

**1.2 平台层是独立静态库。**
`studious_platform` 的编译选项 / 宏 / 链接库全在它自己的 target 上，
不会污染 `studious` 可执行目标。

**1.3 UI 只认工厂。**
调用方只写：

```cpp
#include "platform/PlatformFactory.h"
auto platform = studious::platform::createPlatform();   // unique_ptr<IPlatform>
```

禁止 `#include "platform/NullPlatform.h"`，禁止 `new NullPlatform`，
禁止在 UI 里 `#ifdef Q_OS_WIN` 分支。换实现只改 `PlatformFactory.cpp` 一处。

## 2. 调用时序契约

```
main()
  └─ IPlatform::initProcessDpi();      // 必须在 QApplication 构造之前，且只调一次
     QApplication app(...);
     auto platform = createPlatform();
     auto caps = platform->capabilities();   // 每一项都先看 available / status 再决定 UI
```

- `initProcessDpi()` 提前的原因：Windows 的 DPI 感知是**一次性**的进程属性，
  `QApplication` 一旦建起来（Qt 内部可能已经设过）再改无效。
- `capabilities()` 是**决策入口**：`available == false` 时不要调对应方法，
  直接把 `reason` 显示给用户（中文、可直接上屏）。
- `virtualDesktopRect()` 返回**空 QRect 是事实**（Wayland 不给全局坐标），
  不是错误，调用方不要当失败处理。
- `captureInteractive()` 的实现铁律：**先抓整屏，再显示遮罩**。顺序反了遮罩会被
  自己拍进截图（"截图里有截图框"的经典 bug）。`cancelInteractive()` 用于 ESC / 超时。
- `createOverlay()` 返回 `nullptr` = 不支持浮层，调用方降级（普通窗口 / 状态栏）。
  返回的 `QWidget` 所有权**归平台层**，调用方只 hide/deleteLater，不 delete。
- `recognize()` **同步阻塞**，必须在工作线程调，GUI 线程直调会卡死界面。
  与 `TranslateJob` 的线程模型一致：跨线程只搬纯数据（`OcrResult` 可拷贝）。
- `registerHotkey()` 返回 `false` = 注册失败（被占用 / 不支持），调用方回滚 UI 状态；
  `unregisterHotkey(id)` 用调用方自己给的 `id` 配对，平台层不生成 id。

## 3. NullPlatform 的行为

| 方法 | 返回 |
|---|---|
| `capabilities()` | 6 项全 `{false, Unsupported, "当前平台尚未实现"}`，`backendName = "null"` |
| `backendName()` | `"null"` |
| `monitors()` | 空 `QVector` |
| `virtualDesktopRect()` | 空 `QRect` |
| `captureRect` / `captureMonitor` / `captureInteractive` | `status = Unsupported`，`error = "当前平台尚未实现"` |
| `cancelInteractive()` | 无操作 |
| `createOverlay()` | `nullptr` |
| `registerHotkey()` | `false` |
| `ocrLanguages()` | 空 |
| `recognize()` | `status = Unsupported`，`error` 同上 |

## 4. 加真实实现时改哪

1. `src/platform/win/`（或 `linux/`）下写实现类，`include "platform/IPlatform.h"`。
2. `frontend/CMakeLists.txt` 里 `if(WIN32) / elseif(UNIX)` 分支加 `target_sources` +
   `target_link_libraries`（分支已经留好，含建议的链接库注释）。
3. `PlatformFactory.cpp` 的 `createPlatform()` 里加 `#ifdef` 分支。
4. UI 一行不动。

平台相关的资产命名走 `IPlatform::runtimeAssetSuffix()`（`win-x64` / `linux-x64`）和
`IPlatform::serverExecutableName()`（`llama-server.exe` / `llama-server`），
目的是把 UI 里写死的 `win-x64` / `llama-server.exe` 收口到这里。

## 5. 构建

```powershell
$env:Path = "C:\Code\language\Qt\Tools\mingw1310_64\bin;C:\Code\language\Qt\Tools\CMake_64\bin;C:\Code\language\Qt\Tools\Ninja;C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path
cmake -S frontend -B frontend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build frontend/build --target studious_platform
```

产物：`frontend/build/libstudious_platform.a`。
