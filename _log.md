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



---

## 2026-11-05 · docs/TempUI：Material Design 3 Expressive 单文件 UI 演示

**产出**：`docs/TempUI/index.html`（208 KB / 3517 行，零外链）+ `docs/TempUI/preview.png`（1440×900 截图）。

### 走的路线
1. 先读 `docs/ui-structure.md`（773 行）当唯一信息源，再对照 `frontend/build/gui_*.png` 取真实文案
   （引擎下拉 `自动/web/mock/llm/local-mt`、状态条 `mock:就绪 web:就绪 llm:未就绪 local-mt:未就绪`、
   资产清单里 `runtime.llama.vulkan.win-x64` 250 MB 等 ID/大小直接复用）。
2. 设计令牌全部由 `--seed` 用 `color-mix(in oklab, ...)` 派生，亮暗两套只覆盖 `[data-theme="dark"]`；
   换强调色 = 改 `--seed` / `--tert-seed`，全站自动重算。
3. 图标自写 63 个 `<symbol>`（24px 线性 path，stroke 1.7），无字体、无 CDN。
4. JS 全部事件委托（45 种 `data-act`），无内联 `onclick`，未接线的按钮统一落 snackbar「演示版：仅点击反馈」。

### 验证方式（比截图更靠谱的一招）
用 Edge headless + CDP 真点一遍，而不是只看截图：

```powershell
msedge --headless=new --remote-debugging-port=9333 --user-data-dir=$env:TEMP\prof `
       "file:///C:/Code/code/project/Studious/docs/TempUI/index.html"
# Node 24 自带 WebSocket，直接连 ws://…/devtools/page/<id> 发 Runtime.evaluate
```

刷 246 个 `button[data-act]`，`Runtime.exceptionThrown` 零命中 → 说明切页/菜单/开关/优先级排序/
资产下载/思考探测/Wayland 置灰全部没抛异常。`Page.captureScreenshot` 逐页出图核对布局。

### 踩坑
- **`<div class="menu">` 忘了 `hidden`**：截图里两个下拉同时展开。CSS 里 `.menu[hidden]{display:none}`
  只能兜住「有 hidden」的情况，默认可见；改成一次性补 `hidden` 属性才对。
- **`.pop{display:inline-block}` 打过 `.only-translate{display:none}`**：同特异度、后写的赢，
  非翻译页也顶着引擎 chip。改用 `body:not([data-page="translate"]) .only-translate{display:none}` 提高特异度。
- **FAB 截图里消失**：`.page-scroll{flex:1}` 没配 `min-height:0`，flex 子项不收缩 → `.main` 被撑高，
  `position:absolute;bottom:24` 的 FAB 跑到可视区外被 `.app{overflow:hidden}` 裁了。补 `.main{min-height:0;overflow:hidden}`。
- **chip 选中后 check 图标占位 0 宽**：`aria-pressed=true` 那条只改了 opacity/transform，没恢复 width，
  勾选后挤不出位置。
- **`color-mix` 里用 `calc(var(--op) * 100%)` 做百分比**：不如直接塞 `88%` 字符串稳，少一层嵌套。
- **CDP 取 target 别取第一个 `type=page`**：Edge 自带 `edge://sync-confirmation-dialog/`，
  会拿到一个空 DOM 的页面，报 `Cannot read properties of null`。

### 复检后改动（Master 验收轮）
- **FAB 从右下角挪到 top app bar 尾部**：`position:absolute; right/bottom:24` 在 1440×900 下会压住
  译文面板的「命中 3/5」、分类页阈值、快捷键表格右侧、外观预览块。改成 `.appbar-actions` 里的
  `position:static` 48px 扩展 FAB，≤1180px 收成 48px 纯图标，内容区不再被遮。
- 复检方式：`msedge --headless=new --virtual-time-budget=3000 --screenshot` 逐页出图 + `--dump-dom`
  核对 `rail-item.is-active` / `railIndicator transform`（导航态正确，早先截图发灰是入场动画未结束）。
- 暗色校验技巧：`applyTheme()` 启动即写 `data-theme`，直接改 HTML 的 `data-theme` 无效；
  必须拷一份临时文件把 `let themeMode = 'light'` 改成 `'dark'` 再截图。

---

## 2026-11-05 · docs/TempUI/flows.html：按 ui-prompts.md 做三模块交互模拟稿

**产出**：`docs/TempUI/flows.html`（181 KB / 2,864 行，零外链）+ `preview-flows.png`。
模块 A 截图翻译 10 屏、模块 B 翻译小窗 9 屏、模块 C 实时翻译 8 屏，外加一页验收清单 = 27 个状态视图。

### 走的路线
1. **复用上一份的 token 与组件**：从 `index.html` 抽出两段 `<style>`（54 KB）和 63 个 `<symbol>` 图标，
   注入新文件的 `/*__BASE_CSS__*/` 与 `<!--SPRITE-->` 占位符；新增 CSS 只写舞台、选区、控制条、小窗。
   好处是配色/圆角/动效天然同源，坏处是带了些用不上的页面级规则（不影响）。
2. 每个模块一页，页顶一排状态 chip（`.state-chip`）切视图；rail 切模块；hash 深链到单屏（`#A4`、`#B7`），
   截图和评审都用它。
3. 状态说明不写在画面上，统一放到舞台下方 `#noteA/B/C`，由 `NOTES` 表在切屏时写入。

### 验证方式
```powershell
# 静态复检：动画全关，避免截到入场淡入
Copy-Item flows.html _static.html
(Get-Content _static.html -Raw).Replace('</head>','<style>*{animation:none!important;transition:none!important}</style></head>')
msedge --headless=new --virtual-time-budget=1500 --window-size=1440,900 --screenshot=...
```
- CDP 实测：状态切换 + 面包屑 + ⚙ 面板、目标语言复选框（勾第二个后字段变「2 个目标语言」）、
  `show-ruby` 开关、记录页签、清空确认框、C2 暂停/继续、主题切换 → `Runtime.exceptionThrown` 0。
- **C2 浮层内 `button` 数量实测为 0**（零按钮是硬约束，这条要拿数据说话）。
- 1440×900 与 1280×800 两档都出图核对工具栏/尺寸标签不打架。

### 踩坑
- **尺寸标签（`bottom:-26px`）会被下方工具栏压住**：选区和工具栏都用百分比定位时，视口变矮就重叠。
  工具栏改成 `top:calc(44% + 34px)`（选区 44% 结束），按像素偏移，和百分比脱钩。
- **`0×0` 的选区加 `box-shadow:0 0 0 9999px` 不会压暗**（零面积不产生阴影），空选区状态要另加一层 `.dim`。
- **浮层「已中断」直接给整个 zone `opacity:.72` 会让底下的桌面文字透上来**，看起来像花屏；
  只降文字透明度 + 换灰底，不改容器 opacity。
- **`.rb-copy` 想绝对定位在结果块右上角**，得写 `.res-block .rb-copy{position:absolute}`；
  只写 `right/top` 没生效，图标会跑到行首。
- **记录列表右列删除按钮被挤出**：grid 中列要 `minmax(0,1fr)`，否则长文本撑开把 `auto` 列推出容器。
- **用 `--screenshot` 抓不到动画结束后的样子**：CSS 动画按真实时间跑，virtual-time 不推它，
  切屏后立刻截图必然是半透明；要稳定出图就先把动画关掉（实测 `getComputedStyle(...).opacity` 1.6s 后是 1，代码没毛病）。

### 返工：译文从「整块刷白」改成「按行覆盖」（Master 验收意见）

**问题**：A4/A5/A7 的译文原来装在一个不透明的 `.ov-body` 里，等于把整个选区刷成一块底色，
和真实行为（只顶掉原文那几行）不符。

**改法**
- 原文抽成独立图层 `.game-lines`（`left:8%; top:var(--txt-top); width:56%`），
  译文用同样坐标的 `.ov-text` 逐行对齐；每行 `.ov-line.plate` 用 `width:fit-content` 只包住自己的文字。
- `.ov-text` 不再有内联 `top:21%`——内联样式会盖掉 `var(--txt-top)`，这条踩过一次。
- 译文/原文/长条态用 `:has()` 把底下的 `.game-lines` 隐藏，避免英文和中文叠在一起。
- 加载态（A3）保持整块覆盖：那是「正在翻译」，遮住内容是对的。
- 尺寸标签从选区正下方挪到正上方右侧，`top:calc(var(--txt-top) - 9px)` 随文字块一起走。

**踩坑**
- **`min-width:47%` 的按行底板在窄视口会露馅**：文字宽度是 px、底板宽度是 %，屏幕一窄英文尾巴就从底板右边冒出来。
  既然底板对齐的是「被隐藏的原文」，宽度直接 `fit-content` 就够，不要跟百分比较劲。
- **`--txt-top:max(21%,128px)`**：假窗口的标题栏是固定 px，文字块用纯 % 定位，短屏（1280×800）时会压到标题；
  取 `max()` 后选区、工具栏、标签全部跟着同一个变量走。
- **放在 `.sel` 里的 `.sel-size` 用 `left:calc(7% + 58%)` 是错的**：百分比按选区自身宽度算，不是屏幕。
  要贴到选区外侧就写 `left:calc(100% + 8px)`。

### 补动效：翻译中 / 报错底色（Master 反馈「太单调」）

- **翻译中（A3）**：`::before` 两层 radial（primary + tertiary）+ `blur(22px)` 做极光，`busyDrift` 6.5s 交替漂移；
  `::after` 一条 34% 高的扫描带 `busyScan` 2.4s 自上而下，两端淡入淡出避免循环跳帧；标签跟着呼吸（0.62↔1）。
- **报错（A6）**：`::before` 红色呼吸光 `errBreath` 3.6s；`::after` 斜向细纹 `errStripes` 9s 匀速漂移；图标小幅起伏。
- 两个伪元素都 `< *{position:relative;z-index:1}`，文字和按钮永远压在动效上；容器加 `overflow:hidden; isolation:isolate`。
- **循环位移要和图案周期对齐**：135° 条纹周期 15px（法向），位移沿 (1,1) 时法向位移是 `d√2`；
  d=112 → 158px ≈ 10.5 个周期，肉眼能看到跳；改成 106px ≈ 150px = 10 个周期才顺。
- 全局 `prefers-reduced-motion` 那条 `animation-duration:.001ms !important` 覆盖到了伪元素，
  用 CDP `Emulation.setEmulatedMedia` 实测：正常 3.6s → 减弱后 1e-06s。

---

## 2026-11-06 · index.html 按六组意见改版（截屏/小窗/实时/二级设置 之外的那份主演示）

**产出**：`docs/TempUI/index.html` 213,320 → 205,161 B；`preview.png` 换成新的欢迎页。`flows.html` 未动。

### 改了什么
1. **翻译页**：页头改「欢迎来到 Studious」+ 一句「文字如舟，渡你过语言的河。」；删除「当前链路」流程块（`.chain-wrap` 与 `#chainDetail` JS 一起清）；目标语言改单选（组内互斥），译文只留一个 tab；引擎改 `自动 | web | 云端 LLM | 本地 MT`，mock 全库清零，底部状态条同步。
2. **删分类页**：section/rail/PAGE_META/图标/CSS/JS 一并删除；场景（屏幕区域/整个窗口/全屏/剪贴板/批量文件）搬到快捷键页「场景快捷键」，默认「未设置」，点设置进「按下新快捷键…」，清除回空。
3. **快捷键页**：删除平台预览、Wayland 置灰、平台能力卡、`data-platform` 与 `.wl-only/.w-only`；只留 Windows。
4. **配置页**：优先级 `web → 云端 LLM → 本地 MT → 本地 LLM`；状态面板去 mock、加本地 LLM「去配置」；四块配置改二级手风琴（默认收起、标题带摘要），两列布局左 Web/本地 MT、右云端 LLM/本地 LLM 各自独立；云端只留 URL/Key/模型/获取模型，其余进三级「高级设置」；本地 LLM 只留选模型/GPU 加速/启停/状态/日志，其余进高级设置。
5. **设置·资产**：`全部资产 | local-mt | 本地 LLM` 过滤（7/2/1 行）+「导入文件」；行加类别标签。
6. **外观**：删除平台能力矩阵；about 的第三方组件表仍用 `.matrix`，保留。

### 编码事故与防线（这次写进流程）
上一轮子代理用 PowerShell 5.1 `Get-Content -Raw` 读写 UTF-8，把 flows.html 写成乱码。
本轮强制：先 `Copy-Item` 备份，改文件只用 `edit` 或 `.NET File.ReadAllText/WriteAllText(..., UTF8NoBOM)`；
每轮自检 `FFFD=0`、乱码正则命中 0、`node --check`=0。收尾时 `mock|category|Wayland|X11` 全库命中 0
（仅剩 CSS 注释 `05 · PAGE-SPECIFIC — CATEGORY`，无害）。

### 顺手修正
- 云端 LLM 的 Base URL 预填值原来是 `http://127.0.0.1:3065/v1`（本地 llama-server 的地址），改成 `https://api.deepseek.com/v1`；
  本地 LLM 的日志与端口仍保留 3065。

### 验证
- 结构：5 个 page / 5 个 rail / 0 外链 / 0 内联事件；`node --check`=0。
- CDP 真点：目标语言点「英语」后只剩 1 个选中；场景快捷键「设置→按下新快捷键…→清除→未设置」；
  配置默认 4 个 `aria-expanded=false`、0 个展开；云端展开后有 `#llmBase`/获取模型，高级设置展开后 `#temp` 可见；
  本地 LLM 有 3 个模型项、GPU 开关、启动按钮；资产过滤 7/2/1；导入按钮有 snackbar；外观无 `.matrix`。
- `Runtime.exceptionThrown` 0；1280×800 无横向溢出。

---

## 2026-11-06 · index.html 第二轮：去浮层、窗口按钮、Google 运行页签

### 改动
1. **删浮层内容**：快捷键页整组「浮层」热键删除；外观页「浮层」section（样式/布局/行为 + 实时预览）整段删除，
   `[data-ov]` 滑杆逻辑与 `ovSet` 的 ov-bg/ov-fg/ov-shadow 分支一并清；外观页副标题改成「主题 / 字体 / 主窗口 / 语言显示名」。
2. **顶栏**：删除重复的引擎 chip（页面里已有分段控件）；删掉「快速翻译」FAB 与设置图标；
   右侧改为 主题 / 最小化 / 关闭，没有最大化。新增 `#i-min` 图标。
3. **运行按钮**：去掉工具栏右侧的「翻译 / 取消」，改成**引擎下方的扩展 FAB**（默认文案 `Studious`）。
   点击 → `翻译中…` + 6 秒倒计时（便于测）；翻译中 `mouseenter` 变红字「取消」，点击取消；跑完恢复 `Studious` 并给 snackbar。
   第一版误做成左下角页签，后按反馈改为 FAB；`.tr-controls` 的 `align-items` 从 end 改 start，避免 FAB 把三个字段的标签顶得不齐。
4. **引擎**：翻译页分段加「本地 LLM」→ `自动 | web | 云端 LLM | 本地 MT | 本地 LLM`；状态条补「本地 LLM 未就绪 / llama-server 未启动」。
5. **版式**：欢迎标题 2.5rem；翻译页改为 `height:calc(100vh - 148px)` 的 flex 列，`.panes{flex:1;min-height:0}`，
   底部 Google 条与状态条贴住视口，不再留白；≤1400 / ≤820 两档收紧字号与间距。

### 踩坑
- **flex 子项 `min-height:auto` 会把页面顶高**：`.panes{flex:1}` 但 `textarea.src{height:100%;min-height:280}` 让最小内容高度撑到 441px，
  页面整体溢出、页脚看不见。改成页面固定 `height:calc(100vh-148px)` + `.panes/.pane-body/textarea{min-height:0}` 才真正填满。
- **媒体查询的层叠看的是位置**：先写的 `@media(max-height:820)` 被后面页面级 `textarea.src{min-height:280}` 覆盖，
  必须提高选择器特异性（`.page[data-page="translate"] textarea.src`）。
- 删外观预览后 `ovSet` 只剩调用没有定义，先把调用方 case 删干净，避免留下 ReferenceError 隐患。

### 验证
- `node --check`=0；FFFD=0；外链 0；页数 5 / rail 5；`快速翻译`=0、顶栏设置按钮=0。
- CDP：Google → 翻译中 5.4s → hover 变「取消」→ 点击取消 → 再跑 6 秒完成；
  快捷键页分组只剩 截图翻译/剪贴板/窗口进程/场景快捷键；外观 section 主题/字体/主窗口/语言显示名；`Runtime.exceptionThrown`=0。
- 1440×900 与 1280×800 均无滚动、页脚与状态条在视口内。

### 追加：把中间空档合掉（FAB 行与工具栏同排）
- 原来 `.tr-controls` 是三列单行，FAB 撑高第三列后，左两列底下空出一大块。改成两行网格：
  第一行 源语言 / 目标语言 / 引擎，第二行左边 `.tr-toolbar`（原文对照 + 缓存键）、右边 `.tr-action`（FAB + 提示）。
  这样 FAB 正对引擎下方，工具栏补上左边的空档，中间不再断层。
- 欢迎语放大到 `clamp(2.5rem, 3.2vw, 3.25rem)`，副标题 1rem，占掉顶部余量；`.tr-controls` 的 `align-items` 保持 start。
- 网格第二行用 `align-self:start` + `margin-top:4px`，让 40px 的工具栏和 48px 的 FAB 视觉居中对齐。

### 追加：默认优先级改为 web → 云端 LLM → 本地 LLM → 本地 MT
同步了 5 处，避免只改一行留下自相矛盾的文案：
1. 配置页标题旁「默认优先级：…」；
2. `#prioList` 里 4 行 `.prio-row` 的 DOM 顺序与 `prio-rank`（本地 LLM 3、本地 MT 4）；
3. 引擎状态面板 `.eng-row` 顺序；
4. 翻译页引擎分段（`自动 | web | 云端 LLM | 本地 LLM | 本地 MT`）；
5. 翻译页底部状态条顺序。

验证：CDP 点一次「下移」→ 行序与序号同步变成 `llm:1 / web:2 / local-llm:3 / local-mt:4`，`Runtime.exceptionThrown` 0；
全库搜 `本地 MT → 本地 LLM` 命中 0。`flows.html` 未动（那边的引擎集是 web / llm / local-mt，没有本地 LLM 这一项）。

### 追加：场景快捷键改成一行五张卡
- 把原来「表头 + 5 行表格」的 `.sc-hk` 改成 `.scene-cards` 五列网格，每张卡：图标 + 名称、说明、键位框、设置/清除。
- **卡片仍保留 `.sc-hk-row` 类和 `data-role="key"`**：`scene-set / scene-clear` 用的是 `closest('.sc-hk-row')`，
  换外观不换结构，JS 一行没改。
- 键位框做成 34px 的虚线框，空态「未设置」、等待态实线 + primary 底色，状态一眼能分。
- 响应式：≤1180 三列，≤900 两列。

### 追加：场景快捷键提到第一行，去掉「批量文件」
- 把整个 `.hk-group` 移到快捷键页第一组（在 RegisterHotKey 说明卡之后），顺序变为：
  场景快捷键 → 截图翻译 → 剪贴板 → 窗口/进程 → 快捷键管理。
- 删掉 `data-scene="batch"` 卡片，剩 4 张（屏幕区域 / 整个窗口 / 全屏 / 剪贴板）；网格从 5 列改 4 列，
  ≤1180 两列、≤900 两列。
- 验证：`data-scene` 计数 4、`batch` 卡片 0；设置→等待态→清除仍正常；`Runtime.exceptionThrown` 0。

### 追加：修「折叠面板只能开不能收」+ 引擎区加「配置」跳转
- **根因**：`closePops()` 里写的是 `$$('[aria-expanded="true"]')`，把**所有**带 `aria-expanded` 的元素都置 false，
  包括 `.acc-head` / `.adv-head`。于是每次点折叠头：先被 closePops 清成 false，再被 `case 'acc'` 读成「当前是关的」→ 又打开一次，
  表现就是「能弹出、收不回」。
  改成只复位 `[data-act="pop-toggle"]` 的触发器，折叠面板的 aria 归 `case 'acc'` 管。
- 引擎区 `自动选择 / 强制引擎` 后面加「配置」按钮：自动模式跳到 **Web 来源**，强制模式按当前选中的引擎跳
  （云端 LLM / 本地 LLM / 本地 MT），已经展开就不重复点，只 `scrollIntoView({block:'center'})`。
- 顺手：菜单贴近视口底部时加 `.menu.up` 上翻；强制引擎列表顺序同步成 `Web → 云端 LLM → 本地 LLM → 本地 MT`。
- 验证：折叠头连点两次 `aria-expanded` true→false、面板 hidden true；高级设置同样；自动/强制两种模式的「配置」分别展开
  `#accWebBody` / `#accLocalBody`；`Runtime.exceptionThrown` 0。

### 追加：强制引擎加单选圆点与选中态
- 每行前面插 `<span class="force-radio" aria-hidden="true"></span>`，纯 CSS 画圆点：外圈 2px `--outline`，
  选中时外圈变 `--primary`、内点 `scale(0→1)`（弹性曲线），整行加 primary 9% 底色 + 32% 描边，标题转 primary。
- `.list-row` 原来是 3 列 grid，强制引擎这组单独改成 `auto auto minmax(0,1fr) auto`，圆点不挤标题。
- 选中逻辑复用已有 `case 'radio'`（对兄弟节点写 aria-pressed / aria-checked），JS 没改。
- 验证：初始 Web 选中；点「本地 LLM」后只有它 `aria-checked="true"`，其余 false；`Runtime.exceptionThrown` 0。

### 追加：左上角品牌标换成图标
- 原来是个字母 `S`，先换成 `#i-translate`——但和导航栏「翻译」用的是同一个字形，视觉重复，遂改成 `#i-spark`（四角星 + 小星），白色描边。
- `.rail-brand` 的渐变底、圆角、hover 旋转都保留；图标 26px、`stroke-width:1.9`。
- 备选：`#i-globe`（语言）、`#i-bolt`、`#i-layers`；要更"产品化"可以手写一个 A/文 组合标。

### 追加：手写 A/文 组合标（i-brand）
- 新建 `<symbol id="i-brand" viewBox="0 0 40 24">`，左 A 右 文，纯描边（stroke 1.7，白色）：
  - A：`M3.4 21.4 9.8 3.6l6.4 17.8` + 横杠 `M6 14.6h7.6`
  - 文：点 `circle cx=27.9 cy=4.6 r=1.8`（fill）、横 `M21.2 9.4h13.4`、乂 `M22.4 12.8 33.4 21.2` / 反向一条
- `.brand-ic` 从 26×26 改成 34×20（viewBox 是 40×24，横向锁排），仍在 44px 渐变底里居中。
- 试过三版：`#i-translate`（和导航重复）→ `#i-spark`（偏 AI，太泛）→ 手写 A/文（最终）。

## QML 前端 · 本地引擎接口打通（A/B/C 三组）

### A 本地 LLM 四个启动参数透传
- 链路：`AppSettings`（键 `settings/llm/localContextSize|localThreads|localGpuLayers|localExtraArgs`）→ `AppBackend` 四个 Q_PROPERTY → `LocalEngineConfig` → `LlamaServerOptions`（contextSize/threads/gpuLayers/extraArgs 早就在后端结构体里，**不用改后端**）。
- 语义：`contextSize 0 = 2048`、`threads 0 = 按后端自动`（preferredThreads）、`gpuLayers -1 = 自动`（Vulkan 99 / CPU 0）、`gpuLayers 0 = 显式不卸载`（`LlamaServer` 里 `if (m_options.gpuLayers != 0)` 决定要不要传 `-ngl`）。
- 热切换：`ensureStarted()` 里必须比较**折算后的生效值**（`EffectiveLaunchConfig` + `sameLaunchConfig`），否则用户把 0 改成 2048、-1 改成 99 会白重启一次 llama-server。
- 实测命令行（`%APPDATA%\studious\assets\logs\llama-server.log` 里 `[studious] … start:` 一行）：
  `llama-server.exe -m …\model.qwen2.5-0.5b-instruct.q4km --host 127.0.0.1 --port 18080 -c 2048 -t 8 -ngl 99 --no-webui`
- 注册表塞 4096/6/12/`--no-warmup --flash-attn` 再截图，配置页四个控件读到的就是这些值 —— 证明 settings→backend→QML 全程未丢。

### B 本地 MT 卡片接真数据
- 后端 `LocalMtTranslator` 原本就有 `installedPairs()/routeId()/routeDescription()/pivotsThroughEnglish()`，界面却写死 `localMtRoutes: []`，所以永远显示「需下载」。
- 新增 `requiredFiles()`（6 个：encoder/decoder quantized onnx + source/target.spm + vocab.json + config.json；merged onnx 是可选 KV cache 不算）与 `missingFiles(pair)`。
- **踩坑**：原来 `pairAvailable()` 用另一张 5 条的表 + `QFileInfo::exists()`，会出现「界面说缺 config.json、路由却认为这对能用」，最后在 `MtModel::load` 才炸。已合并成同一张 `requiredFiles()` + `isFile()`。
- **踩坑**：`routeDescription(src,dst)` 在 src=auto 时返回的是技术性错误串（"多语言模型不支持源语言 auto (mt.opus.en-mul 的 vocab.json 里没有它的语言 token)"），不能直接怼给用户。`mtRouteFor()` 先看 `routeId().isEmpty()`，空就返回空串，界面自己说人话。
- `missingFiles()` 直接拼 `"mt.opus." + pair`，不走 `pairDir(src,dst)`——`mul` 伪语言标签和 `zh-TW` 这种带横线的目标段，按 `-` 拆再拼回来是在赌运气。

### C 三个 bug
1. `LlmTranslator::cacheTag()` 里 baseUrl 带随机端口 → 本地 LLM 缓存永不命中。加 `cacheTagBaseUrl()`：回环地址（127.0.0.1/localhost/::1）只留 `scheme://host`，其它地址原样（不牺牲云端命中率）。实测同一句跑两次：`命中=0/1` → `命中=1/1`。
2. 本地 LLM 不能沿用云端的 temperature/maxTokens/systemPrompt/retryBudget —— 决定**不单开采样参数 UI**，沿用云端那份，本地只覆盖 `batchSize=1`、清空 `reasoningEffort`、`stripModelNotes=true`，并在卡片里写一行说明。
3. `req.webProviders` 从来没被赋值 → Web 来源选择不影响翻译。`translate()` 里按 `settings.webProviderId` 填；`refreshEngineStatuses()` 也用同一个列表建 router，避免「状态说 web 就绪、真翻译走另一个来源」。

### 其它
- `LlamaServer::logFilePath()/logTail()` 从 private 移到 public（界面要显示日志），`LocalEngine` 转发。
- `ReportBadge`/`Badge` 加 `tone: "danger"`。
- **杀进程会漏 llama-server**：`Stop-Process studious -Force` 只杀父进程，`llama-server.exe` 会留在 18080 端口上、还锁着日志文件。开发时要么正常关窗（走 shutdown），要么手动 `Get-Process llama-server | Stop-Process`。读日志被占用时用 `File::Open(..., 'Read', 'ReadWrite')` 共享打开。
- 翻译文件：`update_translations` 后 PowerShell 单引号里的 `` `r`n `` 是字面量，必须用 `[regex]::Replace` + `(\s*)`；ts 里 `<源>` 是 `&lt;源&gt;`，正则要跟着转义。最后 `studious_en.ts` unfinished = 0。

### 追加：本地 LLM「点启动就退 1」的真实原因与修法
- **症状**：配置页/翻译页选「本地 LLM」→ 启动失败，界面糊一整屏 llama.cpp 日志。
- **原因**：设置里「模型文件 = 自动（第一个可用）」存的是空串，而 `AppBackend` 以前无条件拼
  `LocalEngine::modelsDir() + "/" + settings.localModelFileName` → 空名字被拼成
  `C:\...\assets\models\`（一个**目录**）。`LocalEngine::resolveModelPath()` 里「空 = 取第一个可用模型」
  的分支只在真收到空串时才走，于是被绕过，llama-server 拿到 `-m <目录>`：
  `gguf_init_from_file: failed to open GGUF file … (No such file or directory)` → 退出码 1。
- **修法**：`localModelPathFrom(fileName)` —— 空 → 返回空串（交回 LocalEngine 解析），否则绝对路径原样 /
  相对名拼 modelsDir。`translate()` 与 `startLocalServer()` 两处都改。
- **再加一层**：`ensureStarted()` 里对模型路径的检查从 `exists()` 改成 `exists() && isFile()`，
  目录也会被当成「缺少模型」并给出人话提示，而不是让 llama-server 报退出码。
- **错误信息分层**：`LlamaServer` 的「提前退出」「启动超时」两条不再把 24 行日志尾部塞进 error 返回值，
  改成 `qWarning` 打进 stderr + 日志文件；返回给界面的只有「退出码 / 超时 + 日志在哪」。
  界面截图核对：`本地引擎启动失败：llama-server 提前退出（退出码 1）。详细日志见配置页「本地 LLM」的日志面板（…\logs\llama-server.log）。`
- **顺带修的**：
  - 引擎选择持久化只认 `web/local-mt/llm` 三个老值，现在也认 `llm-cloud`/`llm-local`。
  - `startLocalServer()` 失败时只 `emit errorRaised`，而 QML 里**没有任何消费者** → 点「启动」等于静默失败。
    现在同时写状态行（翻译页底部可见）。
  - `TranslatorRouter::status()` 的「状态矛盾」警告每次刷新都打一遍，改成同一 (引擎, 理由) 只打一次
    （`warnContradictionOnce`，QSet + QMutex）。local-mt 在 src=auto 下就是"ready 但 resolve 拒绝"，属正常态。
- **验证手法**：临时给 `--ui-shot` 加了 `STUDIOUS_UI_CALL=<Q_INVOKABLE>` 钩子，在真 GUI 通路上直接调
  `startLocalServer` / `translate`（**验完已删除**）。证据：日志里
  `start: …llama-server.exe -m …\model.qwen2.5-0.5b-instruct.q4km --host 127.0.0.1 --port 18080 -c 2048 -t 8 -ngl 99 --no-webui`
  → `model loaded` → `listening on http://127.0.0.1:18080`，退出后 `Get-Process llama-server` 为 0。



## 资产清单：删掉 runtime.onnxruntime.win-x64 + 拦截框接线

用户问「runtime.onnxruntime.win-x64 是什么？不是本地 MT 的驱动？我删了怎么还能跑？」——
那条是设计期占位，从来没被运行期读过：

- onnxruntime 是**编译期静态链接**：backend/CMakeLists.txt 的 ONNXRUNTIME_DIR /
  ONNXRUNTIME_LIB 进 target_link_libraries，backend/src/mt/MtModel.cpp 直接
  #include <onnxruntime_cxx_api.h>。运行期只要 exe 同目录有 onnxruntime.dll。
- backend/src、backend/include、frontend/src 里 SetDllDirectory|AddDllDirectory|
  onnxruntime.dll|runtime\.onnxruntime **零命中**，没有任何代码从资产目录加载。
- 条目本身也是假的：sha256 空、URL 编造、version 1.20.1 与仓库 vendored 的 1.31.1 不符。
- 处置：backend/asset/manifest.json 删掉整条（14 → 13）。**清单顶层是
  `_comment / schema / keyId / payload / sig`，条目在 payload.assets**，
  Python `d['assets']` 会 KeyError。
- 本地 MT 卡补一行说明：「ONNX Runtime 随应用内置，无需下载；上面的语言对目录齐全
  就能直接跑。」省得下一个人再去资产页找「驱动」。
- notReadyDialog 之前没人调（改成状态胶囊跳转后成了死代码）。现在点「当前请求解析不了」
  的胶囊会弹它，并新增「去资产页」按钮（missing-assets 时才出现，按引擎走 mt / llm 分类）。
- 坑：--ui-shot 的 ScrollView 只渲染视口内内容，看页面底部要么把 ui-size 撑高，
  要么临时把目标 CollapsibleCard 的 expanded 改 true（截完改回，共 6 处）。
- 验证：--assets-cli 打印 13 条、6 个 mt.opus 语言对全在；notready_dialog.png 确认三按钮布局
  （TEMP-SHOT 钩子已删并重建）。

## 本地 llama-server 生命周期：Job Object + 三个开关 + 缺资产禁用

需求：a) Job Object 兜底、切走就停（默认关，放本地 LLM 高级设置）、启动预热开关、
空闲 N 分钟自动停（默认 10 分、默认关）；b) 没装资产时 llm 相关功能不可用并提示去资产页。

- Job Object 只能放前端：backend/src/runtime/LlamaServer.cpp 明确要求 backend 自身源码里
  扫不到平台头（当时先把 attach/release 塞进 LlamaServer.h，已撤回）。
  frontend/src/LocalEngine.cpp 顶部 #ifdef Q_OS_WIN 包 <windows.h>；start() 里 m_server
  就位后 attachChildToJob()，shutdown() 里 releaseJob()。KILL_ON_JOB_CLOSE，句柄存 m_job；
  每步失败只 qWarning，不让它变成启动失败的理由。
  实测：GUI 起着 llama-server 时强杀 studious.exe，4 秒后 llama-server 归 0。
- 三个开关走 AppSettings（settings/llm/localStopOnSwitch|localPreheat|localIdleStop|
  localIdleMinutes，后者夹 1~240），bool 用 toBool() 读，注册表写成 "1"/"true" 都认。
- AppBackend：QTimer 单发。translate() 开始与 onTranslationFinished() 结束各 touch 一次
  （有请求在跑时不能被「空闲」回收）；startLocalServer() 成功也 touch；超时才真停。
  setEngineId() 里只有 stopOnSwitch 才 shutdown。停机走私有 shutdownLocalEngine(reason)
  —— 不能重载成已有的 Q_INVOKABLE stopLocalServer。
- 预热用 QTimer::singleShot(1500)：别跟首屏抢 CPU，也别让 llama 日志插到窗口还没画出来那段。
- 缺资产：startLocalServer() 先查 llamaRuntimeInstalled() + availableModels()，缺就只报
  一句人话、不拉进程；配置页「启动」按 localCanStart 禁用，旁边多一个「去资产页」。
- 坑：i18n 的两个 s() 不是一个东西。AppBackend.cpp 里 i18n::s() 是 ui 那份（返回结构体），
  后端双语必须写全 studious::i18n::s(zh,en) / sf(zh,en,{args})，参数是 const char*，
  传 QStringLiteral 编不过。
- 坑：Start-Process 起 GUI 会秒退 —— 没继承到 Qt bin 的 PATH，缺 Qt6Core.dll 直接退出，
  看起来像崩溃。Start-Process 之前先给 $env:PATH 加 Qt bin。
- 坑：pwsh 每次调用都是新进程，**上一次的变量不会留到下一次**。用 $raw 拼日志时 $raw 是空的，
  WriteAllText 直接把 _log.md 覆盖成新增那一段。救法：git checkout -- _log.md 回 HEAD，
  未提交的那段只能重写。以后拼文件一律在同一个调用里读完写。
- 坑：[System.IO.File]::WriteAllLines 在 Windows 上写 CRLF，反复按行改会让整个
  ConfigPage.qml 从 LF 变 CRLF。改完用 WriteAllText + Replace("\r\n","\n") 归一化。
- 验证：selftest ALL CORE TESTS PASSED；--smoke local-mt「你好,世界观!」；
  --smoke --local 起 vulkan 0.5b、停服务 3088 ms、无残留进程；预热开 → 10 秒内出进程；
  idle=1 分 → 约 60 秒后自动消失；强杀父进程 → 子进程跟着死。
  截图 docs/ui-shots-qml/life_switches*.png、cfg10_life.png。

## 设置页「未实现」清零 + 桌面集成 + 数据管理

目标：设置页 20 个 Stub 全部做掉。后端缺的接口先问，用户全选推荐项：
全局超时走环境变量、桌面四项真做（Windows 原生）、安装根目录支持迁移。

- 后端全局超时：QtHttpClient.cpp 加 effectiveTimeoutMs()，读 STUDIOUS_HTTP_TIMEOUT，
  只当**下限**用（各调用点 5~69 s 各有道理），回环地址不受影响（llama-server 探测仍 5 s）。
  顺手把 send() 里三处超时统一成同一变量。
- platform/Desktop.{h,cpp}：托盘用 Shell_NotifyIcon 手搓（不链 QtWidgets），
  开机自启写 HKCU\...\Run，单实例用 QLocalServer。
  踩坑1：Desktop.cpp 不能 include studious/i18n/L10n.h（studious_platform 不链 backend 库），
  改用 QCoreApplication::translate("Desktop", ...)，并把该文件单独列进 qt_add_lupdate。
  踩坑2：头文件里在 namespace 内写 `class QLocalServer *m_instanceServer` 会就地声明新类，
  编译报 incomplete type。必须在 namespace 之前写全局前置声明。
- 托盘探测的坑：trayAvailable() 只读 m_trayReady，而图标只在 setTrayEnabled(true) 时创建，
  于是两个开关默认关 → 图标永远没建过 → 界面永远显示"当前会话装载不了托盘图标"。
  改成 trayAvailable() 真建一次（NIS_HIDDEN 建出来就隐藏）再返回。
- AppToggle 内部点击会自己 `checked = !checked` 打断绑定，所有开关必须写
  `onToggled: { backend.x = checked; checked = backend.x }`。
- 单实例的坑（真 bug）：startSingleInstance() 无条件先 removeServer 再 listen，
  把正在运行的实例的命名管道注册删掉了 → 第二个实例也能 listen 成功 → 多开防不住。
  正确顺序：先 pingRunningInstance()（改成返回 bool），连得上就判定重复实例；
  连不上才 removeServer 清死 socket 再 listen。修完实测第二个实例 204 ms 退出、进程数 1。
- 翻译文件：lupdate 出来 25 条 unfinished，全填。一开始以为有"巨型合并 source"，
  其实是自己的宽正则跨 <message> 块匹配造成的假象；可靠做法是按 <message>…</message>
  切块再抓 <source>。另外按 context 分开修了「关闭」的误译（ConfigPage 是 Off，
  Main / SettingsPage 的按钮是 Close）。
- 截图设施：--ui-shot 加 --ui-section N，直接切设置页分区，不用再临时改 QML。
- 验证：英文界面七张截图全英文无残留；--smoke local-mt 命中缓存 13 ms；
  selftest ALL CORE TESTS PASSED；单实例实测通过。
- 仍留 Stub：AppearancePage 6 个（字体三处、主窗口卡级、布局密度、状态栏信息密度、语言显示名），
  HotkeysPage 1 个（平台层没有全局热键能力）。
