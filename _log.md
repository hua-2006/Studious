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

## 2026-11-04 前端「获取模型」+ 模型/思考等级下拉（只动 frontend/）

### 成功路线
1. 读 `backend/include/studious/llm/{LlmCatalog,LlmModelInfo}.h` 确认新 API：
   `llm::listModels(cfg, http)` 返回 `ModelListResult`；`reasoningOptionsFor(model)` 纯计算推导等级。
2. `SettingsDialog`：`m_model` 由 QLineEdit 换 `QComboBox`（`setEditable(true)` +
   `NoInsert`，网关不支持 /models 时手输不污染列表），新增 `m_fetchModelsButton` / `m_fetchStatus`（11px 小字、灰/绿/红，默认隐藏）。
3. 「获取模型」：`QtConcurrent::run(&runListModelsInWorkerThread, cfg)`，工作线程内现造
   `studious::net::QtHttpClient` 调 `llm::listModels`，只把 `ModelListResult` 交回
   `QFutureWatcher<studious::llm::ModelListResult>`（**未加 Q_DECLARE_METATYPE，直接能编**）。
   成功后重填下拉（`userData`=id、tooltip=`name / 推理=是|否 / 上限=a/b`），原值还在就保留、否则选第一项。
4. 「思考等级」不再写死 6 项：`rebuildReasoningOptions()` 按 `m_model->currentText()` 在
   `m_modelInfos` 里查能力，查不到就造 `supportsReasoning = true` 的默认对象（保证手输也能看到全部等级），
   重填前记住 `currentData()`、重填后按 `indexOfReasoningOption` 恢复；`options.size()==1` 时 `setEnabled(false)`
   但**保留 data**（空串），这样置灰状态下 `uiToConfig()` 仍能取到值喂给「测试连接」。
5. `--list-models`：`QCoreApplication` + 主线程直接 `QtHttpClient`（`send()` 内部自带 QEventLoop，
   命令行不需要 QtConcurrent），打印目录 + 该模型的思考等级，失败贴后端中文原因 exit 1。
6. 验证：clean 构建 12 步 0 warning / 0 error；`--list-models` 列出 21 个模型；
   `--smoke "Hello, world!" --to zh --engine llm` → 「你好，世界！」(1224ms)；
   `--smoke "Hello" --to ja --engine mock` → 「【ja】Hello」；UIA 实测点「获取模型」→
   绿字「已获取 21 个模型」、模型下拉 21 项、思考等级 6 项、测试连接仍「连接成功…2124 ms」。
   截图：`frontend/build/gui_fetch_models.png`、`gui_model_dropdown.png`、`gui_reasoning_dropdown.png`、`gui_test_connection.png`。

### 踩坑
- **UIA 的 `RootElement.FindAll(Children, pid=...)` 看不到 Qt 模态对话框**：主窗口在、对话框不在。
  用 Win32 `EnumWindows` 拿 HWND 再 `AutomationElement::FromHandle` 就正常。
- **ComboBox 弹出层是独立顶层窗口**：条目要从弹出的那个 HWND 里 `FindAll(ListItem)`；
  别按「标题为空」过滤（会把弹出层漏掉），只排除主窗口/对话框/ConsoleWindowClass。
  另外弹出层只暴露**可见**条目（滚动出去的算不到，10/21 属于正常）。
- **截图 `CopyFromScreen` 用 UIA 的 `BoundingRectangle` 会裁边**：这台机器 150% DPI，
  UIA 报 620x560（逻辑）而实际 930x840 物理。先把对话框 `SetWindowPos` 到 (4,4)，
  再固定截 960x900 区域，才拿到完整窗口。
- **PS 5.1 跑中文 .ps1 必须带 BOM**（源码仍必须无 BOM）：自动化脚本先写成 UTF-8 无 BOM，
  再用 `UTF8Encoding($true)` 重写一遍才不出乱码。
- **`QComboBox::currentText()` 对可编辑下拉返回编辑框文本**：`uiToConfig()` 直接用它即可，
  不用区分「列表选中」与「手输」。
- **`findData("")` 能命中 value 为空串的项**：`applyToUi()` 恢复「跟随模型默认」靠的就是这个，
  但首次填充（`count()==0`）要走「选 recommended」分支，否则会误选到最后一项。

## 2026-11-04 本地模型热切换 + GPU(Vulkan) 后端选择（backend/ + frontend/）

### 成功路线
1. `LlamaServerOptions` 加 `int gpuLayers = 0`：非 0 才往命令行追 `-ngl <n>`（0 = 一个参数都不多传，
   CPU 路径与之前完全一致）。实测 Vulkan 构建**必须** `-ngl 99`，只换 exe 不传 -ngl 时它照样用 CPU 算。
2. `LocalEngine` 重写成「后端解析 + 模型解析 + 幂等启动」：
   - `availableModels()` 扫 `<root>/models/` 的普通文件，按文件名（大小写不敏感）排序——不用 mtime：
     重下一次资产就会让下拉顺序乱跳，用户点错的代价是几秒重启。
   - `isGpuAvailable()/isCpuAvailable()` = 运行时目录里有没有 `llama-server.exe`（零成本，不查显卡）。
   - `ensureStarted(cfg)`：先算 (backend, modelFile)，与当前运行值全等且进程活着 -> 直接返回
     （`lastStartupMs()==0`）；否则**先 shutdown() 再走资产检查**，任何失败路径都再过一遍 shutdown()，
     所以不存在「旧服务还在 + 新服务起不来」的孤儿组合。
   - 线程只信快照：`ensureStarted()` 只在主线程，工作线程只拿 `baseUrl()` 字符串。
3. `AppSettings` 加 `localModelFileName`（settings/llm/localModel，空 = 第一个可用）与
   `localUseGpu`（settings/llm/localUseGpu，默认 true）。
4. 主窗口第二行加 `QComboBox localModelBox`（userData = 真实文件名，显示用 `shortModelName`）与
   `QCheckBox gpuBox`；两者只在「本地引擎」勾选时可用，GPU 选项在 `!isGpuAvailable()` 时置灰并说明缺哪个资产。
   **切换只写设置 + 状态栏提示「下次翻译将切换到 X / Y」**，重启留给下一次翻译前的 `ensureStarted`
   （否则拖一下拉就白等 1~2 秒重载权重）。
5. 状态栏：`引擎: llm（本地·Vulkan·qwen2.5-0.5b）    耗时: 185 ms    段落数: 3`。
6. CLI：`--local-model <文件名>` / `--local-gpu <on|off>`（默认 on）/ `--switch-demo`
   （同一进程内逐个换模型各翻一遍，每轮打 `后端 / 模型 / pid / 启动耗时`）。

### 实测（RTX 4060 Laptop，0.5B，同一句英译中，`--cache off`）
| 场景 | 后端 | 启动 | 单句 |
|---|---|---|---|
| `--local-gpu on` | vulkan | 834 ms | **102 ms**（着色器缓存预热后 47 ms） |
| `--local-gpu off` | cpu | 517 ms | 503 ms |
| `--switch-demo` 第 2 轮 1.5B | vulkan | 1131 ms | 151 ms（pid 变了 = 真的重启） |

### 踩坑
- **Vulkan 首次推理 11.7 秒**：日志 `prompt eval time = 11735 ms / 216 tokens`。
  不是算力问题，是 NVIDIA 驱动第一次编译 Vulkan pipeline（同一次运行的生成速度仍有 251 tok/s）。
  驱动着色器缓存热了之后同一份 prompt 变 `70.66 ms / 216 tokens`（3056 tok/s）。
  验收别用「第一次」的数字下结论，也别把 CPU/GPU 的绝对耗时写进代码。
- **`--switch-demo` 忘了 `--smoke` 会直接进 GUI**：进程挂在 `app.exec()`，零输出、也不起 llama-server，
  看起来像「本地引擎卡死」。CLI 演示参数必须挂在 `--smoke` 后面。
- **`frontend/build/studious.exe` 被上一个 GUI 实例占用时链接报 `Permission denied`**：
  不是代码问题，先关掉在跑的实例（`CloseMainWindow()` 走 closeEvent，`llama-server` 由析构回收）。
- **1.5B 在 Vulkan 下偶尔照抄原文**（同一句 CPU 能给译文）：直连 `POST /v1/chat/completions`
  同一后端同一模型能正确翻译（124 ms），所以是抽样敏感度不是后端损坏；要稳就调低 temperature 或换 0.5B。
- **`say()` 零输出 = 还没走到第一条**：冒烟排障先确认 `main()` 走的是 `runSmoke` 而不是 GUI 分支。

