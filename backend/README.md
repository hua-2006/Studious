# Studious Backend（翻译引擎，仅后端）

C++20 + Qt 6 **Core / Network** 实现的平台无关翻译后端。不含任何 UI、不含任何平台头文件。
前端（截图 / 选区 / 浮层 / 热键 / OCR）另行设计，见 `docs/backend-architecture.md`。

## 1. 目录结构

```
backend/
├─ CMakeLists.txt
├─ README.md
├─ include/studious/
│   ├─ contract/Types.h              # TranslateRequest / ResolveResult / TranslateOutcome
│   ├─ contract/ITranslator.h        # 翻译组件统一接口
│   ├─ contract/LanguageRegistry.h   # 内部语言码 ⇄ 各组件语言码
│   ├─ net/IHttpClient.h             # 网络抽象（唯一允许的出网口）
│   ├─ net/QtHttpClient.h            # QNetworkAccessManager 同步实现
│   ├─ translators/IWebAdapter.h     # 单个 Web 翻译源接口
│   ├─ translators/WebTranslator.h   # web 组件（多适配器 + 降级）
│   ├─ translators/MockTranslator.h  # 离线自测假引擎
│   ├─ translators/LocalMtTranslator.h  # 本地 MT（v1 桩）
│   ├─ translators/LlmTranslator.h   # LLM（v1 桩）
│   └─ router/TranslatorRouter.h     # 选路 / 降级 / 唯一入口
└─ src/
    ├─ contract/LanguageRegistry.cpp
    ├─ net/QtHttpClient.cpp
    ├─ translators/adapters/GoogleFreeAdapter.{h,cpp}
    ├─ translators/adapters/MyMemoryAdapter.{h,cpp}
    ├─ translators/WebTranslator.cpp
    ├─ translators/MockTranslator.cpp
    ├─ translators/LocalMtTranslator.cpp
    ├─ translators/LlmTranslator.cpp
    ├─ router/TranslatorRouter.cpp
    └─ selftest_main.cpp
```

## 2. 构建

```powershell
$env:Path = "C:\Code\language\Qt\Tools\mingw1310_64\bin;C:\Code\language\Qt\Tools\CMake_64\bin;C:\Code\language\Qt\Tools\Ninja;C:\Code\language\Qt\6.12.0\mingw_64\bin;" + $env:Path

cmake -S backend -B backend/build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/Code/language/Qt/6.12.0/mingw_64" `
  -DCMAKE_CXX_COMPILER="C:/Code/language/Qt/Tools/mingw1310_64/bin/g++.exe"

cmake --build backend/build
```

产物：`backend/build/libbackend.a`、`backend/build/backend_selftest.exe`。

运行：

```powershell
backend/build/backend_selftest.exe --offline   # 跳过联网用例
backend/build/backend_selftest.exe             # 含真实 Web 翻译（允许失败）
```

> 直接双击 exe 会因缺 Qt DLL 启动失败（`exit=-1073741515`），先把 Qt bin 加进 PATH 再运行。
> 自测输出统一写 UTF-8 字节：Windows Terminal / PowerShell 7 正常；老 cp936 控制台先 `chcp 65001`。

## 3. 设计要点

### 3.1 分层与依赖方向

```
TranslatorRouter  ──►  ITranslator  ◄── WebTranslator / MockTranslator / LocalMtTranslator / LlmTranslator
      │                                      │
      │                                      └──► IWebAdapter ◄── GoogleFreeAdapter / MyMemoryAdapter
      └──► LanguageRegistry                       │
                                                  └──► IHttpClient ◄── QtHttpClient
```

- **只有 `IHttpClient` 能动网络**：业务代码里不存在 `QNetworkAccessManager`。换传输（
  代理、重试、录制回放）只改一处。
- **只有 `LanguageRegistry` 能翻译语言码**：禁止在别处写 `"zh-CN"` 这类字面量。
- **平台无关**：`backend/` 全域不 include 任何平台头，`src/` 里也没有 `#ifdef _WIN32` 的
  业务分支。CI 可用下面的命令守护：

```powershell
Get-ChildItem -Recurse backend -Include *.h,*.cpp | Select-String -Pattern 'windows\.h'
```

### 3.2 语言码

内部统一简化 BCP-47：`auto, zh, zh-TW, en, ja, ko, fr, de, es, ru, pt, pt-BR, it`。
`LanguageRegistry::normalize()` 吃常见别名（`zh_CN` / `zh-Hans` / `jp` / `kr` / `pt-br` …），
未知输入返回小写原值，不抛异常。

### 3.3 引擎与降级

默认优先级：`mock → web → llm → local-mt`（`TranslatorRouter` 构造参数可改）。

| 引擎 | v1 状态 | 说明 |
|---|---|---|
| `mock` | 可用 | 离线自测，输出 `【dst】原文` |
| `web` | 可用 | Google 免费接口 + MyMemory，无需 key |
| `llm` | 未实现 | 预留 llama-server 子进程 + OpenAI 兼容协议 |
| `local-mt` | 未实现 | 预留 onnxruntime + OPUS-MT 分语言对模型 |

规则：
1. `preferredEngine` 非空 → **只用它**，不可用也返回明确错误（不偷偷换引擎）。
2. 否则按优先级取第一个 `isReady()` 且支持全部目标语言的组件。
3. 全不可用 → `error` 里拼出每个组件的具体原因与缺失资产 id。

### 3.4 网络与合规说明

- `WebTranslator` 是**免费公开接口**的薄封装，无任何硬编码 API key。
- Google 免费接口（`translate_a/single`）不是官方 API，无 SLA，高频会被限流；
  MyMemory 匿名额度有限（按字符/天），且**不支持 `auto`**——本实现遇到 `auto` 时固定用
  `en` 作代理源语言，并把这一限制写进错误信息。
- 生产环境应换成带 key 的官方 API 或自建的 `llm` / `local-mt` 引擎，接口不用改。

### 3.5 线程模型（后续接入注意）

`QNetworkAccessManager` 是线程亲和的：`QtHttpClient` 不能跨线程共享，工作线程里各自构造。
当前 `send()` 是**同步语义**（局部 `QEventLoop`），调用方必须放在工作线程，绝不能在 UI 线程调。

## 4. 已知限制 / 下一步

1. `WebTranslator` 逐段串行请求：长文本慢且容易撞限流，后续加并发（2~4 路）+ 退避重试。
2. 无缓存：段落重复率高（galgame 场景 30~60% 命中率）时浪费严重，`TranslationCache`（SQLite，按 key = sha256(engine+src+dst+text)）是下一个高价值模块。
3. `llm` / `local-mt` 仅接口占位，`supports()` 恒 false，保证 Router 不会误选。
4. 单请求单目标语言：`dstLangs` 已按复数设计，但每个目标语言是一次独立的段落循环。

## 5. 踩过的坑（别再踩）

### 5.1 `QVector` 装不下 `std::unique_ptr`（设计偏离的唯一一处）

`TranslatorRouter` 的构造参数规格是 `QVector<std::unique_ptr<ITranslator>>`，实测在 Qt 6.12
**不可用**：`QList`/`QVector` 的元素类型必须可拷贝——`push_back` 会走 `copyAppend`，
连 `reserve()`、`operator[]`（→`data()`→`detach()`）都会实例化拷贝路径，而 `unique_ptr`
删除了拷贝构造 → 直接编译失败：

```
qarraydataops.h:311: error: use of deleted function 'std::unique_ptr<...>::unique_ptr(const ...&)'
```

试过 `Q_DECLARE_TYPEINFO(..., Q_RELOCATABLE_TYPE)`：`isRelocatable` 确实变 true，但
`reallocateAndGrow` 里 `copyAppend` 与 `moveAppend` 两个分支都会被实例化，仍然报错。
**结论**：改用 `std::vector<std::unique_ptr<ITranslator>>`（`router::TranslatorList` 别名），
接口语义完全一致，只是容器从 Qt 换成 STL。其余容器（`QStringList` / `QVector<QString>`
/ `QHash`）都在 `Types.h` 契约里，未受影响。

### 5.2 QtHttpClient 的 `QNetworkAccessManager` 是线程亲和的

一个实例只能在其所属线程用。当前 `send()` 是同步语义（局部 `QEventLoop`），调用方必须在
工作线程；跨线程共享 NAM 会随机崩或返回空。

### 5.3 `setTransferTimeout` 不总够用

个别平台 abort 不及时，`QtHttpClient` 里同时挂了 `QTimer` + 手动 `abort()` + `loop.quit()`
兜底，避免同步调用挂死。

### 5.4 免费接口的错误页是 HTML

Google 免费接口限流/要求验证码时返回的是 HTML 页面，不是 JSON。解析失败时错误信息里带
响应前 200 字节，排查时直接看这段就能判断是限流还是 URL 拼错。
