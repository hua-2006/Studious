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

- 顶部第一行：源语言 / 目标语言 / 引擎 / 显示原文对照 / 翻译按钮（Ctrl+Enter、Ctrl+Return）
- 顶部第二行：**来源**（Web 翻译来源下拉）+ **设置…**（打开「设置 — LLM 翻译」对话框）
- 中间：左原文、右译文（QSplitter 可拖）
- 底部：`引擎 / 耗时 ms / 段落数`，失败时红字显示后端 error；
  再下一行是各引擎就绪状态（来自 `router.status()`，如 `mock:就绪  web:就绪  llm:未就绪  local-mt:未就绪`，
  鼠标悬停看每个引擎的具体 reason / 缺失资产）

**段落规则**：输入按空行切段（连续空行算一个分隔，首尾空白去掉，空段丢弃），每段一个 `paragraphs` 元素。

**注意**：引擎选「自动」时，后端默认优先级是 `mock → web → llm → local-mt`，
`mock` 永远就绪，所以自动模式实际走 mock（输出 `【zh】原文`）。要看真翻译请显式选 `web` 或 `llm`。

### 2.1 来源（Web 翻译来源）

| 列表项 | userData |
|---|---|
| `自动（Google → MyMemory）` | 空串（= 后端默认顺序） |
| 每个 `listWebProviders()` 条目：`{displayName}` | `{id}` |

- `requiresApiKey && !configured` 的来源（v1 里就是 Azure 占位）显示成 `{displayName}（未配置）`
  并**置灰不可选**：`setItemData(idx, 0, Qt::UserRole - 1)` 负责外观，
  `QStandardItemModel::item(idx)->setEnabled(false)` 负责真正禁止选中（只做前者是假灰度）。
- 每项 `Qt::ToolTipRole` = `note`（悬停看"要不要 key / 国内要不要代理 / 额度"）。
- 选中项在翻译时组装成**有序 id 列表**传给 `makeDefaultTranslators(http, llmCfg, providerIds)`；
  选「自动」= 空列表。

### 2.2 设置 — LLM 翻译（SettingsDialog）

| 控件 | 说明 |
|---|---|
| Base URL | 默认 `http://127.0.0.1:3065/v1` |
| API Key | 默认 `12345678`，密码态 + 右侧「显示」勾选框切换明文 |
| 模型 | 默认 `deepseek-v4.1-flash` |
| 超时 | 1000~180000 ms，默认 60000，后缀 ` ms` |
| Temperature | 0.0~1.5，步长 0.1，默认 0.2 |
| System Prompt | 可空，空 = 用后端内置 prompt |
| 测试连接 | 工作线程里用**当前填写的** `LlmConfig` 真实翻译一次 `Hello, world! -> zh` |
| 恢复默认 / 确定 / 取消 | 恢复默认只改控件，点「确定」才落盘 |

「测试连接」成功显示绿字 `连接成功：Hello, world! → 译文（耗时 ms）`，失败显示红字 **引擎原文**
（`[llm] ...`，401 / 模型名错 / 超时 都靠这行定位）；测试期间按钮禁用。

### 2.3 配置持久化（AppSettings）

`QSettings("Studious", "Studious")`——Windows 落在
`HKCU\Software\Studious\Studious\settings\*`，不用自己管文件。

- 启动时 `AppSettings::load()`，某项为空回落 `defaults()`（`systemPrompt` 例外：空是合法值）。
- 关窗时 `MainWindow::closeEvent()` 写回（语言 / 引擎 / 来源）。
- 设置对话框点「确定」立即保存，随后**重建两个下拉**（`configured` 与 `llm.isValid()` 可能已变化）。
- `llm` 配置无效（baseUrl 或 model 空）时，引擎下拉里的 `llm` 项显示 `llm（未配置）` 并置灰。

---

## 3. 冒烟模式

```powershell
# 真联网翻译（Google 不通时自动降级 MyMemory，可能耗时 15s+）
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine web

# 指定单一 Web 来源（不走默认降级链）
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine web --provider mymemory

# 真实 LLM（默认用 AppSettings::defaults() 的本机网关配置）
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine llm

# 临时换网关 / key / 模型
.\frontend\build\studious.exe --smoke "Hello, world!" --to zh --engine llm `
  --llm-base http://127.0.0.1:3065/v1 --llm-key 12345678 --llm-model deepseek-v4.1-flash

# 离线自测
.\frontend\build\studious.exe --smoke "Hello" --to ja --engine mock
```

- 参数：`--smoke <文本>` `--to <lang>`（默认 zh）`--engine <mock|web|llm|local-mt>`（默认自动）
- `--provider <google|azure|mymemory|auto>`（默认 auto）：auto = 空列表 = 后端默认 google→mymemory
- `--llm-base/--llm-key/--llm-model`：不给就用 `AppSettings::defaults().llm`
- 不创建窗口，只 `QCoreApplication`；走**与 GUI 完全相同**的路径（工作线程 + factory + router）
- 成功 exit 0，失败 exit 1；控制台输出为 UTF-8 字节（老控制台先 `chcp 65001`）
- 内置 120s 超时保护

示例输出：

```
=== Studious 前端冒烟 ===
engine=llm  to=zh  src=auto  provider=auto(google→mymemory)  paragraphs=1
llm=deepseek-v4.1-flash @ http://127.0.0.1:3065/v1  timeout=60000 ms  temperature=0.2
耗时: 1345 ms
译文:
你好，世界！
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
                                          makeDefaultTranslators(http, req.llm,
                                                                 req.webProviders);  // 线程内装配
                                          TranslatorRouter r{std::move(translators)};
                                          r.translate(req) ──> TranslateOutcome
                                    ┌──<  纯数据回传
                              finished(outcome, elapsedMs)
```

- `QtHttpClient` / translator 集合 / `TranslatorRouter` **全部在工作线程内构造、使用、销毁**，绝不共享。
- 跨线程只搬纯数据：`TranslateJobRequest`（入：段落 + 语言对 + 引擎 + 来源顺序 + LlmConfig）/ `TranslateOutcome`（出）。
- `QFutureWatcher` 活在主线程，`finished` 是队列连接，UI 不会被卡。
- 每次刷新 `engineStatus` 都会在主线程临时造一个 `QtHttpClient`——`status()` 不联网，
  但对象必须与调用同线程，所以不能复用工作线程那个。
- 设置对话框的「测试连接」同理：`QFutureWatcher<LlmTestResult>` 在主线程，
  真正的 `QtHttpClient` + `makeLlmTranslator()` 在 `QtConcurrent::run` 的线程里生灭。

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
    ├─ MainWindow.h/.cpp     # 主窗口：语言 / 引擎 / Web 来源 / 设置入口 / 输出
    ├─ TranslateJob.h/.cpp   # splitParagraphs() + TranslateJobRequest + QtConcurrent 作业
    ├─ AppSettings.h/.cpp    # QSettings 持久化（LlmConfig + 来源 + 引擎 + 语言）
    └─ SettingsDialog.h/.cpp # 「设置 — LLM 翻译」对话框 + 测试连接
```
