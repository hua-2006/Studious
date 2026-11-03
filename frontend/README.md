# Studious 前端 Demo

纯文本翻译测试客户端（Qt 6 Widgets）。**不做截图 / OCR / 浮层**，只验证
`文本输入 → 后端 router → 译文输出` 这条链路，同时提供 `--smoke` 无界面冒烟模式。

后端是 `backend/` 下的静态库（本目录**不修改**后端任何文件）。

---

## 1. 构建

```powershell
$env:Path = "C:\Code\language\Qt\Tools\mingw1310_64\bin;C:\Code\language\Qt\Tools\CMake_64\bin;C:\Code\language\Qt\Tools\Ninja;C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path

cd C:\Code\code\project\Studious
cmake -S frontend -B frontend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"
cmake --build frontend/build
```

产物：`frontend/build/studious.exe`。

> 项目根的 `build.ps1` 也会自动带上 `frontend`（它按 `frontend/CMakeLists.txt` 存在与否决定构建）。

**运行前把 Qt 的 bin 加进 PATH**（MinGW 运行时不随 exe 复制）：

```powershell
$env:Path = "C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path
```

---

## 2. GUI

```powershell
.\frontend\build\studious.exe
```

- 顶部：源语言 / 目标语言 / 引擎 / 显示原文对照 / 翻译按钮（Ctrl+Enter、Ctrl+Return）
- 中间：左原文、右译文（QSplitter 可拖）
- 底部：`引擎 / 耗时 ms / 段落数`，失败时红字显示后端 error；
  再下一行是各引擎就绪状态（来自 `router.status()`，如 `mock:就绪  web:就绪  llm:未就绪  local-mt:未就绪`，
  鼠标悬停看每个引擎的具体 reason / 缺失资产）

**段落规则**：输入按空行切段（连续空行算一个分隔，首尾空白去掉，空段丢弃），每段一个 `paragraphs` 元素。

**注意**：引擎选「自动」时，后端默认优先级是 `mock → web → llm → local-mt`，
`mock` 永远就绪，所以自动模式实际走 mock（输出 `【zh】原文`）。要看真翻译请显式选 `web`。

---

## 3. 冒烟模式

```powershell
# 真联网翻译（Google 不通时自动降级 MyMemory，可能耗时 15s+）
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine web

# 离线自测
.\frontend\build\studious.exe --smoke "Hello" --to ja --engine mock
```

- 参数：`--smoke <文本>` `--to <lang>`（默认 zh）`--engine <mock|web|llm|local-mt>`（默认自动）
- 不创建窗口，只 `QCoreApplication`；走**与 GUI 完全相同**的路径（工作线程 + factory + router）
- 成功 exit 0，失败 exit 1；控制台输出为 UTF-8 字节（老控制台先 `chcp 65001`）
- 内置 60s 超时保护

示例输出：

```
=== Studious 前端冒烟 ===
engine=mock  to=ja  src=auto  paragraphs=1
耗时: 3 ms
译文:
【ja】Hello
[OK] exit 0
```

---

## 4. 线程模型（本 demo 的核心约束）

`net::QtHttpClient` 包着 `QNetworkAccessManager`，**只能在所属线程使用**。因此：

```
主线程                                   工作线程（QtConcurrent 线程池）
──────────────────────────────           ──────────────────────────────────────
MainWindow ── start(req) ──> TranslateJob
                              QFutureWatcher<TranslateOutcome>
                                    │     QtConcurrent::run(translateInWorkerThread, req)
                                    └──>  QtHttpClient http;                        // 线程内构造
                                          makeDefaultTranslators(http);             // 线程内装配
                                          TranslatorRouter r{std::move(translators)};
                                          r.translate(req) ──> TranslateOutcome
                                    ┌──<  纯数据回传
                              finished(outcome, elapsedMs)
```

- `QtHttpClient` / translator 集合 / `TranslatorRouter` **全部在工作线程内构造、使用、销毁**，绝不共享。
- 跨线程只搬纯数据：`TranslateRequest`（入）/ `TranslateOutcome`（出）。
- `QFutureWatcher` 活在主线程，`finished` 是队列连接，UI 不会被卡。
- 每次刷新 `engineStatus` 都会在主线程临时造一个 `QtHttpClient`——`status()` 不联网，
  但对象必须与调用同线程，所以不能复用工作线程那个。

另外：`router::TranslatorList` 是 `std::vector<std::unique_ptr<ITranslator>>`，
**不能用 QVector**（Qt6 的 QList 要求元素可拷贝，`unique_ptr` 直接编译失败）。

---

## 5. 文件

```
frontend/
├─ CMakeLists.txt      # Qt6 Core/Widgets/Concurrent + add_subdirectory(../backend)
├─ README.md
└─ src/
    ├─ main.cpp        # 入口；--smoke 走 QCoreApplication + QEventLoop，否则 QApplication + MainWindow
    ├─ MainWindow.h/.cpp
    ├─ TranslateJob.h/.cpp  # splitParagraphs() + QtConcurrent 工作线程作业
```
