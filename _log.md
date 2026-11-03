# Studious 开发日志

## 2026-11-04 前端 demo（frontend/，纯文本翻译测试，无截图/OCR/浮层）

### 成功路线
1. 读后端头文件确认 API：`router::TranslatorList = std::vector<std::unique_ptr<ITranslator>>`、
   `factory::makeDefaultTranslators(net::IHttpClient&)`、`TranslatorRouter::translate/status`。
2. `frontend/CMakeLists.txt`：`find_package(Qt6 Core Widgets Concurrent)` +
   `add_subdirectory(../backend ${CMAKE_BINARY_DIR}/backend)` + 执行文件**不加 WIN32**（保住控制台看 smoke 输出）。
3. `TranslateJob`：`QtConcurrent::run(translateInWorkerThread, req)` +
   `QFutureWatcher<core::TranslateOutcome>`；工作线程内构造 `QtHttpClient` → factory → router，
   跨线程只搬 `TranslateRequest` / `TranslateOutcome` 纯数据。
4. `MainWindow`：顶部语言/引擎行、QSplitter 左右原文译文、底部 status + engineStatus；
   Ctrl+Return / Ctrl+Enter 双 QShortcut；status 每帧用主线程临时 `QtHttpClient` + `router.status()` 刷新。
5. `main.cpp --smoke`：`QCoreApplication` + `TranslateJob` + `QEventLoop` + 60s 超时保护，UTF-8 字节直写 stdout。
6. 验证：clean 构建 23 步 0 warning / 0 error；web 冒烟 exit 0 出「您好，世界！」；
   mock 冒烟 exit 0 出「【ja】Hello」；GUI 起 3s 存活且标题正确；backend/ 0 文件被改动。

### 踩坑
- **TranslatorList 不能用 QVector**：Qt6 QList 要求元素可拷贝，`std::unique_ptr` 直接编译失败 → 用 `std::vector`。
- **QtHttpClient 线程亲和**：QNetworkAccessManager 只能在本线程用。共享会踩
  "Cannot create children for a parent in a different thread" 及请求不回调。每次作业现造一套（微秒级）。
- **WIN32 子系统砍控制台**：`add_executable(... WIN32)` 后 `--smoke` 什么都看不到 → 不加 WIN32，
  GUI 照样弹窗（实测）。
- **QFutureWatcher 结果取回**：`finished` 信号里调 `result()` 即可；`TranslateOutcome` 可拷贝，无需 `Q_DECLARE_METATYPE`。
- **重复快捷键会 ambiguous**：别同时给按钮 `setShortcut(Ctrl+Return)` 又注册同序列 QShortcut，
  会变成 ambiguous shortcut 谁都不触发。改成只注册 `Ctrl+Return` + `Ctrl+Enter` 两个不同序列。
- **运行需 Qt DLL 在 PATH**（MinGW，无水印部署）：`C:\Code\language\Qt\6.12.0\mingw_64\bin`。
  否则直接 `exit -1073741515 (0xC0000135, DLL_NOT_FOUND)`。
- **后端 `add_subdirectory` 不影响 exe 输出目录**：backend/CMakeLists 里的
  `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 只作用于子目录 scope，产物仍在 `frontend/build/studious.exe`。
- **默认「自动」引擎实际走 mock**：后端默认优先级 `mock → web → llm → local-mt`，mock 永远 ready。
  要看真翻译必须显式选 `web`。（demo 行为，不改后端）
- **web 冒烟 ~16s**：Google 免费接口在该网络下走 15s 超时，然后自动降级 MyMemory 成功。
  stderr 有一句 `QIODevice::read (QNetworkReplyHttpImpl): device not open`——后端 abort 后 readAll 的噪音，无害。
- **控制台编码**：本机 `[Console]::OutputEncoding = utf-8 (65001)`，UTF-8 字节直写即可正常显示中文；
  老 cp936 控制台需先 `chcp 65001`。

## 2026-11-04 后端扩展：Web 来源可选 + LLM 真实接入（只动 backend/）

### 成功路线
1. `IWebAdapter` 加三个纯虚 `displayName/requiresApiKey/isConfigured`，Google/MyMemory/Azure 三家同步实现
   （新增纯虚必须一次性补齐全部实现，否则编译期就断）。
2. `translators/WebProvider.h` 做来源注册表：`listWebProviders()` 出静态元信息表，
   `makeWebAdapters(ids)` 按 id 装配适配器，空列表回退默认 `google -> mymemory`，未知 id 忽略 + 去重。
3. `WebTranslator::providers()` 把"装配结果"回读：id/名称/需key/已配置/备注，configured 以适配器实现为准
   （注册表的 configured 只当静态默认值）。
4. `WebTranslator::translate()` 先按 `isConfigured()` 跳过未配置来源（Azure v1），再照旧"失败就换下一家"，
   一家成功即采用；全部失败才把各家原因合并上报。
5. `config/LlmConfig.h` + `LlmTranslator(cfg, http)`：逐段 POST `{baseUrl}/chat/completions`，
   system 强约束 + user 用 `<<< >>>` 划边界，只取 `choices[0].message.content`，HTTP 错误码映射中文原因。
6. `Factory` 全部保留旧签名（前端只用 `makeDefaultTranslators(http)`，零改动），新增 providerIds / LlmConfig 重载。
7. 自测新增 `--providers`（表格 + 装配/降级实测）与 `--llm`（真翻两段 + 每请求耗时 + 失败时 dump 原始响应）；
   `--offline` / 无参模式输出与原先一致。
8. 验证：clean 构建 19 步 0 warning / 0 error；`--offline` = ALL CORE TESTS PASSED；
   `--llm` 出「你好，世界！」「这是一个测试。」（~1.2s/请求）；平台头文件扫描 0 命中。

### 踩坑
- **`reasoning_content` 会跟 `content` 一起返回**：deepseek-v4.1-flash 实测 reasoning 有 170~344 字符，
  只认 `content`，否则用户看到的是模型自言自语。
- **弱 prompt 真的会聊起来**：只写"翻译下面文本"会得到"翻译如下："前缀、或直接回答原文里的问题。
  加"你是翻译引擎不是聊天助手 + 四条硬规则 + `<<< >>>` 边界"后，疑问句/UI 菜单/剧情长句三组刁钻输入全部正常。
- **错误前缀会双写**：适配器 error 本身带 `google: `，WebTranslator 又拼一次 id → `google: google: ...`。
  拼接前先判断 `r.error.startsWith(id + ':')`。
- **非 2xx 时 Qt 也报"网络错误"**：网关返回 404（模型名错）时 `QNetworkReply::error()` 是 203，
  `resp.error` 只有 "server replied: Not Found"，真正原因在响应体 → 4xx 分支优先贴 body 前 200 字节。
- **PowerShell 5.1 的 `Set-Content -Encoding UTF8` 会写 BOM**：把源文件改成 UTF-8 BOM 后与项目其余
  无 BOM + LF 的文件不一致 → 一律用整篇重写的方式落盘（勿用 Set-Content 改源码）。
- **本机没有 `pwsh`**（只有 powershell 5.1）且中文内联命令会被控制台编码破坏：
  需要跑带中文的临时脚本时，先把脚本写成 UTF-8 文件，再 `Get-Content -Raw -Encoding UTF8 | Invoke-Expression`。
- **Google 免费接口在这条网络下会 429**：单靠它必挂，正好证明"多来源 + 降级"这条链是必要的（MyMemory 兜底可成）。
- **`--llm` 硬编码配置不好排查**：补了 `STUDIOUS_LLM_BASE_URL / _API_KEY / _MODEL` 环境变量覆盖（默认值不变），
  借此实测了 401（key 无效）、404（模型不存在）、连接拒绝三条错误映射。
