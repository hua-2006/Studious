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

## 测试发现的问题与六项修复（2026-11-07）

四个只读子代理（运行时矩阵 / AppBackend 状态机 / QML 走查 / 后端库审查）出了一份问题清单，
用户对六处方向性问题全选推荐项，本轮按此落地。

### 决策
1. mock 彻底退出生产装配：只有 `--engine mock` 与 `backend_selftest` 才注册。
2. 引擎 id 收敛成唯一换算：`toUiEngineId()` / `toRouterEngineId()` / `toRouterPriority()`。
3. 缓存键加语义白名单 + 模型指纹。
4. auto 选路与运行时失败都降级，本地优先。
5. 错误反馈走 Main.qml 顶层的全局通知条（订阅 `errorRaised`）。
6. 资产清单从"exe 旁边可写的 json"改成**编译进 qrc**。

### 改动要点
- `frontend/src/AppBackend.cpp`：默认优先级 `{llm-local, local-mt, llm-cloud, web}`；`translate()` /
  `refreshEngineStatuses()` 一律经 `toRouterPriority()`；`saveSettings()` 改存**界面 id**
  （原来存 Router 的 "llm"，llm-local 会被折成空串、重启回自动）；`setStatus(text, true)` 里统一
  `emit errorRaised(...)` 并带去重成员 `m_lastNotifiedError`；下载 worker 捕获 `QPointer` 防 UAF；
  `restartApplication()` 先 `m_desktop.shutdown()` 再 `startDetached`（否则新进程 ping 到旧实例自杀）。
- `frontend/src/AppSettings.cpp`：`localGpuLayers` 的 -1 往返修复 —— native 后端把 -1 写成
  `REG_DWORD 0xFFFFFFFF`，`.toInt()` 溢出成 0，而 0 = "一层都不卸载"，"自动"静默变成纯 CPU。
  改成 `toLongLong()` 读、按 `quint32` 还原再夹紧。
- `frontend/CMakeLists.txt`：`qt_add_resources(... "asset_manifest" PREFIX "/assets")`；
  `findManifestPath()` 先读 `:/assets/manifest.json`，只有 `STUDIOUS_DEV_MANIFEST=1` 才回退磁盘。
- `frontend/src/TranslateJob.cpp`：`includeMock = (engineId == "mock")`；兜底链 `{local-mt, llm, web}`。
- `frontend/src/main.cpp`：CLI 走 `AppSettings::load()`；`resolveEngineId()` 认界面 id；
  **auto 不写死 preferredEngine**（写死等于关掉降级），只把优先级链翻成 Router id 传给 `job.priority`；
  `--local` 时 `req.llm.model` 用真实 gguf 名（原来写死 "local"，0.5B / 1.5B 共用一条缓存）；
  `--ui-shot` 跳过重复实例判定；新增 `STUDIOUS_UI_CALL` / `STUDIOUS_UI_ARG`（仅截图分支，
  用来拍"点下去之后"的界面，如通知条）。
- 后端：`TranslatorRouter::translate()` 改成按 priority 逐个尝试（可用性失败换下一个、语义失败立刻停，
  判据是 error 文案关键字）；`Factory` 加 `includeMock=false`；`QtHttpClient` 换
  `LoopbackAwareProxyFactory`（回环一律 NoProxy）；`LlmTranslator::cacheTag()` 4 段
  （id | 模型指纹 | 语义摘要 | baseUrl）；`WebTranslator` 自带 cacheTag（来源集合）；批量路径
  与单段共用 `buildChatRequest()`、输入改 JSON 数组（段内换行免疫）、整次调用一个 deadline。
- QML：`NoticeBar.qml`（新）+ Main.qml 顶层订阅；5 处 `AppToggle` 补回灌
  `onToggled: { backend.x = checked; checked = backend.x }`；最小窗口 1120×720；
  翻译页正文面板最小高度 120、状态条换横向 ScrollView；语言/引擎名不再露裸 id。

### 实测
- `--smoke`（不带 --engine）不再落到 mock：`engine=web (auto: web)`，耗时 5795 ms，exit 0。
- `--engine mock` → `[WARN] …假翻译` + `[DONE] exit 2`（以前 exit 0，看起来像成功）。
- 坏代理 `STUDIOUS_HTTP_PROXY=http://127.0.0.1:1`：`--local` 照常出译文（回环不走代理），
  `--engine web` 明确失败，不带 --engine 时 web 失败后**自动降级到 local-mt** 出译文。
- 缓存：0.5B 与 1.5B 同一句都 `命中=0/1`（模型指纹生效），同模型第二次 `命中=1/1`。
- 单实例：第二个实例 211 ms 内退出，第一个仍活着。
- `backend_selftest.exe` → `ALL CORE TESTS PASSED`。
- 截图：`notice_bar.png`（全局通知条）、`min_translate.png` / `min_assets.png`（1120×720 不再塌陷）。

### 坑
- 只把 mock 从 Router 默认链删掉还不够：`TranslateJob` 装配时不注册 mock，`preferredEngine="mock"`
  会报"引擎不存在"，所以 `includeMock` 必须跟着 engineId 走。
- CLI 的 auto 一旦把 `req.engineId` 写成选中的那个，Router 就当成"强制引擎"、失败不降级 ——
  auto 必须留空 + 只给 priority。
- `QNetworkProxyFactory::setProxyFactory` 不接管所有权，工厂要以 NAM 为 parent。
## 配置页：四张引擎配置卡合并成 tab 版（2026-11-07）

需求：引擎配置区从"Web 来源 / 云端 LLM / 本地 LLM / 本地 MT"四张竖排卡改成一张卡 + 分段控件切换，分段项要带状态点，且与上方引擎列表联动。

改动（只两个文件）：

- `frontend/qml/components/Segmented.qml`：新增 `property var badges: ({})`（code → 颜色）。delegate 标签前画 6px 圆点，只有在 `badges[code]` 有值时才画，宽度公式把圆点算进 `Layout.preferredWidth`。其它页面不传 `badges` 时视觉零变化。
- `frontend/qml/pages/ConfigPage.qml`（2236 → 2333 行）：
  - webCard/cloudCard/localCard/mtCard 四张卡合并成 `id: detailCard` 的「引擎配置」卡，卡内 `Segmented` 四项（Web / 云端 LLM / 本地 LLM / 本地 MT）+ 四个 `ColumnLayout` 面板，各自 `visible: page.configTab === "<code>"`；
  - 新增 `property string configTab`、`configTabFor(key)`（`llm` → `llm-cloud`，其余原样，auto/空 → `""`）、`configTabBadges`（`engineState()` 的 ready/missing-assets/其它 → `Theme.good/danger/caution`）；
  - `cardForEngine()` 五 key 一律返回 `detailCard`；`openEngineConfig()` 先切 tab 再 `expanded = true` / 滚动 / `focusTimer`（下拉仍按 key 开 `cloudModelCombo` 或 `localModelCombo`）；
  - `Component.onCompleted` 与 `onEngineIdChanged` 按 `backend.engineId` 定初始 tab（自动模式留默认 `llm-cloud`）；
  - 原卡的 `hint` 挪成各面板顶部一行 Text；本地 LLM / 本地 MT 的「下载 / 导入」从 `headerActions` 挪到面板顶部右对齐。

验证：构建 `[11/11] Linking CXX executable studious.exe`；`--ui-shot --ui-page 2` 三个 tab 各截一张（`tab_config.png` / `tab_local.png` / `tab_mt.png` / `tab_light.png`）；把注册表 `engine\id` 改成 `local-mt` 重启后 tab 自动落在「本地 MT」（联动生效），验完复位成空；`studious_en.ts` 补 2 条（引擎配置 / 卡 hint）后 unfinished = 0；`--smoke ... --engine local-mt` → 你好,世界观! 1378 ms；`backend_selftest` → ALL CORE TESTS PASSED。

坑：
- 子代理搬最后一段（mt 面板）时少了一个收尾 `}`，会让 Popup 落进 ScrollView、根 Item 永不闭合。括号配平能过是因为少的是成对结构之外的一层——查法是数 `CollapsibleCard {` 次数 + 看 `Item{}` 与 ScrollView 的闭合顺序。
- 验证 tab 联动不要用 `STUDIOUS_UI_CALL`（它只能调 backend 的 Q_INVOKABLE），直接改注册表 `engine\id` 重启最快；或临时把 `configTab` 默认值改成目标 tab 截图，截完必须改回。

## 引擎描述带模型 + 去掉两张卡的折叠（2026-11-07 02:20）

用户要求：本地 LLM / 云端 LLM 的副标题要写"用的哪个模型 + CPU/GPU"，翻译页状态条同步；点状态胶囊跳转后分段控件高亮与控件值不刷新；把「引擎」「引擎配置」两张卡的折叠去掉。

改动：
- rontend/src/AppBackend.cpp：匿名命名空间加 shortModelName()（去目录、去 model. 前缀、去 .q4km/.q8_0/.gguf/.bin 后缀）与 localModelLabel()（设置里没选就用第一个可用模型）；	ranslate() 的 m_lastEngineLabel 改成 if 链，LLM 类引擎带模型名；efreshEngineStatuses() 里云端 LLM 就绪时 detail = 「模型 · 云端」，本地 LLM 跑着时 = 「模型 · GPU/CPU · 端口 N」，没跑时 = 「模型 · GPU/CPU · 翻译时自动启动服务」（模式按 settings.localUseGpu && localGpuAvailable() 推）。
- rontend/qml/pages/ConfigPage.qml：内联 CollapsibleCard 加 property bool collapsible: true（false 时不画箭头、头部 MouseArea disabled、内容恒显），engineCard 与 detailCard 都设成 false；页面加 onConfigTabChanged —— 程序化切 tab 时手动回写 configTabBar.current 并调 syncLlmControls()/syncLocalModelCombo()（Segmented 点击时内部自己写 current，会把父级绑定打断，所以之前只有面板换、高亮和控件值不动）。
- rontend/qml/pages/TranslatePage.qml：StatusDot.detailMaxWidth 160 → 220（塞得下「模型 · GPU」）。
- rontend/qml/components/StatusDot.qml：detail 被省略时悬停出 ToolTip 看全（HoverHandler + ToolTip，import QtQuick.Controls）。

验证：
- --ui-shot --ui-page 2 截图里两卡都无折叠箭头，四行副标题分别为「在线接口，无需下载资产」「qwen2.5-0.5b-instruct · GPU · 翻译时自动启动服务」「直连 (mt.opus.en-zh)」「deepseek-v4.1-flash · 云端」。
- 跳转联调：临时在 Component.onCompleted 末尾 Qt.callLater(... openEngineConfig("local-mt", false))，截图可见「本地 MT」分段高亮 + 面板切到语言对表（**验完已删除**）。
- update_translations → 339 条 0 new（新增文案走后端 i18n::s(zh,en) 双参，不进 ts）。

踩坑：给 engineCard 补 collapsible: false 时多写了一行 expanded: true，qmlcachegen 直接报 Property value set multiple times；studious.exe 残留会让链接报 cannot open output file studious.exe: Permission denied。

## 未配置状态说清缺什么 + 窗口默认 1120×720（2026-11-07 02:40）

用户要求：把「云端 LLM 没配 / 本地 LLM 没模型 / 引擎没用」的副标题补上；窗口改成 1680×1080（微信截图的物理像素）。

改动：
- `frontend/src/AppSettings.cpp`：新增 `textOrKeepEmpty`，`llm/baseUrl` 与 `llm/model` 改用「只有注册表没这个键才用默认值」。**这是行为变更**：以前用 `textOr`，空串会被默认值（`http://127.0.0.1:3065/v1` / `deepseek-v4.1-flash`）悄悄填回去，于是 `LlmConfig::isValid()` 恒真、云端永远是绿的，用户清空后重启又"自己好了"。
- `frontend/src/AppBackend.cpp`：
  - 云端 LLM 就绪 → 「模型 · 云端」；未就绪 → 按缺项拼「缺 Base URL、模型名 · 去下面的「引擎配置」填」，都填了才回落到后端 `resolve()` 原话。注意 `isValid()` 只查 baseUrl + model，**空 API Key 合法**（本地 /v1 网关）。
  - 本地 LLM 分支重排：先判资产（缺运行时和模型 / 缺运行时 / 缺模型 → 红「缺资产」），再判「配置点名的模型文件没了 → 黄「未就绪」+「配置的模型 xxx 不在了 · 去「引擎配置」改选一个」」，最后才是正常文案。`localHasModel` 也算上"配置里写的绝对路径存在"这一种。以前只要 models/ 里还有别的模型就报就绪，一点翻译却说"缺少模型"。
- `frontend/qml/Main.qml`：`width/height` 改为 1120×720 逻辑（= 用户量的 1680×1080 物理 ÷1.5），并 `min(…, 可用区域 × 0.92)` 防小屏溢出；`minimumWidth/Height` 1120×720 → 1040×660。
- `frontend/src/main.cpp`：`--ui-shot` 不给 `--ui-size` 时不再写死 1440×900，改为沿用窗口真实尺寸（以前截图永远看不出默认大小）。

验证：
- `--ui-shot` 不带 `--ui-size` → `1680x1080 @1.5x, logical 1120x720`。
- 场景矩阵（改注册表造态，截完均已还原）：本地 LLM 配置模型不存在 → 黄 + 「配置的模型 does-not-exist 不在了…」；assetRoot 指向空目录 → 本地 LLM 红「缺 llama 运行时和模型文件」、本地 MT 红「没装任何语言对…」、云端保持绿；baseUrl+model 清空 → 黄 +「缺 Base URL、模型名 · 去下面的「引擎配置」填」；只清 model → 「缺模型名 · …」。
- `backend_selftest.exe` → `ALL CORE TESTS PASSED`；`--smoke … --engine local-mt` → 「你好,世界观!」1342 ms exit 0。
- 截图：`docs/ui-shots-qml/size_1680.png`、`state_model_gone*.png`、`state_cloud_empty*.png`、`state_cloud_model_empty*.png`、`state_no_assets2*.png`、`state_normal2*.png`、`llm_row_noassets.png`。

坑：注册表里 `-1` 写成 `REG_DWORD 0xFFFFFFFF`、`localModel` 才是模型文件名（不是 `localModelFileName`）；`Set-ItemProperty -Value ""` 与「键不存在」在 `textOr` 眼里一样，造"未配置"态必须走 `textOrKeepEmpty` 这种「存在但空」的判据。
## 配置页：给「高级设置」加底色与悬浮（2026-11-07）

问题：`ConfigPage.qml` 的内联 `SubSection` 只有一行文字 + 一个 ▾，直接画在卡片底色上，
既看不出这是一组设置，也看不出能点开。

改法（只动 `frontend/qml/pages/ConfigPage.qml` 的 `component SubSection`）：

- 标题条换成 `Rectangle { id: headBox }`，`radius: Theme.rSm`、1px 边框、`implicitHeight: sectionHead.implicitHeight + Theme.s2 * 2`；
  底色 `Theme.surfaceHigh`，悬浮或展开时抬到 `Theme.surfaceHighest`，边框 `outlineVariant` → `outline`；
  两者都套 `Behavior on color { ColorAnimation { duration: Theme.durFast } }`。
- 交互从 `MouseArea` 换成 `HoverHandler { cursorShape: Qt.PointingHandCursor }` + `TapHandler { onTapped: section.expanded = !section.expanded }`，
  这样悬浮态和点击态可以分开描述，光标也不会在禁用时还是手型。
- 标题与箭头的文字色统一成 `Theme.ink`（原来折叠态是 `inkVariant`，在 `surfaceHigh` 上偏灰）。
- 展开后的内容再套一层 `Rectangle { id: bodyBox }`：`color: Theme.surfaceDim`（比卡片更深 = 内嵌下凹）、
  同圆角 + `outlineVariant` 边、`implicitHeight: body.implicitHeight + Theme.s4 * 2`；
  里面的 `ColumnLayout { id: body }` 用 `x/y: Theme.s4` + `width: bodyBox.width - Theme.s4 * 2`，
  不用 `anchors.fill`，避免 `implicitHeight` 依赖子项、子项又依赖父高度的循环绑定。

踩坑：

- 第一版用 `surfaceLow`（深色 ≈ 卡片底色），截图里几乎看不出来；逐步抬到 `surfaceMid` → `surfaceHigh` 才够。
  结论：深色主题下"能看出是一个控件"的最低档是 `surfaceHigh`，`surfaceLow/Mid` 只适合做内嵌内容区。
- 展开态截图要临时给两个 `SubSection` 加 `expanded: true`（`ConfigPage.qml` 的 1467 / 1753 行附近），
  截完必须删掉 —— `grep TEMP-SHOT` 应为 0。
- 写完 QML 用 `[System.IO.File]::WriteAllText` 时要显式传 `UTF8Encoding($false)`，且改完核对 CRLF 计数为 0。- 追加修正：内容面板最初用 `Theme.surfaceDim`（深色下比卡片更暗），用户反馈「背景太深了」→ 改成 `Theme.surfaceLow`。
  深色下 `surfaceDim`(0.03) / `surfaceLow`(0.07) / `surfaceMid`(0.09) / `surfaceHigh`(0.12) / `surfaceHighest`(0.15)
  是按 seed 混进底色的比例，所以「标题条 surfaceHigh + 内容面板 surfaceLow」是既有层次又不压暗内容的组合。
## 翻译页重做成入口页（2026-11-07）

用户要求：原文对照挪到「开始翻译」的位置、删掉原文/译文/开始翻译、空位放三张卡片
（小窗口 / 截图翻译 / 实时翻译），带图标和悬浮动画，事件先预留。

改动：
- `frontend/qml/pages/TranslatePage.qml` 整体重写（1055 → 约 730 行）。
  - 删掉整块双栏（原文 TextArea + 译文分栏 + 块渲染 + 逐段原文 + FAB），
    连带 `srcArea/translatedArea/paneBodyMinHeight/paneMinHeight` 等一整套。
  - 语言对（源/目标两列，各 200px）、引擎 Segmented、状态行、警告条、引擎状态条保留。
  - 「原文对照」开关移到右侧（原 FAB 位置），旁边是缓存键 Badge 与提示文案。
  - 新增内联 `component LauncherCard`：32px 图标（`MultiEffect` 按主题重上色）、
    标题、描述、要点一行、底部「接口已预留」+ 悬浮时才出现的 →。
    悬浮：底色 `surface → surfaceHigh`、描边 `outlineVariant → primary`、
    整卡 `scale 1.0 → 1.012`、图标/要点变强调色，全部 150ms（`Theme.durFast`）。
    交互用 `HoverHandler` + `TapHandler`（`MouseArea` 会吞掉子项 hover）。
  - 预留信号 `launchRequested(string kind)`，kind = `"mini" / "capture" / "live"`；
    `Main.qml` 暂无消费者。
- 新增图标 `frontend/resources/icons/{mini,screenshot,realtime}.svg`（24×24 描边，
  与既有图标同款），加进 `frontend/CMakeLists.txt` 的 `RESOURCES`。
- 状态条：`detailMaxWidth` 220 → 120、项间距 `s5` → `s4`，1120 宽下四项都能露出。
- 译文：`update_translations` 12 条新文案全部补英文，`studious_en.ts` unfinished = 0。

验证：
- 构建 `[9/9] Linking CXX executable studious.exe`，页面无 QML 警告。
- 截图 `docs/ui-shots-qml/launcher4.png`（深色）、`launcher4_light.png`（浅色）、
  `launcher_en.png`（英文，临时改注册表 `ui/language=en`，截完已复原 `zh`）。

坑：
- `Row` 里 delegate 用 `width/height` 会让 `ScrollView.contentHeight` 归零，
  必须用 `implicitWidth/implicitHeight`（状态条那次已经踩过一次，这里沿用）。
- 卡片用 `Layout.fillHeight: true` 撑满剩余高度后中段会空一大块，
  补一行要点 + 底部 1px 分隔线才不至于像个空盒子。
## 深色主题图标看不见：根因与修法（2026-11-07）

现象：深色主题下所有单色图标（左栏、设置导航、引擎行、入口卡）都是暗的，几乎看不清。

根因：`MultiEffect` 的 `colorization` 是**把源像素乘以 colorizationColor**，
不是替换颜色。图标 SVG 的描边原本写死 `#4C3B79`（深紫），乘一个亮色仍然暗 ——
所以不管 tint 给多亮，图标都还是深的。之前的「按主题重新上色」一直没真正生效。

修法：把 14 个图标的描边/填充从 `#4C3B79` 改成 `#FFFFFF`（纯白）。
白色 × 任何 tint = tint，于是每处 `colorizationColor` 就成了图标的精确颜色：
- 左栏选中 `railIconActive`（深色主题近白）/ 未选中 `railIconIdle`
- 设置导航、引擎行同理
- 入口卡：常态 `inkVariant`（深色主题浅灰）、悬浮 `primary`（浅紫）
- 品牌图标：`primaryText`（深色主题深色，压在浅紫圆片上，对比度正好）

走过一条弯路：先试了自写 `ShaderEffect`（片元着色器只取 alpha、RGB 换成 tint）。
Qt 6 里 `fragmentShader` 的**内联字符串会被当成相对 URL**，报
`ShaderEffect: shader preparation failed`；改成外部 `.frag` 文件放进 RESOURCES
依然同样报错（这个 MinGW 构建没跑通运行时 shader 编译）。组件已删除，
CMake 里的 `Icon.qml` / `icon.frag` 条目也回滚了。

验证：`icons_dark.png`（深色，三张卡图标浅紫、侧栏亮）、`icons_page2.png`
（配置页四行引擎图标全可见）、`icons_page3.png`（设置七个分区图标全可见）、
`icons_light.png`（浅色仍为深色描边，未变淡）。
---

## 平台层收口 + 双平台兼容检查（2026-11-07，版本 0.9.0-separate）

### 做了什么
1. **孤儿守护搬家**：新建 `frontend/src/platform/ProcessGuard.{h,cpp}`，把原本写在 `LocalEngine.cpp` 里的
   Job Object 代码（`CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` + `AssignProcessToJobObject`）
   整块搬进平台层。`LocalEngine.h` 现在只持有一个 `studious::platform::ProcessGuard m_guard`，
   并在 `start()` 成功后 `m_guard.attach(processId())`、`shutdown()` 里 `m_guard.release()`。
   于是 `frontend/src` 里 `#include <windows.h>` 只剩 `platform/Desktop.cpp` 与 `platform/ProcessGuard.cpp` 两处。
2. **去掉 win-x64 / llama-server.exe 硬编码**：`LocalEngine.cpp` 的 `kCpuRuntimeId` / `kVulkanRuntimeId` /
   `kServerExeName` 三个常量删掉，换成 `cpuRuntimeId()` / `vulkanRuntimeId()` / `serverExeName()`，
   内部走 `IPlatform::runtimeAssetSuffix()` 与 `IPlatform::serverExecutableName()`。报错文案里的资产 id 也改成拼出来的。
3. **`createOverlay()` 返回类型 `QWidget*` → `QWindow*`**：界面是 Qt Quick，浮层要用 `QQuickWindow` 画，
   Widgets 从没进过链接表。`IPlatform.h` / `NullPlatform.h` / `.cpp` 同步改。
4. **Linux 开机自启真实现**：`Desktop::setAutoStart` / `autoStartRegistered` 的 `#else` 分支不再返回
   "当前平台暂不支持"，改成写 `\/autostart/studious.desktop`（Exec 加引号，兼容带空格的路径）。
5. **`backend/src/runtime/LlamaServer.cpp` 报错文案去平台化**：不再点名 `runtime.llama.cpu.win-x64`，
   改成"对应的 llama.cpp 运行时"（具体 id 由前端 `LocalEngine` 补，那边知道平台后缀）。
6. **界面文案去 Windows 化**：设置页"已写入当前用户的启动项（HKCU\...\Run）" → "已写入当前用户的自启动项。"
7. **底层总结**：新增 `frontend/src/platform/README.md`——文件清单、平台头规则、能力矩阵（Windows / Linux 逐项）、
   静态入口、加平台的四个步骤、已知遗留（Linux llama 运行时占位、onnxruntime 只有 Windows 版、Wayland 抓屏限制）。

### 验证（全部在本机 Windows 上真跑）
- 构建：`cmake --build frontend/build` → `[13/13] Linking CXX executable studious.exe`，0 error。
- `--smoke "Hello, world!" --from en --to zh --engine local-mt` → 你好,世界观!，exit 0。
- `--smoke "The quick brown fox..." --from en --to zh --local` → 后端=vulkan、pid、baseUrl 18080、
  译文正确、停服务 3077 ms、`llama-server` 残留 0（说明平台后缀拼出的 exe 路径对）。
- Job Object 兜底：置 `localPreheat=1` 起 GUI → 9 秒后 `llama-server` = 1 → `Stop-Process studious -Force`
  → 5 秒后 `llama-server` = 0（ProcessGuard 生效）。
- `backend_selftest.exe` → `ALL CORE TESTS PASSED`。
- `--ui-shot` 两张：`docs/ui-shots-qml/plat_config.png`（配置页四行引擎照旧全绿）、`about_090.png`；
  `[appinfo] Studious 版本 0.9.0-separate`。
- 翻译：`update_translations` → 335 条、4 new（三条自启报错 + 一条成功提示），补完 `unfinished = 0`。

### 坑
- `ProcessGuard.cpp` 里必须 `#if defined(Q_OS_WIN)` 包住整段，否则 Linux 上 `HANDLE` 未定义；
  非 Windows 分支写成显式空实现（而不是不定义函数），这样 POSIX 版本补进来时只改这一个文件。
- `Desktop.cpp` 的 `xdgAutoStartFile()` 放在 `#ifndef Q_OS_WIN` 的匿名命名空间里，
  否则 Windows 构建会有"未使用函数"告警。
- 之前那条"`--ui-section 6` 拍关于页"的钩子在 `if (sectionIndex > 0)` 里，行为正常；
  但设置页分区下标改动过，拍关于页要重新确认下标（本次用 `[appinfo]` 行验证版本号即可）。
## 批次 1：平台底座 + OCR + 记录 + QuickTranslate（2026-11-07）

目标：把 docs/TempUI/flows.html 的三条新流程（截屏翻译 / 小窗 / 实时）的地基落地，Linux 只留接口。

### 平台层（frontend/src/platform/，Win 实现 + 其他平台空实现）
- 新增 `ScreenCaptureWin.{h,cpp}`：GDI `BitBlt(SRCCOPY|CAPTUREBLT)` + `CreateDIBSection`，`Format_RGB32`（GDI 不写 alpha），句柄 RAII；调用线程内临时 `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)`；`GetDpiForMonitor` 动态解析（回落 96）；全黑抽样判失败（独占全屏/HDR 需 DXGI，留 `// TODO(dxgi)`）。
- 新增 `OverlayWin.{h,cpp}`：只改 `GWL_EXSTYLE`（恒 `TOOLWINDOW|NOACTIVATE`，穿透加 `LAYERED|TRANSPARENT`）+ `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`；**不碰 `QWindow::setFlags()`**（会重建原生窗口丢 QML 场景）。坑：`WS_EX_LAYERED` 与 D3D11 flip-model 组合可能黑屏，排查先 `QSG_RHI_BACKEND=opengl`。
- 新增 `HotkeyWin.{h,cpp}`：`HWND_MESSAGE` + `RegisterHotKey`，窗口过程只排队、emit 回事件循环避免重入；`HotkeySpec` 无文本字段 → `key==0` 时把 `description` 当 `"Ctrl+Alt+S"` 解析。坑：mingw 的 `winuser.h` 没有 `VK_A..VK_Z`，字母用 `'A'..'Z'`。
- 新增 `WinPlatform.{h,cpp}`：组合上面三个 + 能力矩阵；`createOverlay()` 返回 nullptr（浮层是 QML 的 QQuickWindow，平台层只负责改造窗口）。
- `Types.h`：`MonitorInfo` 加 `workRect`（rcWork，物理像素，浮层定位避任务栏）。
- `IPlatform.h` 加 `HotkeyHandler` 回调出口 + `setHotkeyHandler()`（IPlatform 不是 QObject，不用信号）。
- `PlatformFactory.cpp`：`#if defined(Q_OS_WIN) → WinPlatform`，其它平台仍是 `NullPlatform`（注释写明 X11/Wayland 的硬限制）。

### 后端
- 新增 `backend/include/studious/ocr/OcrEngine.h` + `backend/src/ocr/OcrEngine.cpp`（PP-OCRv4：DBNet 检测 + CRNN/CTC 识别，pimpl 不泄漏 onnxruntime；不做 CTRBox 旋转分类，理由写在头文件）。
- `backend/CMakeLists.txt`：加 Gui（QImage）与 `../frontend/src` include（复用 `platform::OcrWord`）。
- `backend/asset/manifest.json`：加 `ocr.det/rec/dict.ppocrv4` 三条，`sha256` 与 `urls` **故意留空**（Downloader 会拒装而不是假装成功）——要真用 OCR 必须补真实地址与哈希。

### 前端功能层
- 新增 `QuickTranslate.{h,cpp}`：不与 AppBackend 共用 job；进程级共享 `LocalEngine`（三页共用一台 llama-server）与共享缓存；自动引擎走 priority + 降级，显式引擎才写 engineId；`TranslateJob` 没有 cancel()，所以 `cancel()` 是逻辑取消（真结果丢弃）。
- 新增 `TranslatorRecords.{h,cpp}`：SQLite + WAL + 2000 条剪枝；`list()` 只回顶层行、`children(parentKey)` 展开实时会话；`imagePath`/`region` 支持 B7 的缩略图与选区。
- 删掉重复实现：`backend/src/records/Records.cpp` + `backend/include/studious/records/Records.h`（另一套三表模型，全仓无人引用；与"以后端为准"冲突时以 UX 契约优先）。
- 新增 `ScreenFlow.{h,cpp}`：状态机 Idle→Capturing→Selecting→Recognizing→Translating→Showing(→ShowingOriginal)/Error；**先抓整屏再显示遮罩**；OCR 在工作线程（QFutureWatcher + 截图 token 作废在途结果）；段落与译文块 1:1 对齐（数量不符补空串而不是错位）。
- `AppSettings`：新增 `capture/*`、`overlay/*`、`mini/*`、`live/*`、`record/saveMode`、`hotkey/{capture,mini,live}` 等键（load/save/defaults 全齐）。
- `main.cpp`：GUI 路径新建平台实例 + `ScreenFlow`，注册三个热键（失败只 `qWarning`，不影响启动），把 `screenFlow` / `platformName` / `platformCanCapture` / `platformCanHotkey` 注入 QML 上下文。

### 验证
- `cmake --build frontend/build` 通过（`[4/4] Linking CXX executable studious.exe`）。
- `--ui-shot docs/ui-shots-qml/batch1_start.png --ui-page 0 --ui-size 1120x720` 正常出图（1680x1080），QML 无警告。
- `--smoke "Hello, world!" --from en --to zh --engine local-mt` → 「你好,世界观!」exit 0。
- 本机 `Ctrl+Alt+W` 被别的程序占用（注册失败只打日志，不影响其余热键）——说明热键冲突路径真的走到了。

### 未完成（下一批）
- flows.html 的 A/B/C 三套界面（选区遮罩+工具栏+译文块、小窗、实时浮层+控制条）与 D 二级设置页。
- `LiveRunner`（增量 OCR + 只在文本变化时翻）。
- 记录面板接线（`ScreenFlow::saveRecordRequested` 目前没有消费者）。
- OCR 资产真实下载地址与哈希。
## 批次 2：A 页截屏翻译（2026-11-07）

### 做了什么
- `frontend/qml/overlay/CaptureOverlay.qml`（约 620 行）：一个窗口承担 A1–A10。
  - 底图 = 抓屏 PNG（`screenFlow.imageSourceUrl()`，绑定里显式引用 `screenSize` 才会随每次抓屏重新求值）。
  - 压暗用四块 Rectangle 围出镂空（不是「大矩形 + 选区反色」——那样选区内会被压两次）。
  - 选区：1.5px 边 + 尺寸标签 + 八个手柄；松手只到「已选、等确认」，翻译要按工具栏（或开「选完直接翻译」）。
  - 工具栏：引擎槽（Dropdown 改 `backend.engineId`，与翻译页/配置页同一个值）+ 翻译/重新翻译 + 重新框选 + 显示原文/译文 + 复制 + 关闭 Esc；放不下自动翻到选区上方。
  - 译文块 `plateLayer`：按 OCR 行 1:1 贴纸，底色 `surfaceLow`（设计稿 `--plate`）+ `backend.overlayOpacity` 不透明度，字号 `backend.overlayFontSize` 再按行高收一次；「保留原文标注」在译文上方压一条薄原文条。
  - 错误态 `errBox` 用 `dangerContainer` 底 + **`dangerContainerText`**（`dangerText` 在深色主题下是深红，压在 danger 容器上看不清 —— 本轮踩到并修掉）。
- `ScreenFlow` 新增：`Q_PROPERTY(QRect desktopRect)`、`Q_INVOKABLE void debugSampleBlocks()`（仅截图走查）、自动复制（`captureAutoCopy`）。
- `AppBackend` 新增 capture/overlay 六个属性（`captureAutoRun/captureAutoCopy/captureAutoHide/overlayOpacity/overlayFontSize/overlayKeepOriginal`），全部落盘到 `settings/capture|overlay/*`。
- `main.cpp` 截图钩子扩展：`STUDIOUS_UI_TARGET=screenFlow`、`STUDIOUS_UI_CALL=a|b|c`（`|` 分隔顺序执行）、`STUDIOUS_UI_SELECT="x,y,w,h"`（摆到二次确认态）、`STUDIOUS_UI_RECT`（直接送翻译）。正式启动路径完全不看这些变量。

### 坑
1. **`Screen.virtualX/virtualWidth` 在这版 Qt 6.12 的 QML 里取不到**（`undefined`），`Qt.rect(...)` 变 `QRectF(0,0,nan,nan)`，浮层窗口只有 160×160。改成 C++ 暴露 `ScreenFlow::desktopRect`。
2. **`forceActiveFocus()` 在 `Window` 上不存在**（`ReferenceError`），要用 `requestActivate()`。
3. `MultiEffect.colorization` 是**乘法**不是替换 —— 图标 SVG 描边改白色之后，各处 `colorizationColor` 才是精确颜色。
4. `Popup` 的 parent 要给 `contentItem`，直接把 Window 当 parent 拿不到可视父级。
5. 截图钩子的 `STUDIOUS_UI_TARGET` 忘了设时，调用会静默打到 `backend` 上（方法不存在 → 什么都不发生，截图是主窗口而不是浮层）。

### 验证
- `capture_a2.png`（选区 + 六键工具栏 + 引擎槽「本地 LLM ▾」）、`capture_a4.png`（三块译文贴纸）、`capture_a5.png`（显示原文 + 描边）、`capture_a6.png`（缺 OCR 模型的错误态）。
- 压暗实测：选区内外同一行像素 (11,10,14) vs (20,18,26) —— 外侧确实压暗。
- `--smoke --engine local-mt` → 你好,世界观! exit 0；`backend_selftest.exe` → ALL CORE TESTS PASSED；翻译 371 条 0 unfinished。

## 翻译页入口卡接线（2026-11-07）

**用户要求**：把翻译页「截图翻译」卡片的事件绑到新做的截屏翻译上。

**根因（真的是 bug，不是没接线）**：三张卡片都是内联组件 `component LauncherCard`，
实例上的处理器写成 `onActivated: page.launchRequested(card.kind)`。
`card` 是组件**内部**的 id，在实例级处理器的作用域里不可见 —— 运行时抛
`ReferenceError: card is not defined`（TranslatePage.qml:646），信号发出去、处理器一进去就断，
所以点卡片**完全没有反应**，而且只在控制台留一行 qml 报错，界面上看不出任何异常。
改成每张卡写字面量 `page.launchRequested("mini"/"capture"/"live")`，问题消失。

**顺带的改动**
- 三张卡加 `objectName`（`cardMini` / `cardCapture` / `cardLive`），`TranslatePage` 实例加
  `objectName: "translatePage"` —— 截图走查要按名字找对象。
- 卡片底部小字由固定「接口已预留」改成 `required property string footer`：截图翻译是
  「点一下开始框选」，另两张仍是「接口已预留」。**别写死热键**：`Ctrl+Alt+S` 是设置项，
  写进文案就会和用户改过的值打架。
- `--ui-shot` 分支新增 `STUDIOUS_UI_EMIT="objectName:signal:arg"`（先按 `signal(QString)` 找，
  找不到再按零参 `signal()` 找），用来验「点下去之后」的链路；新增 `STUDIOUS_UI_SHOT=overlay`
  改抓浮层窗口，并把日志里的逻辑尺寸改成按**被抓的那个窗口**算（原来抓整屏却报 1120x720）。
- 去掉浮层里那行 `console.log`（Qt 6.12 的 `Screen.virtualWidth` 在 QML 侧取不到，
  已经改成 C++ 的 `ScreenFlow::desktopRect`，日志留着只是噪声）。

**验证**
- `STUDIOUS_UI_EMIT=cardCapture:activated:x STUDIOUS_UI_SHOT=overlay` →
  `[ui-shot] after emit: phase=Selecting`、`captureOverlay 1707x1067 visible=1`、
  截图 `docs/ui-shots-qml/card_capture.png`（2561x1601，整屏压暗 + 「拖拽框选」提示）。
- 常态页截图 `docs/ui-shots-qml/launcher_bound.png`：截图卡底部「点一下开始框选」，
  另两张「接口已预留」，零 QML 警告。
- `STUDIOUS_UI_EMIT=cardMini:activated:x` → 顶部通知条出现（与常态页对比前 300px 带内
  抽样差 10223 点），docs/ui-shots-qml/card_mini_notice.png。
- 翻译链路未受影响：`--smoke "Hello, world!" --from en --to zh --engine local-mt` → 你好,世界观!
- 译文：`update_translations` 372 条（1 new =「点一下开始框选」），补英文后 unfinished = 0。

**坑**
- 内联组件的内部 id **不能**在实例的处理器里用；要么写字面量，要么把值提成 `required property`
  再在实例上赋值（属性本身在实例作用域里是可见的）。
- `QMetaObject::invokeMethod` 默认匹配签名，给零参信号硬塞 `Q_ARG(QString, …)` 只会返回 false，
  看起来像「信号没连上」。判 `indexOfSignal` 更省事。
- `QQuickWindow::findChild` 要用窗口对象，别从 `contentItem()` 往下找 —— 页面级 objectName
  不在那棵子树里。


## 截屏翻译：选中后没有工具栏（2026-11-07）

用户报："可以选中翻译区域了但是选中后没有出现工具栏，选中后就只是被选中了一下"。

### 两个独立根因

1. **CaptureOverlay.qml 里的 property bool active 撞了 Window::active**
   Window 自带 ctive（窗口是否拿到键盘焦点）。我写的自定义 ctive = selecting || working || showing || errored 被它覆盖/覆盖不了，
   而 isible: active 于是变成"窗口获得焦点才可见"。浮层窗口是 WS_EX_NOACTIVATE（platform/OverlayWin.cpp），**永远不会激活**，
   所以浮层在实际运行里几乎从不显示。
   而 --ui-shot 一直能拍到它 —— QQuickWindow::grabWindow() 是离屏渲染场景，跟窗口是否真显示无关。
   这就是"截图看得见、人看不见"的分裂来源，也是最容易骗过自己的坑。
   修法：改名 ctive → engaged，isible: engaged，三处引用同步。

2. **松手顺序错了（onReleased）**
   原代码 overlay.dragging = false; const r = overlay.liveRect; —— liveRect 的定义是
   dragging ? 临时框 : (localRect.width > 0 ? localRect : sel)，一清 dragging 两个来源都是空， 恒为 0x0，
   commitSelection 里 width < 3 直接 return，工具栏的 isible: liveRect.width > 1 永不满足。
   修法：先按 mouse.x/mouse.y 与 dragX/dragY 算出矩形，**再**清 dragging，再 commitSelection(r)。

### 开发钩子（main.cpp）
- STUDIOUS_DEV_CAPTURE=1：正常启动 1.2 s 后自动 screenFlow->begin()，方便手动拖框走查。注意它必须写在 --ui-shot 分支**之外**
  （排查期一度写进分支内，导致"手动拖拽全都没生效"的假象）。
- STUDIOUS_UI_COMMIT="x,y,w,h"：直接对浮层窗口 invoke commitSelection，单独验证"松手之后"这段。
- --ui-shot 日志改为按实际被抓窗口报逻辑尺寸。

### 像素证据（全屏 2560x1600 抓图）
- 选区内部平均亮度 39.9 / 选区外 17.9 / 远处 18.9 → 选区内是未压暗的原始截屏。
- 工具栏带 y=624..668：max=185 bright=80 → 文字在，工具栏确实画出来了。
- Esc 后桌面角落 89 → 105.4 → 压暗消失，浮层已关。

### 教训
- QML 里 Window / Item 已有的属性名（ctive、isible、contentItem、parent）不要拿来当自己的状态位，
  shadowing 不会报错，只会静默变成另一个语义。
- 截图能拍到 ≠ 用户能看到：grabWindow() 走离屏渲染，验证可见性要用全屏抓图或像素对比。

## 截屏浮层统一到主界面视觉（2026-11-07）

要求：浮层（遮罩 + 工具栏）的颜色/圆角/字号跟主界面一套，配色走外观页里配的强调色；引擎选择器留在工具栏内（用户明确选了这一档，不单独钉左上角）。

改动全部落在 `frontend/qml/overlay/CaptureOverlay.qml`，只换令牌、不动结构：

| 位置 | 之前 | 现在 |
|---|---|---|
| 选区边框 / 八手柄 | `Theme.primaryText`（近白） | `Theme.primary`（强调色），边框 1.5 → 2px |
| 尺寸标签 | 黑 72% + 白字 | `Theme.surfaceHigh` + `outlineVariant` 边 + `Theme.ink` 字 |
| busy 框 | 黑 55% + 白字 | `surfaceHigh` 96% + 边，字 `ink` / `inkVariant` |
| 工具栏底 | 黑 82% | `surfaceHigh` 97%，圆角 `rSm` → `rMd` |
| 工具栏分隔 / 阶段标签 | 白 18% / 白 65% | `outlineVariant` / `inkVariant` |
| 按钮 | 白 10% 底、白字 | 次按钮透明底 + 悬浮 `surfaceHighest`；主按钮 `primary` 实心 + `primaryText` |
| 空选区提示 | 裸文字（caution） | `cautionContainer` 胶囊 + `caution` 边 |

好处不只是好看：强调色换掉后浮层整体跟着变（实测 border/主按钮在深色是 `#9A8EC6`、浅色是 `#6750A4`，即从 seed #6750A4 推出来的那份）。

### 验证
- 深色 `docs/ui-shots-qml/ov_theme_a2.png`：边框 `#9A8EC6`、工具栏底 `#2A2736`（surface 系，不是纯黑 `#0B0A0E`）、主按钮 `#9A8EC6`。
- 浅色 `ov_theme_a2_light.png`：边框 `#6750A4`、工具栏底 `#DCD6E7`、主按钮 `#6750A4`。
- 主界面回归 `ov_main_check.png` 零 QML 警告；`--smoke … --engine local-mt` → 你好,世界观!。

### 备注
- 抓图用 `STUDIOUS_UI_TARGET=screenFlow STUDIOUS_UI_CALL=begin STUDIOUS_UI_SELECT="300,220,760,220" STUDIOUS_UI_SHOT=overlay`，一条命令摆出 A2 态。
- 本轮第一次跑 local-mt 冒烟撞到已知的间歇 `exit -1`（header 打完即死），重跑三次均正常，与本次改动无关。

## 引擎下拉弹到屏幕左上角（2026-11-07）

现象：点工具栏里的引擎槽，下拉菜单不贴着按钮出现，而是跑到整个屏幕的左上角。

### 两个原因，一个比一个隐蔽

1. **用 Popup 做浮层内的下拉**。Popup 是**独立顶层窗口**，x/y 只要算偏就贴屏幕角；
   而且我们的浮层本身是 WS_EX_NOACTIVATE 的全屏窗口，弹出去以后 z 序还要自己管。
   改成窗内 Rectangle（id: engineMenu，z=20），天然跟着工具栏走，isible 绑 menuOpen。
2. **mapToItem() 写在绑定里不会产生依赖**。原来写
   engineSlot.mapToItem(overlay.contentItem, 0, 0)，这是 C++ 调用，内部读的 x/y 不进入
   QML 依赖图；它只在第一次求值时算了一次 —— 那时布局还没跑，得到 (0,0)，之后再也不会更新。
   **日志实证**：MENUTEMP anchor 0 0 pos 8 38。
   改成逐级相加：Qt.point(ovbar.x + barRow.x + engineSlot.x, ovbar.y + barRow.y + engineSlot.y)，
   每个坐标都是属性读取，依赖成立。同样的日志变成 nchor 558.46 459 pos 558.46 497，
   即菜单正好贴在引擎槽下沿 8px。

### 附带
- 点空白处收起：加一层 MouseArea { z: 4; visible: menuOpen }（低于工具栏 5、高于内容），
  工具栏按钮照样能点，菜单自身 z=20 在它上面。
- Esc 先收菜单再退出框选；开始新拖拽、阶段回 Idle 也都会把菜单关掉。
- 开发钩子 STUDIOUS_UI_EMIT 挪到 SELECT/COMMIT 之后执行，并且会在**所有顶层窗口**里
  找 objectName（浮层的控件不在主窗口树里），这样才能脚本化点开引擎槽取证。

### 教训
- QML 里凡是"按位置算"的东西（mapToItem / mapFromItem / mapRectToItem）都别直接放进绑定，
  要么放到信号处理里现算，要么拆成属性相加。
- Popup 适合主窗口内的下拉；全屏浮层里宁可用窗内面板。
## 截屏翻译：选区放大镜（2026-11-07）

微信截图那种「放大镜」：拖框时在光标旁边显示一块放大的像素视图，下面列坐标、色值和 RGB。

- `frontend/src/ScreenFlow.{h,cpp}`：新增 `Q_INVOKABLE QVariantMap pixelInfoAt(int x, int y)`，从 `m_shot` 取像素返回 `{ok, x, y, hex, r, g, b}`。**QML 的 Image 读不到自己的像素**，色值只能由 C++ 给；`hex` 直接能当颜色字符串用，省得在 QML 里拼十六进制。
- `frontend/qml/overlay/CaptureOverlay.qml`：新增 `property point cursorPos: Qt.point(-1, -1)` + `MouseArea { hoverEnabled: true }` 跟踪光标；放大镜是一块 `Rectangle`（`objectName: "magnifier"`），上半是放大区 + 十字线 + 中心像素描边，下半是 `X/Y`、色块 + `#RRGGBB`、`R/G/B`。
- 位置：**竖向优先放在光标上方**（工具栏默认贴选区下方，往上一躲就不会互相压），上方放不下才翻到下面；横向在光标左侧，贴左边就翻到右侧。
- z 序压到工具栏之下（`z: 4`，工具栏 5）：放大镜跟着光标跑难免压到「翻译」按钮，主按钮必须永远在上面。
- **只在拖框时出现**（`visible: overlay.dragging`）：松手、选区定了就收起来 —— 第一版写成 `selecting` 就一直挂在画面上，用户当场指出。

### 三个坑

1. **`Image` 在 Qt 6 没有 `sourceRect`**（`Cannot assign to non-existent property "sourceRect"`）。要「只画源图的一小块」只能：把整屏底图按 `zoom` 倍放大、用 `Item { clip: true }` 只露出光标那一块，再按 `-(cursorPos * zoom) + 视口半宽` 平移。`smooth: false` 才有像素块质感。
2. **`QString("…")` 在 QML 里不存在**（`ReferenceError: QString is not defined`）—— 直接写 `"—"` / `""`。
3. `MouseArea.onExited` 里清 `cursorPos` 会把放大镜一起关掉：选区一旦 `confirmed`，`MouseArea` 被 `enabled: false` 停用，Qt 随即派一个 exit 事件。清坐标改挂到「阶段回 Idle」那一处。

### 新增截图钩子

`frontend/src/main.cpp` 的 `--ui-shot` 分支：`STUDIOUS_UI_CURSOR="x,y"`（摆假光标，真光标在自动截图里动不了）、`STUDIOUS_UI_DRAGGING=1`（配合前者假一份「正在拖框」，否则放大镜不显示）。

### 验证

- 深色：`docs/ui-shots-qml/magnifier5.png`（坐标 400/480、色值 `#151515`）、`magnifier6_crop.png`（`X 1050 Y 450` / `#FBF4F8` / `R251 G244 B248`，与页面底色一致 → 取色正确）。
- 拖框中：`mag_drag.png` + `mag_drag_crop.png`。
- 松开后：`mag_after.png` + `mag_after_crop.png`（放大镜已消失，工具栏仍在）。
- `--smoke "Hello, world!" --from en --to zh --engine local-mt` → exit 0；`backend_selftest.exe` → `ALL CORE TESTS PASSED`。
- 新增文案 1 条（「光标不在画面内」/ Cursor is outside the frame），`studious_en.ts` unfinished = 0。
### 追加：位置改右下角 + 倍率可配（2026-11-07）

用户要求「放大镜放鼠标右下角、倍率小一点、不要像素级的、别写死以后在设置里调」。

- 位置：x/y 都改成「光标 + 间距」，右边/下边放不下才翻到另一侧（原来是左侧 + 优先上方）。
- 视觉：`smooth: true`（原来 `false` 是为了看清像素块），中心那一格的描边改成小圆点，倍率默认 3 倍。
- 倍率不再写死：新增 `AppSettings::overlayZoom`（键 `settings/overlay/zoom`，夹 2~8，默认 3）+ `AppBackend::overlayZoom`（`Q_PROPERTY`，`NOTIFY prefsChanged`），QML 里 `magnifier.zoom = Math.max(2, backend.overlayZoom)`。
- 验证同倍率可改：注册表写 `zoom=6` 再截图，放大区明显变大（`docs/ui-shots-qml/mag_zoom6.png`），改回 3 即恢复（`mag_zoom3.png`）→ 证明不是硬编码。
- 设置界面还没做（截图/浮层的设置分区还没建），眼下要改只能写注册表或等设置页接上 `backend.overlayZoom`。
## 截屏浮层：二次调整八向手柄 + 整块搬家 + 待做清单（2026-11-07）

### 做了什么
1. 八个手柄（四角 + 四边中点）改成可拖，框定之后仍然可用：拖它改四条边，最小 12px，翻过对面时钉住对侧，最后夹在屏幕内；`applyResize(dx, dy)` 按 `resizeEdge` 走。
2. 框内长按拖动整块选区（`moveArea`，`anchors.fill` + `margins: 4`，`z` 压在八向手柄之下），`applyMove(dx, dy)` 只在屏幕范围内平移，宽高不变。
3. 「翻译」按钮从工具栏中间挪到最右端（主操作收尾位置），次要操作在左。
4. 新增仓库根 `TODO.md`：九组「还没做成设置」的自定义接口清单（放大镜倍率/位置/开关、浮层外观、操作与热键、小窗、实时、记录、引擎网络、资产 OCR、Linux 预留），每项标 `已实现 / 待接线 / 未开始`。
5. `--ui-shot` 新增 `STUDIOUS_UI_RESIZE="e,120,0"` 与 `STUDIOUS_UI_MOVE="80,40"` 两个钩子：直接驱动 QML 的 `applyResize` / `applyMove`，用来验几何算法（自动截图里没法真拖鼠标）。

### 坑（重要）
- **`Repeater { model: [ {key:…}, … ] }` + `delegate` 里的 `required property var modelData` 在这版 Qt 上取不到值**，八个手柄一个都不显示，且**没有任何 QML 警告**。改成 `model: 8` + `required property int index` + 页面级 `readonly property var handleSpecs` 索引取值后立刻正常。教训：数组当 model 时宁可走 index，别指望 modelData；控件"没显示"先怀疑 delegate 求值失败，而不是 visible 条件。
- 手柄的拖拽位移必须用 `mapToItem(overlay.contentItem, mouse.x, mouse.y)` 算绝对位置：手柄自己会跟着框移动，用相对位移会自我循环。
- 整块搬家必须用按下那一刻的 `moveRect` 当基准，不能每次拿 `liveRect`（否则每次 move 都以最新值为基准，位移被叠加）。

### 验证
- `STUDIOUS_UI_RESIZE="e,120,0"` → 尺寸标签 `880 × 220`（原 760 × 220）。
- `STUDIOUS_UI_MOVE="60,50"` → 标签仍是 `760 × 220`、整框右移下移。
- 截图：`docs/ui-shots-qml/hs_fix.png`（手柄可见）、`hs_resize.png`、`hs_move.png`、`hz_se.png`（角落放大看手柄）。
- `--smoke … --engine local-mt` exit 0；`backend_selftest.exe` → `ALL CORE TESTS PASSED`；`update_translations` 373 条 0 new。
## OCR 实装 + 设置页「截图」分区（2026-11-07）

### OCR 三处真 bug
1. **`QFile::link()` 在 Windows 造出 .lnk**：模型 staging 里用 `QFile::link` 想省空间，结果 `models/ocr/det.onnx` 只有 1243 B、头是 `4c 00 00 00`（Shell Link），onnxruntime 报 `Protobuf parsing failed`。改 `QFile::copy` + 「已存在但尺寸不同先删」。
2. **置信度恒 0**：PP-OCRv4 的 rec 输出**已经 softmax 过**，`softmaxAt()` 再算一次得 ≈1/6625。修法：按第一行行和判 `rowIsProbability`，概率行直接取 argmax 的值。
3. **低置信噪声**：图标/边框常被识别成 0.3 左右的碎片，`ScreenFlow` 里加 `kMinConfidence = 0.5` 过滤 + 空串丢弃。

### 端到端证据
`STUDIOUS_UI_TARGET=screenFlow STUDIOUS_UI_CALL=begin STUDIOUS_UI_RECT="900,300,700,180" STUDIOUS_UI_SHOT=overlay` → `confirmSelection done: phase=Showing words=3 blocks=3 status=本地 MT · 2205 ms`，截图 `docs/ui-shots-qml/ocr_e2e.png` 里贴纸压在原文行上。抓屏 → OCR → 翻译 → 贴纸全链路通。

### 模型来源（写死在 manifest，不随仓库分发）
det 4,745,517 B / rec 10,857,958 B（HF `SWHL/RapidOCR` 的 `PP-OCRv4/*_infer.onnx`）+ dict 26,250 B（PaddleOCR `v2.10.0/ppocr/utils/ppocr_keys_v1.txt`，钉版本防漂移）。`archive: ""` 表示直链单文件。

### 设置页「截图」分区
`sections` 插到「数据」与「关于」之间 → 8 个分区（截图 = 索引 6）；卡片 `SectionCard { title: "截图翻译" }` 里 4 个 `AppToggle`（选完直接翻译 / 翻译完自动复制 / 复制后自动关闭 / 保留原文标注）+ 3 个 `NumStepper`（不透明度 40–100 步 4、字号 10–20、放大镜 2–8）。`NumStepper` 是新加的内联组件（`AppButton −` / mono 值 / `AppButton +`），不用 SpinBox 是因为 Basic 样式的指示器跟主题打架。

坑：`AppToggle` **没有 `text` 属性**，写了就 `Cannot assign to non-existent property "text"` → 说明文字改由 `FieldRow` 的标签承担。

### `captureAutoHide` 真正接上
以前只落盘。现在 `ScreenFlow` 自动复制成功且开着该开关时，`QTimer::singleShot(0, …)` 收掉浮层（`copyResult()` 里手动复制那条路径早就有同样逻辑）。

### 验证
- 构建 `[9/9]`；`--ui-shot --ui-page 3 --ui-section 6` 截图 `docs/ui-shots-qml/set_capture.png`（8 个导航项、截图高亮、四开关 + 三组步进）。
- 注册表回灌：`overlay/zoom=5` + `capture/autoRunOnSelect=1` + `capture/autoCopy=1` → 截图显示 5× 与两个开关亮起（`set_capture_state_crop.png`），改回 3 / 0 / 0。
- `studious_en.ts` 386 条、unfinished = 0；`--smoke … --engine local-mt` exit 0；`backend_selftest.exe` → `ALL CORE TESTS PASSED`。
## 浮层设置页 + 翻译历史框架（2026-11-07）

**做了什么**
- 导航栏在「快捷键」与「配置」之间加第 6 项「浮层」（新图标 `resources/icons/overlay.svg`，白描边给 MultiEffect 上色）。页序变成：翻译 0 / 快捷键 1 / 浮层 2 / 配置 3 / 设置 4 / 外观 5；`Main.qml` 里两处写死的 `currentPage = 3` 跟着改成 4，`TranslatePage.onNavigateRequested` 里 `index >= 2 ? index + 1 : index` 平移一位（TranslatePage 那句 `navigateRequested(2)` 是上一版写死的配置页下标，这次不动那个文件）。
- 新页 `frontend/qml/pages/OverlayPage.qml`（280 行）三张卡：放大镜（开关 / 四角分段 / 倍率 2–8）、遮罩（压暗 20–80）、操作（方向键微调开关 / 复制后延迟关闭 0–10 秒）。
- C++ 新增 6 个设置项：`overlayMagnifier`、`magnifierCorner`（四值白名单）、`overlayDim`(20–80)、`overlayArrowNudge`、`overlayAutoHideDelay`(0–10)、`historyEnabled` + `historyDir`（只读解析）。`AppBackend` 全部走「比对 → 落盘 → `prefsChanged`」。
- `CaptureOverlay`：压暗改用 `backend.overlayDim / 100`；放大镜按 `magnifierCorner` 拆成 x/y 两轴各自翻折（贴边只翻越界那轴，不会跳到对角）；方向键微调走 `Keys.onPressed` 内的门控函数（不用 `Keys.enabled`，否则同一 Item 的 Esc/Enter 会被一起关掉），1px / Shift 10px，只在框定未翻译时生效；进入 Selecting 时补一次 `requestActivate()`（原来只在 `Component.onCompleted` 抢焦点，第二次热键进来方向键会哑）。
- `ScreenFlow` 两处「复制后自动关闭」都改成按 `overlayAutoHideDelay` 秒数 `QTimer::singleShot`（原来是写死 0）。
- 翻译历史：设置 → 缓存 → 「历史对话」小节（开关 + 目录只读展示 + 打开目录 + 「待做」徽章 + 三行说明）。`AppBackend::historyDir()` 空串时解析成 `<资产根>/history`，`openHistoryDir()` 先 `mkpath` 再交给 `QDesktopServices`——目录归应用所有，不该弹「路径不存在」。
- `TODO.md`：第 1/2/3 节里放大镜位置、放大镜开关、遮罩压暗、方向键微调、复制后延迟关闭全部落成「已完成」；新增第 11 节「翻译历史（框架已立，功能未做）」，把写入 JSON、浏览列表、容量剪枝三条列清楚。
- `studious_en.ts`：lupdate 422 条（35 新增），全部补英文，unfinished = 0。

**验证**
- 构建 `[5/5] Linking CXX executable studious.exe`；`--smoke … --engine llm-cloud` 1366 ms 出「你好，世界！」。
- `--ui-page 2` 中文/英文各一张截图，OCR 复核到「放大镜 / 遮罩 / 操作」三张卡、四角分段（右下/左下/右上/左上）、`3x`、`45%`、`Shift 10px`；英文版 rail 显示 Translate / Shortcuts / Overlay / Configuration / Settings，`Overlay` 在 88px 栏内不溢出。
- 真机抓屏（记事本）验证压暗可调：`dim=70` 时选区外亮度 3.0~4.1（`dim=45` 时同点是 7.9~10.8），选区内仍 221~245。
- 历史小节截图：`保存翻译历史` 开关、目录 `C:/Users/hua/AppData/Roaming/studious/assets/history`、`打开目录`、「待做」徽章都在；把注册表 `history/enabled` 置 1 后重启，开关显示为开。
- 测试后注册表复位：`overlay/dim=45`、`magnifierCorner=bottomRight`、`magnifier=1`、`history/enabled=0`、`ui/language=zh`。

**坑**
- `components/` 里**没有** `NumStepper`（那是 `SettingsPage.qml` 的页内 component），`ThemedSpin` 才是 ConfigPage 的页内组件；两者 API 不同（`stepSize/onValueModified` vs `step/unit/modified(int)`）。新页里内联了一份 NumStepper，点击只发 `modified()`、不自己赋 `value`，否则会打断 `value: backend.x` 的绑定。
- `--ui-page N` 的语义随页序变动（`2` 现在是浮层，设置 3→4、外观 4→5），旧截图脚本写死数字的会拍到别的页。
- PowerShell 把 `stderr` 重定向成文件会写成 **UTF-16LE**，用 `Get-Content` 读回来再打印仍然是乱码；要 `[System.Text.Encoding]::Unicode` 读、且别指望控制台能显示中文。

## 翻译取消（2026-11-07）

**起因**：`AppBackend::cancel()` 是假的——只把 `m_running` 置 false 加一句状态文案，注释直接写着"TranslateJob 没有 cancel 接口"；`QuickTranslate::cancel()` 同样只是逻辑取消。界面上没有一处能真正停下翻译。

**后端**
- 四个引擎的 `bool m_cancelRequested` 全改成 `std::atomic_bool`（UI 线程置位、工作线程读，必须原子）。
- `TranslatorRouter::cancel()`：逐个转发给注册的引擎。
- **LLM 补齐三处检查点**（这是"点了取消还要等两分钟"的真凶）：
  1. 批量 `while (cursor < pending.size())` 顶部（原本就有，但失败后走的是别的路）；
  2. 批量失败后的**逐段补救**循环 —— 每段最多 5 次换 prompt 重试，13 段就是 65 次完整请求，这条路上原本一次都不查取消；
  3. `postChat()` 的 5 次尝试循环顶部。
  三处统一用 `cancelled` 标志跳出，最后返回「llm 翻译已取消」。

**前端**
- `TranslateJob` 重写：每次 `start()` 建一个 `TranslateCancelState{std::mutex, router 指针, atomic requested}` 和一个独立的 `QFutureWatcher`（旧的还在收尾时新的可以立刻开始）；`cancel()` 置标志 + `++m_generation`（迟到的结果回来时代号不符，直接丢，不会把"已取消"覆盖成"成功"）；另起 150 ms 定时器反复置位，因为各引擎每次 `translate` 入口都会清自己的标志。
- `QuickTranslate::cancel()`：`m_job->cancel()` + 立刻 `m_busy = false`（不等作业收尾就能重新按）。
- `AppBackend::cancel()`：真转 `m_job->cancel()`，状态写「已取消」。
- `ScreenFlow::cancelTranslation()`：浮层专用，保住选区与已经贴出来的译文块，`++m_shotToken` 作废在途 OCR，phase 回 `Selecting`；`m_cancelling` 门闩吞掉取消时同步发出的 `finished`（否则会弹错误框）。
- QML：浮层忙碌框加「取消 Esc」，Esc 改成三档（收菜单 / 取消翻译 / 关浮层）；翻译页状态行右侧加「取消」按钮（`visible: backend.running`）。

**验证**
- 取消：`phase=Translating words=13` → `cancelTranslation()` → 手上那次请求 1.7 s 后返回 → `[router] done llm ok=0 err=llm 翻译已取消` → 进程 8.9 s 内退出（修之前同一场景 213 s 才退）。
- 不取消：15 块全流程 `phase=Showing`，`12925 ms`，`batch ok=1`。
- `--smoke` 命中缓存 0.1 s；`backend_selftest.exe` → `ALL CORE TESTS PASSED`；`studious_en.ts` 425 条 / 0 unfinished。

**坑**
- 排查时在 `TranslateJob`/`TranslatorRouter`/`LlmTranslator` 里插了临时 `qInfo`（统一带 `TEMP-DIAG` 后缀），定位完按整行删掉，别留。
- `stderr` 重定向到文件后**行缓冲不可靠**：进程还活着时可能只看到前半段日志，别据此判断"卡住不动"。
- `--ui-shot` 的 `RECT_WAIT` 是"确认选区后还等多久"，把它调大再去量"总耗时"会把自己的等待算成程序卡死。
- 截图取证要先把记事本 `ShowWindow(SW_RESTORE)` 再 `SetWindowPos(HWND_TOPMOST)`；被最小化时 topmost 不管用，OCR 会得到 `words=0`，看起来像"OCR 坏了"。
- 隐藏主窗口后 `grabWindow()` 抓不到浮层（枚举里 `visible=0`，抓到的是主窗口），这种组合下用真实屏幕 BitBlt 又是一片黑（浮层是分层窗口）。要看浮层就去掉 `STUDIOUS_UI_HIDE_MAIN`。

**遗留**
- 在飞的那一次 HTTP 请求仍不可打断（同步 `HttpClient::send`），取消后最多再等一个请求的时长。
- Web / 本地 MT 只在段落循环顶部查一次取消，各自的重试/decode 没查。
- 翻译页至今没有"开始翻译"的入口（`backend.translate()` 没有任何 QML 调用点），所以主窗口里的「取消」平时不可见。

## 浮层工具栏「设置」→ 独立设置窗口（2026-11-07）

**用户要求**：截图翻译工具栏加齿轮；点它打开一个**独立的**小窗（模板的二级设置），按功能分三节
（翻译小窗口 / 截图翻译 / 实时翻译）；截图翻译节里要有「选完直接翻译」。补充澄清：点齿轮时
如果正在框选，要**先收起框选**再弹窗。

**做法**
- `frontend/qml/overlay/CaptureSettingsPanel.qml`：`Window` + `Qt.Dialog | FramelessWindowHint |
  WindowStaysOnTopHint | NoDropShadowWindowHint`、`transientParent: null`、透明底，圆角与描边由内部
  `card` 画（328 宽，高度由内容撑）。`openAt(screenX, screenY, anchorWidth)`：右边缘对齐锚点、
  向下展开、下面放不下翻到上面、最后按 `Screen.desktopAvailableWidth/Height` 夹边；`openAt` 里才写
  `x/y`（用函数而不是绑定，因为要按"点下去那一刻"的锚点摆）。
- 三节：翻译小窗口（两行 + `Badge`「待做」，小窗界面还没做）／截图翻译（真控件：选完直接翻译、
  保留原文标注、自动复制译文、译文不透明度、译文字号、复制后延迟关闭）／实时翻译（两行 + 「待做」）。
- `frontend/qml/overlay/CaptureOverlay.qml`：齿轮 `onClicked` 只发
  `settingsRequested(screenX, screenY, anchorWidth)`（屏幕坐标 = `overlay.x + ovbar.x + barRow.x + gearSlot.x`，
  不许用 `mapToItem`），删掉原来的窗内 `settingsOpen` 面板与它带来的 Esc 第四档、点空白收起、phase 清理。
- `frontend/qml/Main.qml`：`CaptureSettingsPanel { id: captureSettings }` 挂在根窗口下；
  `captureOverlay.onSettingsRequested` → 先 `screenFlow.cancel()`（收起框选）→ `captureSettings.openAt(...)`。

**为什么不能画在浮层里**：浮层是 `WS_EX_NOACTIVATE` + `WDA_EXCLUDEFROMCAPTURE` 的全屏分层窗口 ——
画在里面的东西会跟着选区被搬走、关掉浮层一起消失、`CopyFromScreen` 也拍不到。第一版就是窗内面板，
真机整屏截图里浮层与小窗**都**不见了，才改成独立窗口。

**验证**
- `STUDIOUS_UI_EMIT="gearSlot:clicked:x"` + `STUDIOUS_UI_SHOT="captureSettingsPanel"` →
  `[ui-shot] after emit: phase=Idle`（框选确实先收了）+ `saved -> capture_settings_win.png (492x846 @1.5x, logical 328x564)`。
- 真机整屏 `CopyFromScreen` → `docs/ui-shots-qml/screen_with_panel.png`：小窗浮在浏览器之上、位置贴着
  原来齿轮那一带；浮层已退出（分层窗口本来就不进截图）。
- 第一版给的是 `Qt.Window` 不置顶 → 真机被浏览器盖住，看起来像"点了没反应"；而且 `Start-Process
  -WindowStyle Hidden` 起的进程抢不到前台，`requestActivate()` 无效。加 `WindowStaysOnTopHint` + `raise()`
  之后才稳定可见。

**坑**
- `STUDIOUS_UI_HIDE_MAIN=1` 原来用 `main->hide()`，而 `CaptureOverlay` 是声明在 `Main.qml` 里的 Window、
  会被 Qt 当成主窗口的 transient 子窗口 —— 父窗口一 hide 浮层一起不可见，`grabWindow()` 就回退抓主窗口
  （1680x1080 而不是 2561x1601）。改成 `setPosition(-20000, -20000)`。
- `STUDIOUS_UI_SHOT` 现在认任意 `objectName`（`overlay` 是老名字，映射到 `captureOverlay`），可以单独抓小窗。

### 追加：去掉两节占位、加「更多设置」、标题可拖动（2026-11-07）

**用户要求**：把小窗里的「翻译小窗口」「实时翻译」两节去掉（等对应功能做了再放回设置里）；
加一个「更多设置」用来打开主界面的「浮层」页；小窗目前不能拖动，补上。

**改动**
- `frontend/qml/overlay/CaptureSettingsPanel.qml`：删掉两节占位（`小窗不透明度`/`小窗字号` 两行 +
  `实时翻译` 的 `刷新间隔`/`只在内容变化时翻译` + 两句占位说明 + 两条分隔线），现在只剩「截图翻译」一节。
  新增 `signal moreSettingsRequested()` 与页脚整行按钮「更多设置 ›」（`surfaceLow` 底 + `outline` 边，
  悬浮 `surfaceHighest`）。标题行改成「标题 + 拖拽层」：`Item` 里放 `Text` 与 `MouseArea`
  （`Qt.SizeAllCursor`），`onPressed` 存 `grabX/grabY` 并调 `panel.startSystemMove()`，
  返回 false（拿不到系统拖拽）时在 `onPositionChanged` 里按 `x/y` 跟随 —— 不会出现"按住标题没反应"。
- `frontend/qml/Main.qml`：`captureSettings.onMoreSettingsRequested` → 收小窗 → `root.show()/raise()/
  requestActivate()` → `root.currentPage = 2`（浮层页）。

**为什么不手工只算坐标**：`startSystemMove()` 由系统接管（跨屏、贴边、不抖）；确认过
`QWindow::startSystemMove()` 在 `qwindow.h` 里、且 QML 侧在 `qml/QtQuick/plugins.qmltypes` 里有导出，
Qt 6.12 可用；手工跟随只作为兜底。

**验证**：构建通过（`[11/11] Linking`）；`--ui-shot`（输出到 `%TEMP%`）走「起浮层 → 框选 → 点齿轮」
全链路，stderr 零 QML 错误，`after emit: phase=Idle`（框选先收），小窗尺寸 328×328（两节删掉后变矮）。
`update_translations` → 436 条（1 new「更多设置」/ More settings，10 条 obsolete 保留），`unfinished = 0`。

### 追加：按钮改成「设置」文字、点设置不打断这一单、页脚样式收统一（2026-11-07）

- `CaptureOverlay.qml`：工具栏那个按钮的 `text` 从 `⚙` 改成 `qsTr("设置")`（英文界面是 Settings），`objectName` 仍是 `gearSlot`（旧的取证脚本还能用）。
- `Main.qml`：`onSettingsRequested` 里去掉 `screenFlow.cancel()` —— 点设置不再把框选扔掉；`onClosed` 补 `captureOverlay.requestActivate()`，把键盘还给浮层（浮层是 NOACTIVATE，不抢就没有方向键与 Esc）。
- `CaptureSettingsPanel.qml`：页脚「更多设置」从带边框的按钮改成跟上面各行同款的无边框行（强调色文字 + `›`，悬浮铺 `surfaceHighest`，圆角 rXs），左右缩进改 Theme.s1 与各行文字对齐。

**验证**：构建通过；`--ui-shot`（输出到 %TEMP%）走「起浮层 → 框选 → 点设置」，stderr 零 QML 错误，`after emit: phase=Selecting` —— 选区确实还留着（改之前是 Idle）。`studious_en.ts` 437 条、`unfinished = 0`。

### 追加：设置小窗改成浮层的子窗口（2026-11-07）

用户要求：不要独立窗口，改成子窗口。

- `CaptureSettingsPanel.qml`：`flags` 从 `Qt.Dialog | Frameless | StaysOnTop | NoDropShadow` 改成
  `Qt.Tool | Frameless | NoDropShadow`，删掉 `transientParent: null`；`openAt()` 只算 `x/y`，
  **不再自己 `visible = true` / `raise()`** —— 显示与否统一由宿主的 `settingsOpen` 管。
- `CaptureOverlay.qml`：拿回 `property bool settingsOpen` 与 `signal moreSettingsRequested()`；
  「设置」按钮改成开关（`settingsOpen = !settingsOpen`，打开时调 `settingsPanel.openAt(...)`）；
  文件末尾实例化 `CaptureSettingsPanel { visible: overlay.settingsOpen && overlay.engaged;
  onClosed: overlay.settingsOpen = false; onMoreSettingsRequested: { 收起自己 → overlay.moreSettingsRequested() } }`；
  Esc 恢复四档、点空白处也收起小窗、phase 回 Idle 一起清。
- `Main.qml`：删掉根窗口下的 `CaptureSettingsPanel` 实例与 `onSettingsRequested`（子窗口自己摆位置），
  只保留 `captureOverlay.onMoreSettingsRequested` → `screenFlow.cancel()` + 主窗口 `show/raise` + `currentPage = 2`。

**为什么归属窗口不需要 StaysOnTop**：被 owner 拥有的窗口在 Windows 上恒在 owner 之上，浮层自己置顶，
小窗自然跟着在它上面；再加置顶反而会盖住系统弹窗、且关浮层时容易留下一个孤儿置顶窗口。

**验证**：构建通过；`--ui-shot`（输出到 %TEMP%）走「起浮层 → 框选 → 点设置」，stderr 零 QML 错误，
小窗 328×328 正常渲染、`after emit` 之后浮层仍是 `Selecting`（选区没被扔掉）。

### 追加：小窗落点让开工具栏（2026-11-07）

用户报「从下方出现会覆盖工具栏，从上方出现没问题」。原来落点是 `screenY + s2*2`（从「设置」按钮**上沿**往下 16px），
而按钮本身就在 44px 高的工具栏里 —— 小窗顶边还压在工具栏下沿以内。改成按「按钮下沿」算：

- `openAt(screenX, screenY, anchorWidth, anchorHeight)` 新增第四参（`CaptureOverlay` 传 `gearSlot.height`，缺省 30）。
- 向下：`py = screenY + barH + gap`；下面放不下才翻到 `screenY - height - gap`；最后夹进 `Screen.desktopAvailableHeight`。
- 实测两次（1024 逻辑高、DPR 1.5）：选区 (300,300,600,200) → 工具栏在下，小窗 y=557（工具栏 508~552，留 5px）；
  选区 (300,760,600,200) → 工具栏在上面被挤到下方，小窗翻到 y=648（底边 976，工具栏 978 起），两向都不压工具栏。
- `[ui-shot] window` 那行补了 `at (x,y)`，以后量位置不用截图猜。

### 追加：引擎菜单加状态点、缺资产不可选（2026-11-07）

- `CaptureOverlay.qml` 新增 `engineStatusMap`（从 `backend.engineStatuses` 绑出来，不拷贝，状态一变圆点就跟着变）、
  `engineStateOf(code)`、`engineUsable(code)`、`engineDotColor(code)`；菜单行右边画 8px 圆点
  （绿 `Theme.good` / 黄 `Theme.caution` / 红 `Theme.danger`），不可用的行 `opacity: 0.45` 且
  `TapHandler.enabled: false`、光标恢复箭头。菜单宽 176 → 196 给圆点留位置。
- 「自动」那一行看全体：只要还有一个引擎不是缺资产就是绿的。
- 打开菜单前先 `backend.refreshEngineStatuses()`，避免用上一单的旧状态。
- 取证：本机四个引擎都是 ready → 5 个绿点（x 968..978，y 830/882/932/984/1034 物理，行距 32 逻辑）。
  临时把 local-mt 强制成 missing-assets 复测：第 5 个点渲成 `#CA888D` =
  `0.45 × #B3261E` 叠在 `#DFD8EA` 上，正好是「红点 + 45% 灰化行」，随后已还原。
### 翻译历史：自动存档 + 三分库 + 合并时间线（2026-11-07）

- 需求：历史开启时不点按钮也自动存；小窗 / 截屏 / 实时分三个库；展示时三库按时间合并；实时翻译「开启到关闭」算一组写一条。
- C++：`AppBackend` 撤掉单库成员，改 `m_recordsCapture/m_recordsMini/m_recordsLive`；
  `recordsFor(kind)` 懒建 `%APPDATA%\studious\records\{capture,mini,live}.db`；
  `persistRecord()` 先读 `settings/history/enabled`，关着直接不写（存档全是自动的，开关是唯一总闸）；
  `historyRecords(limit)` 三库各取满 limit 再合并按 `createdAt` 倒序切前 limit。
- 实时分组：`beginLiveSession()` / `appendLiveSession(blocks)` / `finishLiveSession()`，
  append 时与上一条 `original` 相同就跳过（实时刷新会重复送同一行），finish 才落一条。
- 截屏自动存档挂在 `ScreenFlow` 翻译成功回调里（`blocksChanged` 之后、`finished` 之前，
  因为 `saveToRecords` 要读 `m_blocks`），工具栏的「保存」按钮删掉。
- 设置·缓存 → 历史对话：`最近 %1 条` + 刷新 + 表头（时间/来源/摘要）+ 50 行列表；
  行间距自管（外层 ColumnLayout 的 spacing 会把行撑得很散），行底 1px `outlineVariant` 0.5 分隔线。
- 坑：替换说明文字时从 `FieldRow` 起替换，漏掉它上面那条旧 `Text`，界面上会连着出现两段说明 —— 已删旧的。
- 取证：history=1 → 记事本跑一屏 → `capture.db` 落地 1 条；history=0 重跑 → 条数与时间戳不变（开关卡住）。
  另用 python `sqlite3` 往 mini/live 各塞 2/1 条模拟记录，列表合并显示「最近 4 条」并按时间倒序，
  来源列正确区分「实时翻译 / 小窗翻译 / 截屏翻译」（`docs/ui-shots-qml/settings_history_multi.png`）。测试数据已清空、`history/enabled` 复位 0。
- 遗留：`saveMiniRecord` 与三个 live 接口尚无调用点（B/C 页未做），未做运行期验证；`%APPDATA%\studious\records.db` 单库遗留文件待迁移时清理。
## 实时翻译（C 页）主链路打通（2026-11-07）

**做了什么**
- 新建 `frontend/src/LiveSession.{h,cpp}`（约 780 行）与 `frontend/src/OcrLines.{h,cpp}`：单 QTimer 轮询「抓选区 → OCR → 翻译 → 每行贴回原位」。一帧没回来就整帧丢弃，绝不排队（排队会让译文越追越旧）。行用 `\n\n` 拼给 `QuickTranslate` 再按空行切回，段与行 1:1。
- `frontend/qml/overlay/LiveOverlay.qml`（透明、点击穿透、排除截图）与 `LiveControlBar.qml`（运行 / 暂停 / 出错三态、可拖动、Esc 停止）。
- `CaptureOverlay.purpose = "live"`：同一个框选浮层复用，工具栏只留「开始实时翻译 / 取消」。
- `AppSettings` / `AppBackend` 新增 `liveIntervalMs` / `liveOnlyOnChange` / `liveEngineLocked`；「浮层」页可改刷新间隔。
- 收单时 `sessionFinished` → `AppBackend::saveLiveRecord()` 写 `live.db`，一次会话一条（重复行会跳过）。
- `--ui-shot` 钩子扩展：`STUDIOUS_UI_TARGET=liveSession` 时 `STUDIOUS_UI_RECT` 走 `start(QRect)`，`STUDIOUS_UI_CALL` 也认 `liveSession`（这样才能 `stop()`）。

**踩的坑**
- `LiveOverlay` 根节点写死 `visible: false`，而 C++ 的 `placeWindows()` 只摆位置不管显隐 → 浮层永远不出现，截图里 `liveOverlay visible=0`。改成 `visible: liveSession.phase !== "Idle"` 才对（暂停/出错要留在屏上）。
- `LiveSession.h` 声明 `void placeWindows()` 而定义返回 `bool` → `void value not ignored`；声明跟着改成 `bool`。
- 分层窗口（`WDA_EXCLUDEFROMCAPTURE`）本身就拍不到，`Graphics.CopyFromScreen` 里没有译文，验证只能「先抓屏、再把浮层 grab 叠回去」。

**验证**
- 记事本 5 行英文，逻辑选区 `546,420,560,280` → `phase=Running blocks=6 updates=3`；贴纸窗口 `560x280 at (546,420)`，pixel 统计 plate=108686 / ink=9798（8.27%），是真中文不是空框。
- 叠图 `docs/ui-shots-qml/live_composite.png`；控制条 `619x50`（`live_bar.png`）。
- 优雅停止后 `live.db` 恰好 1 条：`engine=云端 LLM · deepseek-v4.1-flash`、原文 76 字符 / 译文 202 字符。
- `studious_en.ts` 491 条、`unfinished = 0`。

**已知取舍**
- `ocrModelsDir()` 在 `LiveSession.cpp` 里抄了一份（共用要动 `ScreenFlow.h`），将来合并到 `OcrLines` 旁边。
- `liveEngineLocked` 只是默认值，没有在 `LiveSession` 里强制（换引擎仍走 `QuickTranslate` 的自动选路）。
## 小窗翻译（B 页）主链路打通（2026-11-08）

**做了什么**
- 读 `docs/TempUI/flows.html` 的 B 页（25676 字节）：`.miniwin` = `min(560px, 93%)` 宽、`max-height 558`、圆角 20；8 个状态 B1–B8，共用外壳：顶栏「翻译 / 记录」两个页签 + 设置 + 置顶，语言行（源语言 / 目标语言带「多选」）、输入区（`手动输入` / `划词获取` 标签）、`N 字 · M 段`、操作行（清空 / 翻译 / 取消 / 保存到记录）、结果区（目标语言标签页 + 引擎名 + 逐段编号 + 单段复制 + 全部复制）。
- 新增 `frontend/src/MiniSession.{h,cpp}`：持一个独立 `QuickTranslate`，状态机 `Empty / Editing / Translating / Done / Failed`；`srcLang` 空串 = 自动；`dstLangs` 多选；`blocks` 按当前 `activeDst` 切片（带 `passthrough` 标记）；`copyAll / copyBlock / saveRecord / grabClipboard / clearAll` 全是 `Q_INVOKABLE`。
- 新增 `frontend/qml/overlay/MiniWindow.qml`（560×558、圆角 20、无边框可拖、`transientParent: null`）：翻译页 + 记录页 + 窗内语言菜单（不用 `Popup`，理由同实时控制条）+ 设置小窗（`CaptureSettingsPanel { mode: "mini" }`）。
- `AppBackend` 加 `miniAlwaysOnTop`（读写 `settings/mini/alwaysOnTop`）；窗口 `flags` 直接绑它，标题栏「置顶」按钮与设置小窗都能切。
- `CaptureSettingsPanel` 拆出第三段 `mode="mini"`：固定在最前 + 译文带原文（`backend.showSource`），截图组改成只在 `mode="capture"` 显示。
- `frontend/CMakeLists.txt` 加 `MiniSession.{h,cpp}` 与 `qml/overlay/MiniWindow.qml`；`main.cpp` 注入 `miniSession` 上下文属性，`--ui-shot` 的 `STUDIOUS_UI_TARGET` 也认它。
- `Main.qml` 实例化 `MiniWindow { transientParent: null }`，`launch("mini")` 改成 `show/raise/requestActivate`；入口卡的脚注从「接口已预留」改成「点一下打开小窗」「点一下开始框选」。
- 保存走 `saveMiniRecord` → `mini.db`；历史没开时按钮下方直接回一句「没存进去：历史对话是关着的…」，不装作保存成功。

**踩的坑**
- `backend.languages` 每项是 `{code, label}`，不是 `{code, name}` —— 我按 `name` 写，QML 报了一屏 `Unable to assign [undefined] to QString`（`langName()` 与菜单项都中招）。
- QML 函数的形参在元对象里是 `QVariant`，`QMetaObject::invokeMethod(..., Q_ARG(QString, ...))` 匹配不上，返回 false 且静默；取证钩子补了零参的 `showTranslateTab() / showRecordsTab()`。
- 占位文本被输入框左上角的「手动输入」标签压住：`anchors.topMargin` 从 26 提到 `26 + Theme.s2` 并加左右内边距。
- `mini.db` 时间戳不动时先别怀疑代码：`persistRecord()` 会读 `settings/history/enabled`，关着就直接 `return false`。

**验证**
- 空态 `docs/ui-shots-qml/mini_empty.png`、结果态 `mini_result2.png`（三段原文+译文、`本地 MT`、`6 ms · 3 段`）、保存态 `mini_saved.png`、记录页签 `mini_records.png`（`最近 2 条`，小窗 + 实时两条按时间倒序）。
- `mini.db` 新增一条 `source=mini`（原文 2 段、译文 2 段）；`studious_en.ts` 541 条 / unfinished = 0；构建 `[5/5] Linking`。
- 热键链路已通：`main.cpp` 注册 `mini` → `screenFlow->launchRequested("mini")` → `Main.launch("mini")` → `miniWindow.show()`。

---

## 代码审计与六路修复（2026-11-08）

**起因**：用户「测试,查询现在代码漏洞,开子代里」。先起 5 个只读审计子代理（后端翻译链 / 运行网络 MT OCR / 前端 C++ / QML 平台 / 安全面）出清单，用 CLI 实测交叉验证，再按用户「可以,修复吧」分 6 路并行修（文件范围互斥，禁构建、禁用 PowerShell 写源码），主代理负责 `AppBackend`、`QtHttpClient`、`LiveSession` 与统一构建验证。

**实测出来的问题（修前）**
- 缓存污染：`--smoke <"a"×8000>` 第一次命中 0/1 输出原文照抄，第二次 `命中=1/1`、7 ms 返回那串 `a` —— 译文等于原文也被写进缓存。
- 重复文本被静默截断：`"hello world. "×62`（806 字）只剩「你好，世界。你好，世界。」
- `--from xx` 不校验、未知参数 `--whatever` 静默忽略。

**修了什么（按文件）**
- `backend/src/router/TranslatorRouter.cpp` + 四个引擎：新增 `ICancelControl`（`resetCancel()`），入口清标志收敛到 Router 一处；缓存三道闸（不 ok / 空译文 / 译文==原文且源串含字母都不写），tag 改用 `engine->cacheTag()`，命中值等于原文按未命中处理。
- `backend/src/translators/WebTranslator.cpp` + `MyMemoryAdapter.cpp`：`webLooksLikeServiceNotice()`（11 条 needle）与 `webLooksLikeSourceEcho()` 双闸，配额警告不再被当译文缓存；`m_activeSources` 让 `cacheTag()` 反映真正产出者。
- `backend/src/translators/LlmTranslator.cpp`：`translateBatch()` 四处查取消、`translateOne()` 加 `maxAttempts`（批量补救传 1，8 段最坏 40→9 次）、语种启发式重写（无字母/≤4 字符放行，ja 接受汉字，拉丁目标只要求无 CJK）。
- `backend/src/runtime/LlamaServer.cpp`：`parseExtraArguments()` 改 `QProcess::splitCommand()` 分词并拒受管 flag（`--host` 等），extra 拼最前；`maskedCommandLine()` 给 key 类参数打星；`stop()` 先 kill 再 `waitForFinished(3000)`。
- `backend/src/ocr/OcrEngine.cpp`：Session 加互斥与 `ready` 原子位，线程数不一致只警告不再重建会话（UAF 根因），`isAvailable()` 用 `try_to_lock`，rec 尺寸改用模型 `recShape`，det 加 4MP 上限。
- `backend/src/net/QtHttpClient.cpp`：`isLoopbackHost()` 改真解析（`127.attacker.tld` 不再被当回环）、32 MB 响应体上限、跨站跳回本机直接拒绝。
- `frontend/src/ScreenFlow.cpp`：`m_ocr` 改 `shared_ptr` + 自定义析构等 watcher、`m_recognizing` 防重入、失败不再双发、`cancel()` 先置 `m_cancelling`。
- `frontend/src/MiniSession.cpp`：请求代号（编辑后旧结果不再覆盖）、`activeDst` 回退首个语言。
- `frontend/src/AppBackend.cpp`：`m_inflightSrcLang/m_inflightDstLangs` 快照（迟到结果用发起时的语言拼装；截屏记录不再拿全局 settings 当语言）；失败分支清 `m_byLang` 并 `blocksChanged`。
- `frontend/src/LiveSession.cpp`：`m_engineSwitching` 让自己的 cancel 不刷红字。
- `frontend/src/platform/*`：`winApplyOverlayFlags()` 快照并回填 `WDA_EXCLUDEFROMCAPTURE`（切穿透不再把浮层从录屏里放出来）、`SWP_SHOWWINDOW` 去掉、`MOD_NOREPEAT`、托盘双击去重、`m_clickThrough` 默认 false 且随窗口销毁清理。
- `frontend/qml/overlay/*`：`LiveOverlay` 拖动/手柄改屏幕坐标算增量（原先是窗口内坐标回写窗口 x/y 的反馈环，拖起来只有半速且漂）、`MiniWindow` 的 `flags` 只依赖本地 pin 状态（不再每次绑定变化重建原生窗口）、`TextEdit` 改单向灌入避免绑定被首个按键打断。
- `backend/asset/*`：`isSafeAssetId()`/`isSafeRelativeEntryPath()`/`isPathInside()` 白名单（拒 ADS `:`、保留设备名、`..`）；`verifyInstalled()`；解包条目/体积上限、边写边算哈希、staging 随机后缀、失败清临时文件。

**验证**
- 构建 `[74/74] Linking`（先杀掉占着 exe 的进程）；`backend_selftest.exe` → `ALL CORE TESTS PASSED`。
- 缓存污染修复：同一个 8000 × `a` 连跑两次，`命中=0/1`、`命中=0/1` —— 不再把原文照抄那次写进缓存。
- `--assets-cli backend/asset/manifest.json` 正常，16 条（占位 1）、已安装 2.6 GB，说明 `computeStatus()` 的白名单没有误杀现有资产。
- 截图：主界面 `after_fix_main.png`、控制条手柄 `after_fix_handles.png`（八颗点贴内侧）、截屏翻译 `after_fix_capture_ok.png`（`phase=Showing words=7 blocks=7`，本地 MT 2984 ms）。

**还没做**
- 清单验签仍是 `requireSignature=false`（唯一信任根是 qrc 里的清单）；`LocalEngine` 启动前没调 `verifyInstalled()`；目录型资产无法整包验哈希。
- API Key 仍明文存注册表、内置默认值 `12345678`（要改得动加密与迁移，属独立一批）。
- `looksDegenerate()` 对合法重复文本仍会截断（806 字那例）。

---

## 引擎选择统一到一处（2026-11-08）

**起因**：用户问「实时翻译的引擎选择是独立的代码????为什么没有跟随主页的引擎选择????」，随后要求「1 给小窗翻译左下角也加一个切换引擎，使用和截图翻译、实时翻译同款；2 让他们的使用统一的值也就是主页面的那个」。

**口径**：`settings/engine/id`（`backend.engineId`）是唯一真值，四个入口（主页 / 截图翻译 / 实时翻译 / 小窗）都只读写它，不再有任何"本窗口覆盖"。

**实时翻译原来的两个坑**
1. `LiveControlBar.qml` 把空值显示成「自动」，看着像独立设置；一旦点过别的引擎就没有回到跟随的入口。
2. 跟随态下，「只在内容变化时翻译」会在主页换引擎后把那一帧整帧跳过 —— 新引擎要等画面变了才生效。

**改了什么**
- `frontend/qml/overlay/LiveControlBar.qml`：选项回到 `自动 / Web 在线 / 云端 LLM / 本地 LLM / 本地 MT`；`engineShortName()` 与菜单当前项都读 `backend.engineId`；`selectEngine()` 先 `liveSession.engineId = ""`（清掉会话覆盖）再 `backend.engineId = id`，运行中先暂停再恢复；按钮 `text` 里显式读一次 `backend.engineId`（JS 函数体里的属性读取不建依赖）。
- `frontend/src/LiveSession.cpp/.h`：新增 `m_lastSettingsEngineId`，每帧比对 `settings.engineId`；跟随态发现主页换引擎就清 `m_lastText`，强制用新引擎重翻一帧，不受「内容变化」闸影响。
- `frontend/qml/overlay/MiniWindow.qml`：左下角加引擎槽（`objectName: "miniEngineButton"`）+ 窗内上弹菜单（`miniEngineMenu`，五项带状态点，当前项高亮加粗），`selectEngine()` 直接写 `backend.engineId`；Esc / 点空白都会收菜单。

**踩的坑**
- 菜单定位别用 `mapToItem(card)`：窗口刚 show 出来时 `card` 还是 `0x0`、按钮还贴在行首，算出来的锚点是错的，而且布局后来动了绑定也不会重算（菜单会僵在窗口中间）。改成用窗口高度倒推：`y = card.height - height - 26 - s4 - 4`，`x = s4`。
- `--ui-shot` 的 `STUDIOUS_UI_CALL` 只认**零参**方法，取值参数得另加一个 `toggleXxx()` 包装。

**验证**
- 主页引擎设为「云端 LLM」→ 实时控制条显示 `跟随主界面（云端 LLM）`（`docs/ui-shots-qml/live_bar_follow.png`，此截图是上一版语义）。
- 统一后的版本：主页设为「本地 MT」→ 小窗左下角显示「本地 MT」（`mini_engine_sync.png`），菜单五项在按钮正上方弹出（`mini_engine_menu.png`）。
- `studious_en.ts` 545 条 / unfinished = 0；构建 `[9/9] Linking`、`[5/5] Linking`。
## 浮层「更多设置」跳转 + 截屏/实时加语言对 + 语言规则（2026-11-08）

**用户要求**：① 小窗和实时翻译的设置面板里「更多设置」没反应，要跳到「浮层」页；② 截屏翻译和实时翻译也要能选语言，样式极简 `[源] → [目标]`；③ 语言选择统一走一份值（以主页为准），并加规则：Web / 云端 LLM / 本地 LLM 支持多目标，切到本地 MT 就收成单一。

**改动**：
- `Main.qml`：新增 `function openFloatingSettings()`（show/raise/requestActivate + `currentPage = 2`）；`CaptureOverlay` / `LiveControlBar` / `MiniWindow` 的 `moreSettingsRequested` 都接到它。实时那一路**不暂停**会话（用户只是去看设置）。
- `MiniWindow.qml`：新增 `signal moreSettingsRequested()`，面板页脚转发出去；`toggleDst()` 在 `!backend.dstMultiSupported` 时改成单选替换；多选小标签按 `backend.dstMultiSupported` 显示「多选 / 单选」。
- `AppBackend`：新增 `Q_PROPERTY(bool dstMultiSupported)`（`engineId != "local-mt"`）；`setEngineId()` 切到 local-mt 时把 `m_dstLangs` 收成第一个并落盘；`setDstLangs()` 在 local-mt 下自身也截断；构造函数里补同样的启动期归一（注册表里可能留着 `zh,ja`）。
- `MiniSession`：`setSrcLang` / `setDstLangs` 现在会写 `settings/lang/src|dst`（四个窗口共用一份值）；`setDstLangs` 与 `translate()` 在引擎是 local-mt 时收成单选。
- `CaptureOverlay.qml`：工具栏加 `[源] → [目标]` 两个 `OverlayButton` + 窗内 `langMenu`（源含「自动」，目标不含）；Esc 从四档变五档（先收语言菜单）；点空白处连同语言菜单一起收。
- `LiveControlBar.qml`：`mainRow` 里加同样的语言对（`liveSrcLangButton` / `liveDstLangButton`）+ 窗内 `langMenu`；窗口高度把语言菜单也算进去；Esc 顺序为 语言菜单 → 引擎菜单 → 设置 → 停止。

**验证**：
- 截屏工具栏：`已选 600 × 200 | 英语 ▾ → 日语 ▾ | 云端 ▾ | 设置 | 重新框选 | 关闭 Esc | 翻译`（`docs/ui-shots-qml/cap_lang.png` / `cap_lang_crop.png`）。
- 实时控制条：`准备中 | 已更新 0 次 | 英语 ▾ → 日语 ▾ | 云端 LLM | 译文/原文 | 暂停 | 停止 | 锁定选区 | 设置 | ✕`（`live_lang.png`，窗口从 704 变 879 宽）。
- 设置跳转：`STUDIOUS_UI_EMIT=miniWindow:moreSettingsRequested:x` → 主窗口回到最前并切到「浮层」页（`settings_jump.png`）。
- 规则：注册表 `settings/lang/dst = zh,ja` + 引擎 `local-mt` 启动 → 落盘变 `zh`；小窗界面显示「单选」、目标只剩一项（`mini_single.png` / `rule_single.png`）。
- `studious_en.ts` 546 条 / unfinished = 0（新增 `单选 / Single`）。

**坑**：
- 语言的注册表键是 `settings/lang/src` 与 `settings/lang/dst`，不是 `ui/dstLang` —— 一开始读错键，以为归一没生效。
- `AppBackend::setEngineId` 是属性 WRITE 访问器、不是槽，`--ui-shot` 的 `invokeMethod` 调不到（返回 0），所以规则验证改成「先写注册表多选 + 启动看是否被收成单选」。