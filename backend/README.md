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

### 5.5 i18n：静态表会把语言烤死在第一次调用上

`WebProvider` 的来源表原本是 `static const QVector<...>` 常量，`note` 直接是中文。
改成 `i18n::s(...)` 之后**不能再用 static 缓存**：静态初始化只跑一次，之后切语言全是旧文案。
现在的写法是 `buildProviderTable()` 每次取词重建（3 条，成本可忽略）。

同理，`i18n::sf()` 的参数替换必须**单遍扫描**：用 `QString::arg()` 逐个替换时，
参数里的 `%20`（URL 百分号编码）会被当成占位符二次替换，串成乱七八糟的东西。

### 5.6 `IHttpClient` 必须能流式收，否则下不了几个 GB 的模型

`send()` 返回整份 `QByteArray`，用在 3 GB 的 .gguf 上就是直接 OOM。
所以 `IHttpClient` 加了 `sendStream(req, callbacks)`：`onStart(status, contentLength)` 先决定收不收，
`onData(chunk, received)` 逐块交数据。`QtHttpClient` 用 `QNetworkReply::readyRead` 真流式实现；
默认实现（供测试替身）退化成 `send()` + 64 KiB 切块。

两个必须注意的点：

1. **`onStart` 返回 false 之后，一个字节都不能写文件**。否则 404 的 HTML 错误页会被当成包内容
   拼进 `.part`，后面校验报"哈希不匹配"，排查方向全错。
2. **超时要用空闲语义**。`QNetworkRequest::setTransferTimeout()` 对大文件是灾难，
   流式路径里改成"连续 N 秒没有新数据才 abort"（每收到一块就重置计时器）。

### 5.7 断点续传的三个分支，少一个就出脏包

- `206`：追加写，`total = Content-Length + 已有长度`；
- `200`：服务端不支持 Range → **必须 `truncate` 到 0 再写**，否则新旧字节拼接，哈希必然不匹配；
- `416`：Range 无法满足 → `.part` 已经完整，直接跳到校验。

还有：请求要带 `Accept-Encoding: identity`。服务端一 gzip，`Content-Length` 与已下载字节数
就对不上，续传位置全错。

### 5.8 zip 解压：miniz 用回调，别用 `_init_file`

`mz_zip_reader_init_file()` 收的是 `const char*`，Windows 下按 8-bit 本地编码打开文件——
中文用户名直接打不开。现在用 `mz_zip_reader_init(pZip, size, 0)` + `m_pRead` 回调（内部走 `QFile`），
解压写盘用 `mz_zip_reader_extract_to_callback()`。顺带避免把整个 zip 读进内存。

zip-slip 三道闸，缺一不可：
1. 条目名含 `..` 段 → 拒绝；
2. 绝对路径 / 带盘符（`/x`、`C:`）→ 拒绝；
3. 符号链接条目（`m_external_attr` 高位 = `S_IFLNK`）→ 拒绝。

解压目标是 `<target>.tmp`，全部成功才 `rename` 成 `<target>`；失败一律把 `.tmp` 删干净，
绝不留下"半个安装"。

## 6. 双语文案（i18n）

```cpp
#include "i18n/L10n.h"

out.error = i18n::s("未配置 LLM API：请在设置中填写 API 地址 / Key / 模型",
                    "LLM API is not configured. Please set the API URL, key and model in Settings.");
out.error = i18n::sf("目标语言 %1 第 %2 段失败：%3",
                     "Target language %1, paragraph %2 failed: %3", {dst, i + 1, err});
```

- `i18n::current()` **只看系统语言**（`QLocale::system().language() == QLocale::Chinese` → 中文，其余英文），
  不读任何配置文件；`setLanguage()` 只是给测试/前端设置用的显式覆盖。
- 所有会显示给用户的东西（`TranslateOutcome::error`、`ResolveResult::reason`、`WebProviderInfo::note`、
  `AssetStatus::detail`、`DownloadResult::error`、`ManifestLoadResult::error`）都是**已本地化的最终文本**，
  调用方直接显示，不要自己再拼前缀。
- 注释、日志、selftest 的输出标题一律保持中文，不进 i18n。

## 7. 资产：下载 / 安装 / 删除

```
AssetRegistry ── 状态真源（磁盘扫描）
     ▲   │
     │   └─> targetPath(): <root>/models/<id> | <root>/runtime/<id>
Downloader ── 多源 failover → Range 续传 → sha256 校验 → 原子落盘 → markInstalled()
```

磁盘布局：

```
<installRoot>/
├─ downloads/<id>.part          # 断点续传的半成品（取消/失败都保留）
├─ models/<id>                  # model 类：单文件就是文件，zip 就解成目录
└─ runtime/<id>                 # runtime 类：通常 zip 解出的目录
```

关键约束：

1. `sha256` 为空 = 未发布正式哈希 → 直接报错，**不允许跳过校验安装**；
2. 哈希不符 → 删 `.part` 并报"实际 vs 期望"，绝不安装；
3. `uninstall()` 幂等（不存在也 true），并顺手清掉 `<id>.part`；
4. `refresh()` 的判定：路径不存在 → `NotInstalled`；存在但是空目录/0 字节文件/缺点名文件 → `Corrupted`；
   否则 `Installed`。

## 8. 自测模式

```powershell
backend_selftest.exe             # 核心 + 真实 Web（允许失败）
backend_selftest.exe --offline   # 跳过网络
backend_selftest.exe --manifest  # 清单解析 + ed25519 验签
backend_selftest.exe --i18n      # 错误文案中英双语（离线）
backend_selftest.exe --assets    # 下载/续传/failover/取消/校验/zip/zip-slip/卸载（本地 HTTP 服务器）
backend_selftest.exe --cache     # 翻译缓存（需要本机 LLM 网关）
```

`--assets` 完全不碰外网：`QTcpServer` 起在 `127.0.0.1` 随机端口，`/file` 返回 512 KiB 确定性数据
并支持 `Range`，`/bad` 返回与清单哈希不符的内容，其它路径 404；服务器侧会记录收到过几次 Range 请求，
用来证明续传真的走了 `206` 而不是整份重下。
