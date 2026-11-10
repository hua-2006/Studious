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
## 关于页第三方清单 + README 状态更新（2026-11-08）

### 起因
用户要求：① 更新「设置 · 关于」里的第三方组件表与「第三方声明」弹窗；② 更新 README —— 说明基础设施已完成，剩下的是测试打磨。

### 实际核对到的数据（不是凭印象填的）
| 组件 | 版本 | 许可证 | 依据 |
|---|---|---|---|
| onnxruntime | 1.31.1 | **MIT** | backend/third_party/onnxruntime/LICENSE 首行；onnxruntime.dll 文件版本 1.31.1.20261023.4.41d61 |
| sentencepiece | 0.2.0 | Apache-2.0 | VERSION.txt + LICENSE |
| monocypher | 4.0.2 | BSD-2-Clause（上游 BSD-2-Clause OR CC0-1.0） | monocypher/README.md |
| miniz | **11.0.2** | MIT | MZ_VERSION 宏 + LICENSE |
| Qt | 6.12.0 | LGPL-3.0（动态链接） | 运行时 qVersion() |
| 内置字体 | — | OFL-1.1 | font/ 下两套字体的许可证 |

- 旧表两处错：onnxruntime 写成 Apache-2.0、miniz 写成 3.0.2。
- sentencepiece 的 vendored 子库也写进声明了：absl（Apache-2.0）、darts-clone（BSD-3-Clause）、esaxx（MIT）、protobuf-lite（BSD-3-Clause）—— 逐个看过 third_party/sentencepiece/third_party/*/LICENSE。
- 声明里补了一条：推理与翻译模型（Qwen2.5 / OPUS-MT / PP-OCRv4）不属于本程序，按需下载。

### 改了什么
- frontend/qml/pages/SettingsPage.qml：第三方表 4 行 → 6 行（补 Qt 与内置字体），修 onnxruntime 许可证与 miniz 版本；表上方加注释「版本与许可证以仓库里的实际文件为准」；「第三方声明」正文重写（许可证纠正 + 子库 + 动态链接说明 + 模型边界 + 许可证文本位置）。
- README.md：状态行改成「基础设施已全部落地」；已完成表补小窗翻译 / 实时翻译两行；「尚未实现」表换成「下一步：测试与打磨」（端到端测试为主线，另列主界面翻译入口、翻译字号、快捷键页、外观页 6 处、Linux 抓屏、清单验签、API Key 存储）；界面章节五页 → 六页并补「浮层」页；路线图 M2/M5 改 ✅、新增 P0 进行中；使用章节补小窗与实时两小节，并写明三入口共用同一份引擎与语言设置。

### 验证
- 构建通过（[9/9]、[5/5] Linking）；update_translations 567 条，10 条新串补齐英文，unfinished = 0。
- 按用户要求这轮没有截图。

### 未做
- 改动未提交（README.md / SettingsPage.qml / studious_en.ts / studious_zh_CN.ts）。
## 死代码清理 + 运行时资产校验接线（2026-11-08）

用户审核后拍板「按你的计划来」。这一轮只动「有实现、零调用」的接口，外加把安装后复查接上。

### 删掉
- AppBackend::beginLiveSession / appendLiveSession / finishLiveSession 及成员 m_liveSessionActive / m_liveOriginals / m_liveTranslated：上一版「边跑边攒记录」的遗留，已被 saveLiveRecord 取代。它做的「同一行只留第一次」去重挪进了 saveLiveRecord，实时记录不再可能存出一堆重复行。
- TranslatorRecords::children() 与 dd() 的 parentKey 参数：父子记录模型（实时一父 N 子）从来没接线。parent 列保留（旧库里有），dd() 一律写空串，list() 仍兼容历史上写入过非空 parent 的行。
- AppSettings::recordSaveMode（settings/record/saveMode）：决定记录里存原文还是译文的开关，界面上没入口、代码里没读取，留着等于骗人。
- ScreenFlow::startRecognition() 声明（无定义、无调用）。
- platform::winClearOverlayFlags()（声明 + 实现）：窗口销毁时扩展样式本来就跟着没了。
- LocalEngine::backendLabel()：界面文案由 QML 处理，无人调用。

### 接上
- 新建 rontend/src/AssetPaths.{h,cpp}：资产根、清单路径、清单解析、**安装后复查**的唯一实现。AppBackend 里三份本地实现（indManifestPath / assetInstallRootPath / parseBundledManifest）改成薄壳，LocalEngine::installRoot() 也改为 paths::assetsRoot()（原来它自己又拼了一遍 %APPDATA%\studious\assets，迁移资产根后两边会各说各话）。
- LocalEngine::ensureStarted() 在起 llama-server 之前调 paths::verifyInstalled(runtimeAssetId, &err)：清单点名的关键文件必须齐全，运行时资产必须点名了原生可执行文件。失败直接拒绝启动并给「去资产页重新下载」的人话提示。

### 验证
- 正向：--smoke "Hello, world!" --local → 启动 2162 ms、译文「你好，世界！」、[DONE] exit 0；ackend_selftest = ALL CORE TESTS PASSED。
- 负向：把 ssets\runtime\runtime.llama.vulkan.win-x64\ggml-vulkan.dll 改名后再跑，得到
  [FAIL] 本地引擎运行时校验失败（runtime.llama.vulkan.win-x64）：资产 … 缺少清单要求的关键文件：ggml-vulkan.dll exit 1（改名前是照启不误）。测完已改名还原。
- 构建 [14/14] Linking；studious_en.ts 542 条 / unfinished = 0（新增文案走 i18n::sf 双语对，不需要进 .ts）。

### 仍未做
- 清单验签仍关闭（equireSignature=false / llowMissingSha256=true，AppBackend.cpp 与 main.cpp 三处）：运行时校验目前依赖「清单编译进 qrc」，不是密码学意义上的可信。
- API Key 仍明文落注册表、默认值仍是 12345678。
## 源语言「自动」编码统一为 auto（2026-11-08）

### 病因
「自动检测」有两种写法：MiniSession 在内存里用空串（srcLang == "auto" ? "" : code），
其余四个入口一律用 "auto"。这种漏转不崩溃、不报错，只会**静默翻错语言**
（"自动检测"被当成"没指定"）。同一个原因还让语言菜单里的「自动检测」项永远不高亮
（miniSession.srcLang === modelData.code 里左边是空串、右边是 "auto"）。

### 改法
- Languages.h/.cpp 增加 isAuto(code) 与 
ormalizeSource(code)：「自动」唯一编码 = "auto"；
  大小写不敏感；**不**在这里校验语言码是否注册 —— CLI 传没见过的码时应当由后端给出
  「不支持这个源语言」的明确错误，而不是被悄悄改成自动检测。
- 全部调用点改走它们：MiniSession（构造 / setSrcLang / eloadLanguagesFromSettings /
  	ranslate）、AppBackend（构造 / setSrcLang / eloadLanguagesFromSettings /
  detectLanguage 的 auto 判断 / persistRecord）、ScreenFlow（OCR 限定语言 + 发单）、
  LiveSession（同上两处）、QuickTranslate::translate、main.cpp（CLI --from 与同语言判断）。
- MiniSession::setDstLangs 里手写的「trim + 去重 + 空兜底 + 本地 MT 收成单选」换成
  langs::normalizeTargets(langs::split(langs::join(codes)), engineId)，四份实现归一份。

### 验证
- 构建 [14/14] Linking。
- --from AUTO（大小写混写）→ 头部 src=auto、命中缓存、译文「你好，世界！」exit 0
  （改前这种写法会被当成非法语言码）。
- --from xx 仍报错 exit 1：非法码不会被悄悄改成自动（保持原有诊断能力）。
- 注册表 settings/lang/src 写 ja → 小窗显示「日语」；写空串（旧版本遗留）→ 显示「自动检测」，
  说明历史数据不用迁移也能读对。
## 安全项第一批：安装来源 + 选择性校验、API Key 加密（2026-11-08）

口径（用户定）：**我们自己下载安装的资产要校验安全性；用户从文件导入的不校验。**

### 1. 资产来源（AssetOrigin）
- `AssetRegistry` 新增 `enum class AssetOrigin { Unknown, Download, Import }`、`markInstalled(id, origin, error)`（旧签名等价 Download）、`originOf(id)`。
- 来源落盘 `<资产根>/state/asset-origins.json`（QSaveFile 原子写）；读不到/写坏 → 全 Unknown，按 Download 处理。
- `verifyInstalled()`：单文件资产在「0 字节」检查之后，若来源是 Import 直接放行（不比对 sha256）；下载安装的仍逐字节比对。
- `Downloader::installFromFile()`：清单没有 sha256 不再拒绝导入，改为按 Import 安装并记来源；有哈希仍比一次，防选错文件。
- 界面：资产状态徽章对导入项显示「已安装 · 用户导入」；`refreshAssets()` 每条多带 `origin` / `verified` 字段。
- 新 CLI：`studious.exe --asset-verify <assetId>` → `[verify] <id> origin=… -> ok|FAIL`，退出码 0/1。

### 2. API Key 落盘加密
- 新建 `frontend/src/platform/SecretStore.{h,cpp}`：Windows 用 DPAPI（`CryptProtectData`，附加熵 `studious.settings.v1`，不做 LOCAL_MACHINE → 只当前用户能解）；其它平台退回 base64 并注明只防误读。
- `AppSettings`：读时 `unprotect`（老明文原样返回 → 升级不丢 Key），写时 `protect`；**删掉硬编码默认 Key `12345678`**（默认空）。
- 平台库补链 `crypt32`。
- 取证钩子新增 `STUDIOUS_UI_SET="prop=value;…"`（`QObject::setProperty`，因为很多设置的 WRITE 访问器不是 Q_INVOKABLE，invokeMethod 调不到）。

### 3. 清单验签
- `paths::loadManifest()` 现在把 `TrustKeys.h` 的公钥传进 `parseManifest`：**清单一旦带 sig 就必须验过**（不再受 requireSignature 开关影响）。
- 新增开关 `STUDIOUS_REQUIRE_SIGNED_MANIFEST=1`：要求必须带签名，发版/验收用；仓库里这份清单还没签名（发布流水线用 `manifest_tool sign` 生成），所以默认仍是"没签名就放行 + 打警告"。

### 验证
- `--asset-verify runtime.llama.vulkan.win-x64` → `origin=unknown -> ok`。
- 篡改 `models/ocr.det.ppocrv4`（追加 6 字节）→ `origin=unknown -> FAIL`（哈希不符）；写 `{"ocr.det.ppocrv4":"import"}` → `origin=import -> ok`；还原文件后回到 ok。
- Key 迁移：注册表写明文 `plain-test-key-123` → 触发一次保存 → 值变成 `dpapi:AQAA…`；再启动跑 `--smoke` 正常；把密文改成 `dpapi:AAAA` → 日志 `[secret] CryptUnprotectData 失败…按「未配置」处理`，进程不崩。
- `STUDIOUS_REQUIRE_SIGNED_MANIFEST=1` + `--asset-verify` → `[FAIL] 找不到或无法解析资产清单`（拒绝未签名清单）。
- 构建 `[5/5] Linking`；`studious_en.ts` 543 条 / 0 unfinished；`backend_selftest` → `ALL CORE TESTS PASSED`。

### 还没做
- 清单目前**未签名**：需要一对正式密钥，seed 必须离线保存（不进仓库），签名后的信封进 qrc 才算真开验签。
- 模型类资产（.q4km）目前不在启动前校验范围内（只校验运行时资产）；要校验得接受首次加载时算一遍 491 MB ~ 1.1 GB 的 sha256。
- 代理串里的凭据是否也走 SecretStore（现在明文）。
## 3B 模型入库（哈希）+ 7B 改为自行导入（2026-11-08）

### 3B
- 从 ModelScope 下载 `qwen2.5-3b-instruct-q4_k_m.gguf`，先验 GGUF magic（前 4 字节 `GGUF`），再算哈希：
  - 实际大小 `2,104,932,768` 字节（清单原先写的 `1,930,000,000` 是估算值，已改正）；
  - sha256 `626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d`。
- 装到 `<资产根>/models/model.qwen2.5-3b-instruct.q4km`，`state/asset-origins.json` 记 `"download"`（= 是我方下载安装，启动前要校验）。
- `--asset-verify model.qwen2.5-3b-instruct.q4km` → `origin=download -> ok`，耗时 11.4 s（2.1 GB 全量 sha256 的代价）。
- `--smoke "Hello, world!" --from en --to zh --local --local-model model.qwen2.5-3b-instruct.q4km` → 启动 2111 ms、翻译 170 ms、停止 153 ms，无残留 llama-server 进程。

### 7B
- 清单里删掉 `urls`（变成占位条目）→ 不再出现在可下载列表；note 改为「需自行导入」并写明去哪取、怎么装。
- UI：占位资产的按钮分两类 —— 运行时类仍「暂不支持」不可点；**模型类显示「导入文件」且可点**，点了直接开文件选择框，用的是清单里那条 id（导入走 Import 来源，不做哈希校验）。

### 踩坑
- 清单是**编译进 qrc** 的：改完 `backend/asset/manifest.json` 必须重建，否则 `--asset-verify` 读到的还是旧的内置副本，报「清单没有 … 的 sha256」。
- 资产根下的 `state/asset-origins.json` 手工维护时要保证 JSON 合法（QSaveFile 全量重写；读坏会全部回落 Unknown = 按下载来源校验）。
## B 组正确性修复 + 翻译字号实装（2026-11-08）

用户定调：「这只是一个开源的翻译工具软件，没必要做很高的安全性，我源码会直接公开上传的，先修复 b，把翻译字号实装。」
（A = 安全加固清单，B = 正确性/稳定性清单。）

### 1. 合法重复被误截断（LlmTranslator）
- 病因：`looksDegenerate()` 用「结尾 12 字在全文出现 ≥4 次」判复读、`truncateRepetition()` 从第二次出现处切断，
  806 字的 `"hello world. " × 62` 译文被截成两句。
- 修法：新增 `trailingRepeatUnit(text, minRepeats, maxUnit)` 只判「紧邻结尾的同一短单元是否连续重复」；
  `truncateRepetition(output, source)` 增加原文参数——原文本身长度 ≥ 译文一半时直接不截。
- 验证：805 字重复文本 `耗时 1910 ms`、`命中=0/1`，输出 62 个「你好，世界。」+「完成了。」，不再截断。

### 2. 本地 MT 长文本退化（LocalMtTranslator）
- 病因：整段塞进 OPUS-MT，8 段 587 字时解码器进入退化循环，`cleanupTranslation` 清成空串。
- 修法：`kMtChunkChars = 180` + `splitSentences()`（优先在句末标点后切，标点跟上一句）+ `targetWantsSpace()`
  决定拼接是否补空格；块之间也检查 `m_cancelRequested`。
- 验证：同一段八句英文 `耗时 5810 ms`、`命中=0/1`，八句全部译出，`[DONE] exit 0`。

### 3. 缓存并发与连接泄漏（TranslationCache）
- `hits_`/`misses_` 改 `std::atomic<qint64>`（worker 线程会并发累加）。
- 连接泄漏用 `thread_local ThreadConnectionCleanup`：线程退出时 close + `removeDatabase`，
  QThreadPool 反复换线程不再攒连接。

### 4. 本地引擎端口竞态（LlamaServer）
- `pickPort → launch → waitUntilReady` 包进 3 次重试；只有日志里出现 `address already in use` / `failed to bind`
  才重挑端口，模型损坏、显存不足这类不重试。

### 5. 翻译字号（readingFontSize）
- `AppSettings`：`settings/ui/readingFontSize`（11–24，默认 13），load 夹紧、save 落盘。
- `AppBackend`：`readingFontSize` Q_PROPERTY，写访问器 `setReadingFontSize` 夹紧并 `emit fontScaleChanged()`
  （与 `fontScale` 共用通知——QML 都当「字号变了要重排」）。
- `AppearancePage.qml`：「字号缩放」下面新增「翻译字号」滑块 + `N px` 读数。
- `MiniWindow.qml`：结果区正文 `font.pixelSize: max(11, round(readingFontSize * Theme.fontScale))`。
- `studious_en.ts`：545 条、unfinished = 0。

### 坑
- PowerShell 用 `\r\n` 拼接做替换时模式经常不命中且不报错（AppBackend.h 的 getter/setter 就是这么漏的），
  moc 才报 `no member named 'readingFontSize'`。结构性插入一律用 `edit` 工具精确定位。
- `MiniWindow.qml` 替换 `font.pixelSize` 时新行插在旧行之前，出现重复赋值，需手动删旧行。
- CLI `--smoke` 仍会打 `QSqlDatabase requires a QCoreApplication`（缓存统计早于 QCoreApplication），只告警。

### 验证
- 构建 `[60/60]`、`[5/5] Linking`；`backend_selftest` = ALL CORE TESTS PASSED。
- 截图 `docs/ui-shots-qml/mini_font20.png`（20 px 下的结果区正文）、`appearance_font.png`（含两条滑块）。
- 测试后把注册表 `readingFontSize` 复位 13。
## 快捷键页 + 划词热键（2026-11-08）

### 快捷键页
- 三个入口（小窗 / 截图 / 实时）各一张卡，观感照搬翻译页入口卡；点卡开始录制，Esc 放弃，
  键位框显示 `Ctrl+Alt+S` 这类文本，右上角 `已绑定` / `未生效` 徽章。
- 默认全空：`AppSettings` 里三个默认值从 `Ctrl+Alt+S/W/L` 改成空串，注册表旧值也清了 ——
  预置组合键很容易和用户机器上的软件撞车。
- 录制用一个隐形 `Item` 接 `Keys.onPressed`；**坑**：不可见 Item 拿不到 `activeFocus`，
  `visible` 必须跟着录制状态走，并用 `Qt.callLater` 再 `forceActiveFocus()`（visible 翻转在事件循环之后）。
- 改动即时生效：`main.cpp` 的 `applyHotkeys` 收到 `AppBackend::hotkeysChanged` 后
  先注销三个 id 再重新注册，结果写回 `backend.hotkeyStatus`。
- 重复检测（自己先查，系统只会说"被占用"）+ 占用来由（给 `IPlatform::registerHotkey`
  加了 `QString *error` 出参，`WinPlatform` / `NullPlatform` 同步实现）。

### 划词热键
- `AppSettings::hotkeySelection`（键 `settings/hotkey/selection`，默认空），快捷键页
  「场景快捷键」里一行「划词翻译」。
- `MiniSession::grabSelection()`：读剪贴板 →
  只有图片 → 提示「剪贴板里是图片，不粘贴。」不填；
  富文本（只有 HTML）→ `QTextDocument::toPlainText()`；
  ≤1500 字 → 填进输入框并立刻 `translate()`；
  超过 1500 字 → 只填不翻，提示「这段有 N 字，超过 1500 字就不自动翻 —— 看过原文再点「翻译」。」
- `Main.qml` 的 `launch("selection")`：开小窗 → `miniSession.grabSelection()`。
- 说明文字挂在 MiniWindow 输入框下方（`miniSession.noticeText`），平时不占位。

### 验证
- 短文本（59 字 2 段）：粘贴 + 自动翻译，命中缓存 4 ms（`selection_short_zh.png`）。
- 图片剪贴板：提示正确、输入框保持空（`selection_image.png`）。
- 长文本 3841 字：填入 + 只提示不翻，按钮仍是「翻译」（`selection_long.png`）。
- 快捷键页截图 `hotkeys_final.png`（空态）、`hotkeys_scene.png`（划词绑定 `Ctrl+Shift+D`）。
- `studious_en.ts` 544 条 / unfinished = 0；构建 `[15/15]`、`[5/5] Linking`。

### 未完成
真实按键录制仍未自动化验证（`--ui-shot` 下窗口存活时间太短，模拟点击 + SendKeys 三次都没打中），
交由用户手动确认；测试用的 `Ctrl+Shift+D` 已从注册表清掉，保持"默认空"。
### 追加：打开主页热键（2026-11-08）
- 场景快捷键最上面加一行「打开主页」：`settings/hotkey/home`（默认空），
  录制 / 重新录制 / 清除 与划词那行同一套（`comboFor` / `applyHotkey` / `clearHotkey` 加 `home` 分支）。
- 触发链：`main.cpp` 注册 id `home` → `screenFlow->launchRequested("home")` →
  `Main.qml` 的 `launch("home")` 做 `root.show()/raise()/requestActivate()`（托盘里也能叫出来）。
- 证据：`docs/ui-shots-qml/hotkeys_scene2.png`（两行都绑定时的样子）；
  构建 `[15/15]`、`[5/5] Linking`；`studious_en.ts` 546 条 / unfinished = 0。
- 测试用的 `Ctrl+Alt+H` / `Ctrl+Shift+D` 已从注册表清掉，保持"默认空"。
### 追加：左上角 logo 矫正（2026-11-08）
- 症状：`brand.svg` 的 viewBox 是 `40 0 0 24`（横向字标，宽高比 5:3），
  而 `Main.qml` 的 `Image` 只写了 `sourceSize: 22×22` 且没开 `fillMode` ——
  默认 `Stretch`，40×24 被硬拉成 22×22，竖笔画被拉长，看着"歪/被压扁"。
- 改法：`sourceSize: 52×32` + `width: 26; height: 15.6` + `fillMode: Image.PreserveAspectFit`
  （26 : 15.6 = 5 : 3，与 viewBox 同比例；sourceSize 给 2 倍保证高 DPI 清晰）。
- 证据：`docs/ui-shots-qml/logo_fix2.png` + `logo_fix2_crop.png`（A 与文水平并排、居中、无拉伸）；
  构建 `[9/9] Linking`。
### 追加：删掉「锁定本地引擎」（2026-11-08）
- 它本来想解决的问题：实时翻译每 600 ms 发一次请求，云端/Web 单次 1 s 以上，所以默认把实时锁在本地引擎上。
- 实际上它**从来没有生效**：`LiveSession` 一次都没读 `liveEngineLocked`（全仓 grep 只有
  `AppSettings` 存取 + `AppBackend` 转发 + `OverlayPage.qml` 那个开关）。引擎在实时里
  真正读的是 `settings/engine/id`（上一轮统一口径之后），这个开关纯属遗留死代码。
- 删除范围：`OverlayPage.qml` 的开关 + 说明行；`AppBackend` 的 `Q_PROPERTY`、getter、
  setter、成员 `m_liveEngineLocked`、构造回读；`AppSettings` 的字段、defaults、load、save
  （键 `settings/live/engineLocked` 一并从注册表清掉）。
- 实时工具栏里**没有**这个开关（只有「锁定选区」，那是防误拖选区的，属于另一个功能）。
- 验证：构建 `[9/9]` / `[5/5] Linking`；`studious_en.ts` 544 条（两条文案 vanished）、
  unfinished = 0；截图 `docs/ui-shots-qml/overlay_live_nolock.png` 确认「实时翻译」那组
  现在只剩 刷新间隔 / 只在内容变化时翻译 / 区域底色 / 译文不透明度 / 译文字号。
### 追加：截屏翻译的「复制」跟随原文/译文切换（2026-11-08）
- 以前 `ScreenFlow::copyResult()` 无论屏幕上显示的是原文还是译文，都复制译文
  （空块回填原文），用户看着英文点复制却拿到中文。
- 改法：按 `m_showOriginal` 分支 —— 显示原文时复制 `original`，显示译文时复制 `text`，
  缺哪边退回另一边（防止只识别出一半的块复制出空行）。
- 验证（`STUDIOUS_UI_CALL` 直接驱动，读真实剪贴板）：
  - `begin|debugSampleBlocks|copyResult` → 剪贴板是「你好，世界 / 这是一段用来检查排版的译文 / 第三行…」
  - `begin|debugSampleBlocks|toggleOriginal|copyResult` → 剪贴板是「Hello, world / This is a sample translation used to check layout / Third line…」
- 复制按钮在两种状态下都可见（`showing = phase == "Showing" || "ShowingOriginal"`）；
  构建 `[4/4] Linking`；无新增文案。
### 诊断：游戏窗口抓屏全黑（2026-11-08）
- 环境实测：Windows 10 IoT 企业版 LTSC **19044**，RTX 4060 Laptop + AMD 集显，
  未开 HDR（用户确认）→ 之前"HDR/独占全屏"的猜测**不成立**。
- 对每个窗口分别做 GDI `BitBlt(SRCCOPY|CAPTUREBLT)` 与 `PrintWindow(PW_RENDERFULLCONTENT)`，
  抽样 9 px 网格算平均亮度 / 亮像素占比：
  | 窗口 | BitBlt | PrintWindow |
  |---|---|---|
  | Studious | mean 92.6 / 44% | 232.9 / 100% |
  | Folia (CEF 浏览器) | 37.6 / 45% | 97.3 / 99% |
  | Firefox | 37.2 / 43% | 116.2 / 97% |
  | 逆夢のメナスフィア(DLC) ver1.08 (Chrome_WidgetWin_1) | **20.2 / 24%** | **3.6 / 5.6%** |
- 结论：普通窗口 BitBlt 都正常，只有游戏窗口两面都拿不到 → 它的画面被扫到
  硬件平面（MPO / independent flip），GDI 两条路都读不到。不是 OCR 或我们代码的 bug。
- 待验证的下一步：`Win+Shift+S` 系统截图能否拍到游戏（能 → DXGI/WGC 路线可行）。
### 实装：DXGI Output Duplication 抓屏兜底（2026-11-08）
- 新增 `frontend/src/platform/ScreenCaptureDxgi.{h,cpp}`：`captureWithDxgi(physicalRect, *error)`
  - D3D11 设备（BGRA）→ IDXGIDevice → 适配器 → 枚举输出，挑**完全覆盖选区**的那块屏；
  - `IDXGIOutput1::DuplicateOutput` → `AcquireNextFrame(300ms)` → `CopyResource` 到 staging
    → `Map` → 逐行转 QImage → 按输出左上角裁到选区；
  - 三种格式都转：`B8G8R8A8_UNORM` 直接拷、`R16G16B16A16_FLOAT`（scRGB 线性）走
    sRGB 编码、`R10G10B10A2_UNORM`（PQ）先 PQ→nits 再按 203 nits 白点归一；
  - 旋转输出（非 IDENTITY）明确报错，不输出方向错的图。
- `ScreenCaptureWin.cpp`：`looksAllBlack()` 命中后先调 DXGI 兜底，成功就用它，
  失败才报「…DXGI 兜底也没成功：<原因>」；另加诊断开关 `STUDIOUS_FORCE_DXGI=1`
  跳过 BitBlt 直接走复制（用来对比两条路）。
- CMake：`studious_platform` 加 `ScreenCaptureDxgi.cpp/.h`，链接 `d3d11 dxgi`
  （MinGW 有 `libd3d11.a` / `libdxgi.a`）。
- 踩坑（关键）：`AcquireNextFrame` 返回的帧如果 `LastPresentTime == 0`（只有鼠标更新、
  桌面内容没变），**帧纹理内容未定义**，我们第一版直接拷 → 整幅全黑；现在丢掉这种帧重取。
- 新增诊断 CLI `--capture-probe <x,y,w,h,out.png>`：抓一块物理像素区域，打印
  `mean / bright%` 并存图 —— 用来对比 GDI 与 DXGI。
- 实测（本机，Win10 19044 / RTX 4060）：同一块 700×400 区域
  GDI `mean=41.9 bright=28.5%`、强制 DXGI `mean=41.9 bright=28.5%` —— 两条路输出一致，
  说明复制的取帧/裁剪/颜色转换正确。`--smoke` 仍 `[DONE]`。
### 日文识别不出来：OCR 字典换成 PP-OCRv5 多语种（2026-11-08）
- 现象：框住日文，只有部分行出贴纸，识别结果像中文乱码。
- 根因：识别模型配的是 `ch_PP-OCRv4_rec` + `ppocr_keys_v1.txt`（6623 行），
  字典里**一个假名都没有**（实测 `has あ=False has ア=False`，全表只有 5 个片假名标点）。
  det 是无语言的（只找文本区域），能不能出字完全由 rec 的字典决定 ——
  所以假名要么被丢、要么被硬猜成相近汉字，整行长度一变，行合并也会跟着错位。
- 改法：rec + dict 换成 **PP-OCRv5 mobile rec**（16.5 MB）+ **v5 多语种字典**
  （18384 行，含平假名/片假名/汉字/英文）。det 仍用 PP-OCRv4 DBNet。
  - `backend/asset/manifest.json`：删 `ocr.rec.ppocrv4` / `ocr.dict.ppocrv4`，
    新增 `ocr.rec.ppocrv5`（sha256 `bf66820f…`）/ `ocr.dict.ppocrv5`（sha256 `1ea29636…`），
    双 url（huggingface + hf-mirror）。
  - 新增 `frontend/src/OcrModels.{h,cpp}`：`ocr::stageModels()` 统一把三件套收拢到
    `<资产根>/models/ocr/`，assetId 列表「新 id 在前、旧 id 兜底」；
    `ScreenFlow.cpp` 与 `LiveSession.cpp` 里各自抄的那份 `ocrModelsDir()` 改成一行转发
    （以前改模型集合要改两处，注释里还写着"将来要合并"）。
- 验收（同一张 1377×363 日文截图）：
  - 旧：0.621 / 0.911 / 0.930，文本被吞成 `…RPG…` 之类；
  - 新：0.962 / 0.999 / 0.813，识别为
    「【カジュアル】ストーリーを楽しむためのモ一ドです。/ 敵のステータスが大きく下がります。/
     「工ロRPG」といえばこの難易度です。」（长音ー 与 エ 各错一个，属单字混淆）；
  - 中文回归（界面截图 1380×340）：7 个框、信度 0.998~1.000，无退化。
- 顺手加的开发设施：`ScreenFlow::ocrTest()` 除了打 stderr，还会写 `<图>.ocr.txt`
  （UTF-8 旁注文件）—— 中文 Windows 控制台会把非 GBK 字符换成问号，假名根本没法看。
- 坑：`models/ocr.det.ppocrv4` 这类单文件资产会被每次 `begin()` 重新 stage 到
  `models/ocr/`，直接替换 `models/ocr/*.onnx` 会被覆盖回去 —— 要换模型必须换资产源文件。
## 实时翻译「检测不到文字 / 一直报错」的根因与修复（2026-11-08）

用户反馈：截图翻译能翻，实时翻译却报「检测不到文字」，而且一直不恢复。
实测（记事本放三行日文，`STUDIOUS_UI_TARGET=liveSession` 直接开局）：

- OCR 其实**认出了 4~6 块**，不是没有文字。
- 失败在后面：`translation finished ok=0`，错误是
  `[llm] 目标语言 zh 第 1 段失败：翻译失败（已尝试 1 次）：模型没有翻译（返回了原文）`。
- 再跑十几秒，状态变成「引擎忙，等下一帧」，`已更新 0 次` —— 因为每 600 ms 一帧，
  每帧都在走同一条失败路径，界面就一直停在错误上。

**根因一：把「模型原样退回原文」当成硬失败。**
菜单名（文件/编辑）、型号、人名、已经是目标语言的短句，模型返回原文是正确行为。
旧逻辑判它「可疑 → 清空 → 逐段补救 → 还是原样 → 整单失败」，
实时场景每 600 ms 撞一次，于是永远翻不出一帧。
修法（`backend/src/translators/LlmTranslator.cpp` 的 batch `review`）：
原样退回的条目**收下原文**、不再计入可疑，界面按「未翻译」渲染（贴纸画原文样式），
不再拖垮整单。

**根因二：连续 3 帧没字就收单**（已按用户要求删除）。
`LiveSession` 不再自己停；只把状态写成「这一帧没有文字（已连续 N 帧）」，
并加空帧诊断日志 `[live] empty frame: WxH mean=…`，用来区分「区域没框到」
「抓到自家浮层」「画面全黑」。

**配套**
- `ScreenFlow`（截图翻译）同样规则：译文 == 原文 → 按未翻译，不贴假译文。
- 新增 `STUDIOUS_LLM_TRACE=1` 诊断日志：请求 url/timeout/body 字节、返回 status/字节/片段、
  批量逐条「第 N 条返回了原文」的判定。默认关闭。

**验证**
- 干净选区（记事本正文两行）：`blocks=2 updates=1`，状态「画面未变 · 已更新 1 次」，
  贴纸为「状态会大幅下降。」「说到色情RPG，就是这个难…」（`docs/ui-shots-qml/live_ja_clean.png`）。
- 之前那张截图里出现的「文件(F) 编辑(E)」「Studious 截图/文本翻译」是**测试时我自己的窗口
  压在选区上**被 OCR 读到了，不是产品 bug；把窗口移开即消失。
## 本地 LLM 生命周期：选中即启动、切走即停、空闲回收（2026-11-08）

用户要求：
1. 选中「本地 LLM」时就把 llama-server 拉起来（不等第一次翻译）。
2. 配置里加开关：打开 = 切到别的引擎就停；关闭 = 一直留到软件退出。
3. 再加「空闲多少分钟自动关」，并且只在上面那个开关关闭时可用，否则置灰。

**改动**
- `frontend/src/AppBackend.cpp` `setEngineId()`：`next == "llm-local"` 且服务没跑时调
  `startLocalServer()`（缺资产会自己提示去资产页，不会误拉进程）。
- 同一个 `setEngineId()`：切走且 `localStopOnSwitch` 打开时 `shutdownLocalEngine()`（原有逻辑）。
- 构造函数：`m_localPreheat || m_engineId == "llm-local"` 时延迟 1.5 秒预热 ——
  「记住的选择」也算选中，否则开局显示「本地 LLM（未就绪）」还要再吃一次冷启动。
- `setLocalStopOnSwitch()`：打开时停掉空闲计时器，关闭时重新按空闲阈值计时。
- `frontend/qml/pages/ConfigPage.qml`：行名「切走就停」→「切换引擎时关闭」；
  「空闲自动停」开关与「空闲阈值」的数字框都加 `enabled: !backend.localStopOnSwitch`，
  并给出「已关闭：上面那个开关打开时，切走就会停」的解释文案。
  这四行（切换引擎时关闭 / 启动时预热 / 空闲自动停 / 空闲阈值）从「高级设置」里
  挪到本地 LLM 卡的主干上：它们是服务策略，不是采样参数。

**验证**
- 注册表 engine 先置 `web`，用 `STUDIOUS_UI_SET="engineId=llm-local"` 启动：12 秒内
  `llama-server` 进程出现，退出后归零（Job Object 收尾正常）。
- 重启时 engine 已经是 `llm-local`（记住的选择）：1.5 秒后进程起来，
  配置页显示「运行中 · 后端 vulkan · 端口 18080 · PID · 启动耗时 1760 ms」。
- 截图 `docs/ui-shots-qml/config_local_on2.png`：切换引擎时关闭=开，空闲自动停/空闲阈值置灰。
## 本地 MT 常驻 + 本地 LLM 单实例（2026-11-09）

**做了什么**
1. `LocalMtTranslator` 增加 `warmPair(pair)` / `unloadAll()` / `loadedCount()`，`m_mutex` 改 `mutable`，四个入口全部加锁（共享实例会被三个 `QuickTranslate` 在不同工作线程同时调到）。
2. `QuickTranslate` 暴露进程级共享实例：`sharedMt()` / `warmMtAll()` / `unloadMtAll()` / `mtLoadedPairs()` / `setMtUsedHook()` / `setLocalEngineStarter()`。`sharedMt()` 里比对 `LocalEngine::modelsDir()`，资产根迁移后重建，不会捧着旧路径。
3. `TranslateJobRequest` 增加 `sharedMt`，`TranslateJob` 用 `BorrowedTranslator` 适配器把工厂那个临时 `local-mt` 换掉（Router 仍然只看到 `ITranslator*`）。
4. 设置项 `settings/mt/preload`（默认 false）与 `settings/mt/idleMinutes`（0–180，0 = 不释放，默认 10）；`AppBackend` 四个属性 + `m_mtIdleTimer` + `touchMtIdle()`；翻译成功经 `setMtUsedHook` 重置计时。
5. `ConfigPage.qml` 本地 MT 卡新增「开机自启」「空闲释放」两行，`enabled` 绑 `backend.mtPacksInstalled`（第二行还要 `mtPreload`），置灰 0.45 透明。

**修的缺陷（用户报「截图/实时不能用本地 LLM」）**
- `AppBackend` 以前持有自己的 `LocalEngine m_localEngine`，而 `QuickTranslate` 用匿名命名空间里的另一个 `static LocalEngine *`：配置页启动的是 A，截图/实时检查的是 B → 一直提示「本地 LLM 还没启动」，点了启动也没用（用户看到的现象）。现在 `sharedLocalEngine()` 是唯一实例，`AppBackend` 的 23 处调用全部改过去，成员删除。
- `QuickTranslate::translate()` 的「本地 LLM 未运行」分支改成「有 starter 就先发起启动 + 标记可重试」，文案「本地 LLM 正在启动，稍后自动重试…」；`ScreenFlow` / `MiniSession` 各加 `m_retryLeft`（1 s 一次、上限 20），`LiveSession` 每帧天然重试。

**踩的坑**
- `class LocalEngine;` 前向声明写在 `studio::ui` 里会声明成另一个类，编译报 `invalid use of incomplete type`；必须放全局作用域。
- `lock_guard` 绑不上 const mutex（`discards qualifiers`）→ 成员改 `mutable`。
- `ConfigPage.qml` 没有 `page.labelWidth`（那是 `components/FieldRow.qml` 的 132），新行缩进要写字面量。
- 含中文的 QML 片段不能走 PowerShell here-string（引号被 GBK 解析炸），改成写 `_mt_rows.qmlpart` 再按行拼接。

**验证**
- 构建 `[76/76]`；`--smoke … --engine llm-cloud` → 6 ms 命中 1/1；`--local --local-model model.qwen2.5-0.5b-instruct.q4km` → 命中 1/1、70 ms 停服务、`[DONE]`。
- `[mt] warmed 6/6 language pair(s)`（开机自启全量预热）；`[mt] unloaded 6`（空闲释放）。
- `studious_en.ts` 555 条 / `unfinished = 0`。
## 实时翻译切本地 LLM 卡死整机：启动风暴（2026-11-09）

**证据（`%APPDATA%\studious\assets\logs\llama-server.log`）**
- 23:54:52 起 18080，23:54:53 又起一个 18081（上一个还占着端口），23:55:05 再起 18094、18095 —— 中间 18082~18093 全被跳过，说明当时已经有一串 llama-server 活着、每起来一个就把端口再往上推一格。
- 全程 start 记录 240 条；崩溃窗口里一秒内出现 2 条、同一分钟 4 条。每次 start 都要把 1.5B 的 q4km 权重重新塞进显存（Vulkan），叠起来就是显存被打满 → 驱动挂起 → 整机卡死。
- 结论：**不是模型太大，也不是 Vulkan 后端本身的问题，是启动被反复触发**。

**为什么会反复触发**
- `QuickTranslate::translate()` 在本地 LLM 没跑时会给 `starter()`（= `AppBackend::startLocalServer`）发一次请求，并标记 `lastFailureRetryable`。
- 实时翻译每 600 ms 一帧、截图重试每 1 s、小窗 1 s，全都在「引擎还在启动」这段时间里继续报「没在跑」→ 每次都再排一轮启动。
- `startLocalServer()` 用 `QTimer::singleShot(0)` 把阻塞挪出当前帧，于是这些请求排成队列；`LocalEngine::ensureStarted()` 开头就 `shutdown()` 再起新的，既没有「正在启动」标志也没有冷却，队列里每个都真的拉了一次进程。

**修法（四道闸）**
1. `LocalEngine`：新增 `std::atomic_bool m_starting` + `isStarting()`；`ensureStarted()` 入口 `exchange(true)`，后到的调用直接返回「本地引擎正在启动，等它起来再试。」（RAII 复位，任何 return 都会清标志）。
2. `AppBackend::startLocalServer()`：`m_localStartQueued` + `isStarting()` + `isRunning()` 三重判断，先到的那一次才排 singleShot；`runLocalServerStart()` 一进来就清标记。
3. `QuickTranslate`：只在 `!isStarting()` 且**距上次触发 &gt; 5 s**（`starterCooldownOver()`，静态 `QElapsedTimer`）时才调 starter；正在启动时只回「本地 LLM 正在启动，稍后自动重试…」。
4. `LiveSession`：可重试失败时 `m_retrySkipFrames = 3`，接下来三帧直接跳过（不抓屏、不发请求），状态显示「本地 LLM 正在启动，稍后自动重试…」。

**验证**
- 连打 10 次 `startLocalServer`（`STUDIOUS_UI_CALL` 每 100 ms 一次）→ 日志只新增 **1** 条 start（改前是每打一次起一个）。
- `--smoke … --local --local-model model.qwen2.5-1.5b-instruct.q4km` → 你好，世界！、97 ms 停服务、exit 0。
- 构建 `[11/11] Linking`。
## 应用级日志 + 双击 logo 打开日志窗口（2026-11-09）

**做了什么**
- rontend/src/LogSink.h/.cpp（新）：直接写 qInstallMessageHandler，4000 行环形缓冲（QMutex 保护）、落盘 `%APPDATA%\studious\logs\studious.log`、8 MB 轮转成 studious.1.log、200 ms 合并 notify、lushToDisk() 在 boutToQuit 落地。**必须转发旧 handler**，否则 --smoke 的终端输出会消失。
- AppBackend：logText/logFilePath/logLines + clearLogs/copyLogs/openLogFile/openLogDir，QML 不再自己拼 URL。
- rontend/qml/LogWindow.qml（新，900×600、无系统标题栏）：等宽正文、跟随最新、复制、清空、打开所在目录、打开文件；标题条可拖动、双击最大化。
- Main.qml：logo 上加 HoverHandler（手型）+ TapHandler.onDoubleTapped → 显示并前置日志窗口；logo 不在拖拽区，双击不会被 startSystemMove 吃掉。
- 补了约 10 处 qWarning（资产枚举失败、打开历史目录失败、翻译未就绪、剪贴板为空、切本地 LLM 缺资产等），不改行为。

**踩的坑**
- TextElideRight 不是枚举值，QML 里是 Text.ElideRight；写成前者只在运行时刷 ReferenceError，静态检查看不出来。
- 链接偶发 ld.exe: cannot open output file studious.exe: Permission denied，Get-Process 又看不到进程——等两三秒重试即可（杀毒/索引器短暂占用）。
- 子代理给的 LogWindow.qml 用的是旧的 ackend.localLogTail/localLogPath，且清空是"视图摘除"假清空；集成时全部换成 LogSink 的真接口。

**验证**
- 构建 [5/5]，Generated 566 translation(s) (566 finished and 0 unfinished)。
- STUDIOUS_UI_CALL='logWindow:show@1500' STUDIOUS_UI_SHOT='logWindow' → [ui-shot] window 'logWindow' 900x600 at (404,254) visible=1，零 QML 报错；截图 docs/ui-shots-qml/log_window.png（标题条 + 5 行真实日志 + 「5 行 · C:/Users/hua/AppData/Roaming/studious/logs/studious.log」）。
- 双击 logo 的真实鼠标事件未自动化（--ui-shot 下用 logWindow:show 代替），代码路径已审阅。
## 1.0.0 发布（2026-11-09）

- `version.json` 升到 `1.0.0`（codename 仍是 `Erstling`，channel `separate` / `test`），README 两处版号同步。
- 本次发布的实质内容：LogSink + 独立日志窗口、本地 MT 常驻（自启/空闲释放/无语言库禁用）、本地 LLM 单实例与启动风暴防护、DXGI 抓屏兜底、OCR 换 PP-OCRv5 多语种、实时/截图对「原文原样退回」按未翻译处理、本地 MT 长段按句切、缓存与端口竞态修复、资产来源选择性校验与 API Key 加密。
- 发布后仍未做：真实双击之外的按键录制自动化、4000 行日志压力、跨 8 MB 的写中轮转、模型文件的启动前校验。
## 页面切换过渡动画（2026-11-09）

需求：给所有跳转 / 页面切换加过渡（"简单动画有过度就行"）。

### 改了什么
- `frontend/qml/Main.qml` 的 `StackLayout`（主界面六页）：加 `id: pageStack`，`onCurrentIndexChanged` 重启两条 `NumberAnimation` —— 透明度 `0 → 1`（420 ms，`OutCubic`）+ `Translate.y` 从 `Theme.s2`（8px）回到 0（480 ms）。位移刻意做小，正文在长距离滑动里会糊。首版是 250 ms，用户要求「动画时间长一点」后统一放慢：设置页 380 ms、卡片淡入 320 ms、导航高亮 220 ms。
- `frontend/qml/pages/SettingsPage.qml` 右栏 `StackLayout`：同一套淡入（只做透明度，不做位移，因为卡片本身有上下边距，浮起来像抖动）。
- `frontend/qml/pages/ConfigPage.qml` 四张引擎配置卡、`frontend/qml/pages/OverlayPage.qml` 三张功能卡：它们靠 `visible: page.xxxTab === "..."` 切换，各加 `opacity: visible ? 1.0 : 0.0` + `Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }` —— 切到即淡入，切走时立即不可见（不拖泥带水）。
- 导航高亮：`Main.qml` 左侧 rail 的选中底色与文字色、`SettingsPage.qml` 左栏同两处，加 `Behavior on color { ColorAnimation { duration: 220 } }`。

### 踩的坑
1. **多行三元绑定中间不能插属性**。第一版用脚本在 `color: root.currentPage === railItem.index` 这一行后面直接插 `Behavior on color`，而它的值在下一行（`? Theme.primaryContainer : "transparent"`）→ `qmlcachegen` 报 `Unexpected token '?'`（`Main.qml:249`、`SettingsPage.qml:273`）。正确位置在整个绑定结束之后。
2. **同一个属性不能挂两个 `Behavior`**。`SettingsPage.qml` 的导航项原本已有一个 `Behavior on color`，我又加了一个 → 运行时刷 7 条 `Attempting to set another interceptor on QQuickRectangle_QML_107 property color - unsupported`（7 = 设置页左栏 7 个分区）。删掉重复那条即归零。

### 验证
- 构建 `[13/13] Linking`。
- `--ui-shot docs/ui-shots-qml/anim_page3.png --ui-page 3` 与 `anim_page4.png --ui-page 4`：页面正常渲染，stderr 里 `interceptor` / `ReferenceError` / `Unable` 全为零（只剩既有的清单未验签、`local-mt` 未选源语言、CLI 阶段 `QSqlDatabase requires a QCoreApplication` 三类老噪声）。
- 动画本身是运动过程，截图只能证明"终点正确、无 QML 报错"；手感需要肉眼过一遍。

## 导航与分段控件改成滑块（2026-11-09）

需求：「给左侧的导航栏和设置里的导航栏和那些多项选择，改成滑块设计」。

### 做法
- **主界面左栏**（`frontend/qml/Main.qml`）：不再让每个导航项自己涂选中底色。rail 里新增一颗 `railPill`（`Theme.rMd` + `Theme.primaryContainer`），`root.syncRailPill()` 用 `railRepeater.itemAt(currentPage).bodyItem.mapToItem(railPill.parent, 0, 0)` 量出当前项的真实位置再赋给胶囊；`Behavior on y / height` 240 ms `OutCubic` 滑过去。触发点：`onCurrentPageChanged`、`Component.onCompleted`（`Qt.callLater`，等首帧布局）、`railColumn.onWidthChanged`（切语言改字宽时重新对位）。delegate 加了 `property alias bodyItem: railItemBody` 供外部量尺寸。
  - 为什么不能按「下标 × 行高」硬算：第 1 项（快捷键）里夹着一条 1px 分割线，行距不是等距的。
- **设置页左栏**（`frontend/qml/pages/SettingsPage.qml`）：原来的 `Column` 外面包了一层 `Item`，里面先放 `sectionPill`（`y: page.currentSection * (40 + Theme.s1)`，`Behavior on y` 220 ms），再放 `Column { id: sectionNav }`。设置页的左栏高度固定 40、间距固定 `Theme.s1`，所以这里可以用算式，不必 `mapToItem`。导航项自身只保留悬浮反馈（`navHover.hovered ? surfaceHigh : transparent`）。
- **所有分段控件**（`frontend/qml/components/Segmented.qml`，界面语言、关闭时、引擎模式、引擎 tab、浮层功能 tab 全走它）：`Rectangle pill` 放到 `seg` 里、`RowLayout` 之前（垫在按钮下面）。`syncPill()` 遍历 `row.children`，按 `modelData.code === seg.current` 找到当前按钮，`mapToItem(seg, 0, 0)` 取坐标后赋 `x/y/width/height`；`Behavior on x / width` 220 ms。触发点：`onCurrentChanged`、`Component.onCompleted`、`Connections { target: row; onWidthChanged }`（父容器变宽或切语言改文字宽度都要重新对位）。按钮本身 `color: "transparent"`，文字仍按选中改色/加粗。

### 踩坑
1. **换容器时漏了一行**。把设置页 `Column` 包进 `Item` 时按行号切片，起始行取成 259（实际 `Repeater {` 在 258），重建后 `model:` 直接挂在 `Column` 下 → `SettingsPage.qml:1763: Syntax error`。补回 `Repeater {` 即通过。
2. **`mapToItem()` 不能写在绑定里**（不产生依赖，永远拿到 (0,0)），必须放在函数里、由事件触发调用。
3. 布局完成前 `itemAt(i)` 的量是 0，所以首次对位要 `Qt.callLater`。

### 验证
- 构建 `[11/11] Linking`（中途一次 `ld.exe: cannot open output file studious.exe: Permission denied`，是上一次 `--ui-shot` 的进程还没退，`Stop-Process` 后重链即过）。
- `--ui-shot` 拍 0/3/4/5 四页：`docs/ui-shots-qml/pill_0.png`（左栏「翻译」有胶囊、引擎分段「自动」有胶囊）、`pill_3.png`（「自动选择」「云端 LLM」胶囊位置正确）、`pill_4.png`（设置左栏「通用」+「跟随系统」「退出程序」胶囊）、`pill_5.png`；stderr 里 `ReferenceError` / `Unable` / `is not defined` / `Syntax` 全为零。
- 滑动过程本身截图拍不到，静态位置与动画参数已按上面写死。

## 悬浮残影 / 闪烁：改成整列单 handler + 悬浮胶囊（2026-11-09）

用户反馈（附截图，设置页左栏约 5 项带灰底）：「这个阴影是啥,鼠标在上面滑动会闪烁」。

### 根因
上一轮把左栏改成滑块时，每个导航 delegate 各自绑了 `color: navHover.hovered ? Theme.surfaceHigh : "transparent"` + `Behavior on color` 150 ms。鼠标快速划过时，途经的每一项都各起一次淡入淡出，截屏正好抓到一串不同透明度的灰块 —— 看起来像闪烁的残影，其实只是没人等的动画尾巴。

### 改法
- `SettingsPage.qml`：删掉 delegate 里的 `HoverHandler` 与颜色绑定（`color: "transparent"`）；整列 `Column` 上只留一个 `HoverHandler`，按 `point.position.y / (40 + Theme.s1)` 算出 `page.sectionHover`；根页加 `property int sectionHover: -1`，切分区时清零；新增 `sectionHoverPill`（surfaceHigh，`y`/`opacity` 各带 Behavior，停在当前分区时不显示）。
- `Main.qml`：主栏原本完全没有悬浮反馈，同法补一颗 `railHoverPill`。delegate 里只上报 `root.railHover = railItem.index`（离开时只有「还是自己」才清），胶囊位置由 `syncRailHoverPill()` 用 `mapToItem` 量真实坐标（第 1 项夹分割线，行高不等距，不能按下标乘行高算）；`onWidthChanged` 与 `Component.onCompleted` 都重新对位。

### 验证
- 构建 `[9/9] Linking`；`--smoke "Hello, world!" --engine llm-cloud` → exit 0。
- `docs/ui-shots-qml/hover_off.png`（默认 `sectionHover = -1`）：设置页左栏只剩选中项「通用」一颗胶囊，没有残影。
- 临时把 `sectionHover` 置 2 出 `hover_pill.png`（「缓存」一行有灰底、与选中胶囊同宽同高、文字对齐）；把 `railHover` 置 3 出 `rail_hover.png`（主栏「配置」有灰底）。两处都已改回 -1 并重新构建。
- stderr 无 `ReferenceError` / `Unable` / `Syntax` / `interceptor`。

### 教训
- 悬浮态不要用「每项一个 handler + 颜色动画」：鼠标扫过时动画会一段段残留。要么瞬时变色，要么一颗胶囊跟着走。
- `mapToItem()` 写在绑定里不产生依赖，位置要显式调用函数更新。
## 关闭确认弹窗（2026-11-09）

需求：右上角 ✕ 点了以后先问一句 —— 收进托盘还是关闭软件。

### 实现
- `frontend/qml/Main.qml`：新增 `property bool closePromptOpen`、`quitApplication()`（置 `forceQuit = true` 后 `close()`，绕过 `onClosing` 里「收进托盘」的拦截）、`minimizeToTray()`（`hide()`）。
- 遮罩 `closeScrim`：铺满窗口的 38% 黑，`opacity` 带 140 ms 过渡，点空白处或按 Esc 取消；卡片 `closeCard` 420 宽、居中、`scale 0.96 → 1`。
- 按钮：`最小化到托盘`（primary，`buttonEnabled: backend.trayAvailable` —— 装不上托盘就不给藏，否则点完就找不回来了）、`关闭软件`（danger 描边）。
- ✕ 的 `onClicked` 从 `root.close()` 改成 `root.closePromptOpen = true`。

### 验证
- 临时把 `closePromptOpen` 默认置 `true` → `docs/ui-shots-qml/close_prompt.png`：遮罩、卡片、标题「关闭 Studious」、说明、两个按钮都正确，`stderr` 无 `ReferenceError` / `Unable`。
- 改回 `false` 重新构建 `[13/13] Linking`；`--smoke` exit 0。
- `update_translations` → 741 条，4 条新串补齐（Close Studious / Keep running in the tray, or quit the app? / Quit app / Minimize to tray），`unfinished = 0`。
### 追加条件（同日）
- 用户要求：设置·通用里勾了「最小化到托盘」时，点 ✕ 不再弹确认。
- `Main.qml` 的 ✕ 分支改成三路：`minimizeToTray && trayAvailable` → `root.hide()`；否则 `closePromptOpen = true`。托盘装不上时仍走弹窗（弹窗里那颗按钮会置灰，至少还能选「关闭软件」）。
- 构建 `[9/9] Linking`；`--ui-shot docs/ui-shots-qml/close_prompt_off.png` 确认默认不弹、无 QML 报错。
## 字体内置与外观页字体选择（2026-11-09）

**做了什么**

- `font/` 里只挑真正用到的 5 个字重，用 fontTools 改写 `name` 表（1/4/16 族名、2/17 字重名、6 PostScript 名必须是 ASCII），存到 `frontend/resources/fonts/`：
  - `StudiousSans-{Regular,Medium,Bold}.otf` → 族名「软件默认-思源黑体」
  - `StudiousWenKai-Regular.ttf` → 「软件默认-霞鹜文楷」
  - `StudiousWenKaiMono-Regular.ttf` → 「软件默认-霞鹜文楷等宽」
- 新增独立静态库 `studious_fonts` 挂 qrc（`:/fonts/...`），`studious` 链接它；`loadBundledFonts()` 改成从 qrc 装载，删掉 `STUDIOUS_FONT_DIR` 宏。
- 外观页：「界面字体」「译文字体」两个 `AppComboBox`（`backend.fontChoices`），`ui/fontUi`、`ui/fontContent` 落注册表；`Main.qml` 的 `Binding` 改成「空 = 自带 / `__system__` = 系统 / 否则按族名」。
- 外观页删掉「主窗口」「语言显示名」两张卡（里面只有占位符）。

**为什么字体放在独立静态库**

`qt_add_resources(studious ... FILES fonts...)` 会把字体文件挂进目标的 `SOURCES`；`update_translations` 时 lupdate 照这份清单解析，25 MB 的 TTF 直接把它卡死（实测 422 s CPU 仍在跑，`--target update_translations` 两分钟超时）。`QT_EXCLUDE_SOURCES_FROM_TRANSLATION` 生成的排除正则带上了源码目录前缀，实测不生效。拆成 `studious_fonts` 之后 lupdate 只扫 `studious` 自己的源文件，恢复 1 秒内完成（559 条，0 new）。

**验证**

- `[font] ui=软件默认-思源黑体 content=软件默认-霞鹜文楷 mono=软件默认-霞鹜文楷等宽`（启动日志，来自 qrc）。
- 外观页截图 `docs/ui-shots-qml/font_lib_crop.png`：字体卡两个下拉都是「跟随项目字体」，下面是字号缩放 100%、翻译字号 13 px；卡后直接结束，两张删掉的卡不再出现。
- 字体切换实测：`ui/fontUi=__system__` 与默认渲染差 226,844 px；`ui/fontUi=软件默认-霞鹜文楷` 差 192,563 px，截图里正文/标题明显变成文楷（`font_wenkai_crop.png`）。
- 构建 `[92/92] Linking`；`lrelease` 559 条 0 unfinished；`exe` 146,512,419 B。

**坑**

- `i18n::s("中", "英")` 在 `studious::ui` 命名空间里会解析成 `studious::ui::i18n::s()`（返回 `Strings`），编译报 `no known conversion`；要写全限定 `studious::i18n::s(...)`（那是 `const char*` 版本）。
- 改 `qt_add_resources` 之后必须重新 `cmake -S frontend -B frontend/build`，只 `--build` 不会重新生成 qrc。
## 字体下拉改成「自带默认 + 系统字体」（2026-11-09）

**用户口径**：界面字体本来就是思源黑体、译文本来就是霞鹜文楷，那就把默认值做成这两个（不是"跟随项目字体"）；下拉里其余项读系统已装字体。

**改了什么**

- `AppSettings::defaults()`：`fontUi = "软件默认-思源黑体"`、`fontContent = "软件默认-霞鹜文楷"`（原来都是空串）。
- `AppSettings::load()`：老配置里写进去的空串统一折回默认值，升级后不会显示成空。
- `AppBackend::fontChoices()`：前两项是自带的两套，第三项「系统默认字体」，其余用 `QFontDatabase::families()` 全量枚举并按名字排序（本机 350 项 = 2 自带 + 1 系统默认 + 347 系统族名）；`knownFontChoice()` 白名单同步放开到「系统里真实存在的族名」，等宽那份不进下拉（界面/正文用等宽没意义）。
- `main.cpp`：`loadBundledFonts()` 从 1589 行提到 `AppBackend` 构造之前 —— 构造函数现在会核对「选中的族名是不是真的装了」，顺序反了会把自带的两个报成 `(MISSING)`。
- 新增两条诊断日志：`[font] selected ui=… content=…`（带 `(system)` / `(MISSING)` 标注）与 `[font] choices: N`。

**验证**

- 默认启动：`[font] selected ui=软件默认-思源黑体 content=软件默认-霞鹜文楷`，无 `(MISSING)`。
- `STUDIOUS_UI_SET="uiFont=__system__"` → 日志 `ui=(system)`，与默认截图差 106,751 px。
- `STUDIOUS_UI_SET="uiFont=Consolas"` → 设置被接受（`set uiFont = Consolas -> 1`），与默认截图差 96,140 px，证明系统族名可以直接用。
- 测完把注册表 `ui/fontUi`、`ui/fontContent` 写回两个默认值，再拍 `docs/ui-shots-qml/font_final.png` 确认。

**等宽字体的结论**：`Theme.fontMono` 全仓 53 处引用（日志窗口正文、截图浮层尺寸标、热键键位框、资产路径），**在用**，所以「霞鹜文楷等宽」继续留在包里，只是不进下拉。要省那 24.8 MB 只能改成系统等宽，等用户决定。
## 字体子集化（2026-11-09）

**目标**：exe 里的 97.2 MB 字体压下来，界面观感不能变。

**字符集**（`frontend/resources/fonts/subset.py` 生成，脚本入库）：ASCII / Latin-1 / Latin Ext-A、General Punctuation / Currency / Arrows / Math / Geometric / Dingbats（`✓ ✕ × ▾ ≈`）、CJK 标点 + 假名 + 全角半角、**GB2312 全量 6763 字**、**JIS X 0208 的汉字**（日文 OCR 原文）、**KS X 1001 的 2350 个音节**（韩语译文），再加项目源码/翻译文件里实际出现过的字。共 14,524 个字符。

**结果**（fontTools 4.68 subset，保留 `--layout-features=*`、name 表）：

| 字体 | 前 | 后 |
| --- | --- | --- |
| 思源黑体 Bold | 16.2 MB | 6.0 MB |
| 思源黑体 Medium | 15.8 MB | 5.8 MB |
| 思源黑体 Regular | 15.8 MB | 5.8 MB |
| 霞鹜文楷 Regular | 24.7 MB | 6.5 MB |
| 霞鹜文楷等宽 Regular | 24.8 MB | 6.3 MB |
| 合计 | 97.2 MB | **30.4 MB** |

exe：146.5 MB → **73 MB**。

**验证**

- 逐字查 cmap：日文样例（`【カジュアル】ストーリーを…`）、繁体样例、韩语样例 **0 缺字**；`✕ ▾ ⠿` 三字原字体本来就没有（一直靠系统回退），子集前后一致。
- 界面截图逐像素比对：子集前后 `docs/ui-shots-qml/font_default5.png` vs `subset_appearance2.png` **差异 0 px**。
- 构建 `[4/4] Linking`，`[font] selected` 仍是两个默认族名、无 `(MISSING)`。

**保留的兜底**：包里没有的字（生僻字、GB18030 扩展、全量谚文）走 Windows 字体回退，显示成系统字体，不出豆腐块。

**产物**：`frontend/resources/fonts/subset.py`（可重跑）、`README.md` 重写（字重表、字符集口径、覆盖自测、许可证、加字重五步、以及"别把字体挂回 studious 目标否则 lupdate 卡死"的警告）。
## 文案面向用户 + 卡片防溢出 + 快捷键页图标（2026-11-09）

**卡片溢出（`frontend/qml/pages/TranslatePage.qml`）**

- `LauncherCard` 加 `clip: true`；`Layout.minimumHeight` 156 → 104、`preferredHeight` 200 → 176；外层 `RowLayout.minimumHeight` 176 → 104 —— 窗口压矮时卡片先缩，内容被裁在圆角内，不再顶出去。
- 描述文字 `maximumLineCount` 按卡高三档（≥190 三行 / ≥150 两行 / 其余一行）+ `elide: Text.ElideRight`；分隔线与页脚那行 `visible: card.height >= 140`，太矮时整块收掉。
- 删掉三张卡的 `required property string tags` 与「全局热键 · 置顶不抢焦点 · 回车即译」那行（含三处赋值）。

**文案：开发者口气 → 用户口气（约 70 条，QML + 一处 C++）**

- 参数名直接露英文的：`Base URL`→「服务地址」、`API Key`→「密钥」、`timeoutMs`→「等待上限」、`maxTokens`→「最长回复」、`temperature`→「发散程度」、`systemPrompt`→「风格要求」。
- 内部术语：`后端`→「运行方式」（值 vulkan 显示「显卡加速」）、`PID`→「进程号」、`实际路径`→「保存位置」、`运行时`→「加速组件」、`运算线程`→「线程数」、`附加参数`→「附加选项」。
- 重写的长句：本地 LLM 的启动/预热/空闲三段说明、MT 常驻与空闲释放、缓存（`命中/未命中`→「复用 / 新翻译」、`条数`→「已缓存译文」、`数据库路径`→「缓存文件」）、代理与超时、资产的下载/导入/分类、`管「应用怎么跑」…` 副标题。
- 删掉 `qsTr("缓存键含引擎")` 徽章；`Stub` 的「未实现」→「即将支持」；OverlayPage 的「边框颜色跟随强调色，不单独暴露」「和浮层里那颗『设置』是同一份值」等改成口语；MiniWindow 的「自动检测 (auto)」「自动存进 mini.db」去掉内部标识。
- `frontend/src/AppBackend.cpp` 的本地 MT 副标题：不再用 `routeDescription()`（会露出 `mt.opus.en-zh`），改成 `英语 → 中文 · 本地语言包`（走 `LanguageRegistry::displayName`）；诊断细节仍留在状态栏与日志。

**快捷键页图标（`frontend/qml/pages/HotkeysPage.qml`）**

- 三张卡原本是 `translate.svg / translate.svg / overlay.svg`（复制粘贴留下的错），改成与翻译页一致：`mini.svg` / `screenshot.svg` / `realtime.svg`。
- 两页卡片图标都加 `sourceSize.width/height: 64`（先 2 倍光栅化再缩到 32），DPR 1.5 下描边不再发虚。

**翻译**

- `lupdate` 后 563 条，逐条补英文，`unfinished = 0`；`lrelease` → `Generated 563 translation(s) (563 finished and 0 unfinished)`。
- 坑：想清 `vanished` 条目时用「`<message>…</message>` + `re.S`」整块删除，正则跨块吞掉了正常条目，`studious_en.ts` 从 801 条被删到 172 条；`git checkout` 还原后重新 `lupdate` + 脚本逐条填。以后清 obsolete 交给 `lupdate -no-obsolete`，别自己写正则。

**验证**

- 构建 `[11/11] Linking`；`lrelease` 563/0。
- 截图：`docs/ui-shots-qml/cards_small.png`（1120×620，卡片不再溢出）、`cards_normal.png`（1120×720，页脚与分隔线正常）、`text_cards_cfg.png`（配置页新标签）、`hotkeys_icons.png`（三卡图标与翻译页一致）、`translate_icons.png`。

## 数据目录内迁 + OCR 卸载确认（2026-11-09）

**用户口径**：① OCR 三件套的删除要有提示（删了就不能识别图像）；② 资产目录从系统目录搬进项目目录，顺手把结构整理干净，多余文件（含 `docs/`）挪到 `C:\Code\code\project\doc`。

**① 路径统一到一个根**
- `AssetPaths.h/.cpp` 新增 `dataRoot()`：从 exe 目录往上找 `version.json`（最多四层）当「应用目录」，默认数据根是它下面的 `data/`；找不到就用 exe 目录 —— 宁可放 exe 边上，也不再散回 AppData。
- 新增 `logsDir()` / `recordsDir()` / `cacheDbPath()`；`assetsRoot()` 默认值从 `<AppData>/studious/assets` 改成 `<dataRoot>/assets`（设置里的自定义资产根仍然优先）。
- 接线四处：`LogSink` 走 `logsDir()`（原来是「资产根的父目录/logs」，资产根一改日志就乱跑）；`main.cpp` 与 `QuickTranslate` 的 cache 走 `cacheDbPath()`；`AppBackend` 的 records 走 `recordsDir()`；`OcrModels` 与 `--assets-cli/--asset-download` 的安装根走 `assetsRoot()`。
- llama-server 的日志仍在 `<资产根>/logs/llama-server.log`（后端按运行时目录倒推，不依赖界面层）。
- `.gitignore` 加 `/data/`（5.6 GB 可再生数据，绝不入库）。

**② 文件搬迁（robocopy /MOVE，秒级、不复制）**
- `%APPDATA%\studious\assets|logs|records` + `cache.db` + 旧 `records.db` → `Studious\data\{assets,logs,records}` 与 `data\{cache.db,records.db}`；旧目录已空。
- `data/assets/dist`（639 MB 的 opus zip，代码里零引用）→ `doc\studious-old-cache\dist`。
- 仓库根杂物 `b2.log`/`b4.log`/`b_test.log`/`mt_warm.log`/`_tmp_smoke.log` → `doc\logs-archive\`；整个 `docs/` → `doc\docs\`（本来就没入库，`git ls-files docs` 为 0）。

**③ OCR 卸载确认**
- `frontend/qml/pages/SettingsPage.qml` 的 `uninstallConfirm` 加 `readonly property bool isOcr: assetId.startsWith("ocr.")`：仅 OCR 三件套显示红框警告「删掉之后就没法识别图像了：截图翻译和实时翻译都会停在「缺少 OCR 模型」，直到你把这三个组件重新下载回来。」
- 顺手修好弹窗定位：`parent: Overlay.overlay` + `anchors.centerIn: Overlay.overlay`（原来 `anchors.centerIn: parent` 会按页内 StackLayout 坐标算，出过一次弹窗被顶到左上、半截在窗外的图）。
- 缺文件时的错误处理本来就有：`ScreenFlow`/`LiveSession` 都返回「缺少 OCR 模型：%1，去资产页下载。」（这次把 ScreenFlow 那条补上了「去资产页下载」），`OcrEngine` 侧是「OCR 模型缺失：%1（目录 %2）」，无降级、不会崩。

**验证**
- 构建 `[9/9] Linking`；`[logsink] installed, file=C:/Code/code/project/Studious/data/logs/studious.log`（新根生效）。
- `--asset-verify model.qwen2.5-3b-instruct.q4km` → `origin=download -> ok`；`--asset-verify ocr.rec.ppocrv5` → `origin=unknown -> ok`（读到的是 `data/assets`）。
- `--smoke "Hello, world!" --from en --to zh --local --local-model model.qwen2.5-0.5b-instruct.q4km` → 你好，世界！、`[local] 已停止 llama-server（耗时 61 ms）`、`[DONE] exit 0`。
- 新增两处译文补齐，`unfinished = 0`；截图 `doc/docs/ui-shots-qml/ocr_uninstall_confirm.png`（1680×1350，弹窗居中、红框警告完整）。
- 坑：给 Popup 加 `anchors.centerIn` 前要删掉原来那条，否则 `Property value set multiple times`（qmlcachegen 直接编译失败）。

## 快捷键页按钮闪烁 + 界面字体按实际用字裁剪（2026-11-09）

**① 按钮闪烁**
- 现象：快捷键页把鼠标移到「录制」/「清除」上时，按钮底色一闪一闪的，像是残留的阴影。
- 根因：卡片上抄了翻译页入口卡的 `scale: hover.hovered ? 1.012 : 1.0`。翻译页那三张卡里没有按钮，放大无害；快捷键卡里有按钮，放大后按钮跟着位移，指针正好压在按钮边缘时 `HoverHandler` 在"进/出"之间反复翻，按钮的 `Behavior on color` 就一遍遍重放。
- 修法：快捷键卡删掉 `scale` 与对应的 `Behavior on scale`，只留 `color` / `border.color` 的过渡；注释写明「这张卡里放着按钮，绝不能做缩放/位移」。
- 验证：构建 `[12/12] Linking`；截图 `doc/docs/ui-shots-qml/font_ui_subset.png`（快捷键页三卡正常）。

**② 界面字体裁到实际用字**
- `subset.py` 加 `--ui` 模式：扫 `frontend/qml`、`frontend/src`、`backend/{src,include}`、`frontend/translations` 里出现过的非 ASCII 字（含后端 `i18n::s/sf` 的中文），加 ASCII / Latin-1 / 通用标点 / 箭头符号 / CJK 标点 / 全角半角整段，只处理 `StudiousSans-*`。
- 结果：**2,835 字（汉字 1,308）**；三份字体各 0.7 MB（原 5.8~6.0 MB），exe **73 → 57 MB**。
- 覆盖自测：拿原字体 cmap 逐字比对，介面字符集里原本存在的 2,093 字**一个没丢**；两张截图（快捷键页 / 设置·资产）逐字检查无异常字形。
- 译文用霞鹜文楷仍是宽口径（GB2312 + JIS 汉字 + 韩语音节），不许跟着裁 —— 译文是任意文本。
- `TODO.md` 顶部新增「0. 置顶：加界面文案就要重裁界面字体」，写清触发条件与命令；`frontend/resources/fonts/README.md` 补 `--ui` 模式、两张字符集口径与体积对比。
- 临时产物 `_fontbuild/`、`_charset*.txt`、`_check_subset.py` 已删。

## 资产「从文件导入」按分类分派（2026-11-09）

**问题**：资产卡的「从文件导入」原来直接弹一张 16 条的长列表让人挑条目，用户不知道
本地 LLM / 本地 MT / OCR 各自该选哪一条，也分不清哪些是运行时（不能导入）。

**改法（rontend/qml/pages/SettingsPage.qml）**
- 「全部」栏点「从文件导入」→ 先弹 importCategoryPicker：三个按钮，每颗下面用等宽字
  写清「装到哪一条」（已安装的补「（已安装，会覆盖）」），点类别直接进系统文件选择框。
- 已经在某个分类栏（LLM / 本地 MT / OCR）里 → 一次点击直接开文件选择框，不再多问。
- 行内「导入文件」（占位模型，例如 7B）保持原样：那条的 id 是明确的，直接选文件。
- 目标条目怎么定：importTargetFor(category) —— 占位条目优先（LLM 落到
  model.qwen2.5-7b-instruct.q4km），其次第一个未安装的，都装齐了才落到该类代表条目
  （MT 用 mt.opus.en-zh、OCR 用 ocr.rec.ppocrv5）；运行时（	ype: runtime）一律跳过。
- 文件框标题改成「选择要装进 <assetId> 的文件」，避免选完才猜装到哪儿去了。
- 顺带修掉资产卡下面那句误导文案：原来写「导入的文件会先校验完整性，对不上会拒绝」，
  实际用户导入的文件不过哈希（记 Import 来源），改成如实描述。

**验证**：构建 `[9/9]`；临时把 importCategoryOpen 默认置 true 截图
`doc/docs/ui-shots-qml/import_category.png`，沿弹窗中线做像素带扫描确认三颗 tonal
按钮分别在 y=530/625/719，弹窗到 y≈820 结束、没有溢出；随后改回 false 重新构建；
`--smoke "Hello, world!"` → 你好，世界！；`studious_en.ts` 571 条、unfinished = 0。
## 资产目录分格（models/llm|mt|ocr）+ 导入按钮hover/目录 + MT 默认根（2026-11-09）

**问题**：`data/assets/models` 平铺了 16 个条目（3 个 GGUF、6 个 `mt.opus.*` 目录、5 个
`ocr.*` 文件 + 一个 `ocr/` staging 目录），肉眼分不清谁能被 llama-server 打开、谁会被
OCR 直接读；资产卡的「从文件导入」按钮在弹出层里没有悬浮反馈；点它总是从 C:\ 开始翻。

**改法**
- `frontend/src/AssetPaths.h/.cpp`：新增 `modelsLlmDir()` / `modelsMtDir()` / `modelsOcrDir()`。
- `backend/src/asset/AssetRegistry.cpp` 的 `resolveTarget()` 按 id 前缀分派：
  `runtime` → `runtime/`；`mt.` → `models/mt`；`ocr.` → `models/ocr`；其余 → `models/llm`。
- `backend/src/runtime/LlamaServer.cpp` 的 `installRoot()`：模型路径往上推根时，先判断末级
  目录是不是 `llm|mt|ocr`，是就多 `cdUp()` 一层（否则会把 `models/llm` 当成根）。
- `frontend/src/LocalEngine.cpp` 的 `modelsDir()` 指向 `models/llm`；`frontend/src/QuickTranslate.cpp`
  的 MT 两处改指 `modelsMtDir()`；`frontend/src/OcrModels.cpp` 的 staging 目标改成 `models/ocr/run/`，
  候选路径同时兼容新的 `models/ocr/<id>`、旧的平铺 `models/<id>` 和已收拢的 `models/ocr/<file>`。
- 磁盘已按新布局搬好：`llm/` 三个 GGUF，`mt/` 六个语言包，`ocr/` 五个资产文件 + `run/` 收拢副本。
- `frontend/qml/components/AppButton.qml`：`tonal` 以前**没有** hover 分支（恒 `surfaceHighest`），
  弹出层底色本来就是 `surfaceHigh` → 看起来像"死的"；改成 hover 抬到 `primaryContainer`。
- `frontend/qml/pages/SettingsPage.qml`：新增 `assetFolder(id)`（取安装路径的父目录）与
  `openImportDialog()`，把 `beginImport()`、行内「导入文件」、`importPicker` 选中三处统一走它，
  文件框默认落在该类资产所在目录；数据目录树文案同步成 `models/{llm,mt,ocr}` + `ocr/run/`。
- `backend/src/translators/LocalMtTranslator.cpp` 的 `defaultModelsRoot()`：不再写死
  `%APPDATA%/studious/assets/models`，改成从 exe 往上找 `version.json` → `<根>/data/assets/models/mt`，
  找不到才退回旧路径。此前 CLI / `TranslateJob` 走的是工厂默认值，指向早已废弃的 AppData。

**验证**
- 构建 `[26/26] Linking`、`[4/4] Linking`。
- `--asset-verify` 三条都 `ok`（新布局能定位到实际文件）。
- `--smoke "Hello, world!" --engine local-mt` → 你好,世界观! exit 0；`--local` → 你好，世界！+ 68 ms 停服务。
- OCR 走新 staging 路径实测：`[ocr] staged models into …/models/ocr/run`，同一张日文测试图
  识别 0.980「こんにちは、世界。」/ 0.910「今日はいい天気ですね。」。
- `update_translations` 574 条（5 new）→ 补齐 → `lrelease` 574/0；`--ui-shot` 资产分区截图
  `doc/docs/ui-shots-qml/assets_split.png`。

**坑**
- `--assets-cli` 仍然需要显式 manifest 路径（`backend/asset/manifest.json`），不传会打印用法。
- `main.cpp` 的 usage 里 `--asset-verify` 的说明行被插到了 `--capture-probe` 后面，已调回。

## 设置页「滚到底再切分区变空白」（2026-11-09）

**现象**：设置右栏滚到最底部（缓存那张卡最长），再点别的分区，右侧一片空白；往上滚能看到内容。

**根因**：右栏是 `ScrollView > ColumnLayout > StackLayout`，`StackLayout` 的隐式高度取的是
**所有子项里最高的那个**（缓存卡带历史列表，比别的高出一大截）。于是短分区底下多出一大片
空滚动区；`ScrollView` 切分区时又不会把 `contentY` 归零，停在旧的越界位置 → 看着像没渲染。

**改法（`frontend/qml/pages/SettingsPage.qml`）**
- `sectionStack` 加 `Layout.preferredHeight`，只跟当前项走：
  `{ var it = itemAt(currentIndex); return it ? it.implicitHeight : 0 }`
  （StackLayout 没有 `currentItem`，先写 `currentItem` 会报 `ReferenceError`。）
- `onCurrentSectionChanged` 里把 `contentScroll.contentItem.contentY = 0`，换分区一律回顶部。

**验证**：构建 `[9/9]`；`--ui-shot` 拍 1/2/3/6 四个分区，逐行扫右栏像素求内容范围 ——
内容分别止于 y=682 / 1313 / 649 / 979（物理像素），短分区不再带一大片空滚动区；
运行时无 `ReferenceError`/`Unable`。

## 资产路径收口：一个 dataRoot + 三种模型目录（2026-11-09）

**起因**：语言包搬到 data/assets/models/mt 之后，配置页把本地 MT 报成「缺资产」——
AppBackend 有三处用 LocalEngine::modelsDir() 去扫语言对，而那个函数被我刚指到
models/llm。同一份「数据根在哪」的推导在前后端各有一份，改一处漏一处。

**改法**
- 后端成为唯一实现：新增 ackend/include/studious/asset/DataRoots.h +
  ackend/src/asset/DataRoots.cpp——dataRoot()（从 exe 往上最多四层找 ersion.json →
  <应用目录>/data，找不到用 <exe 目录>/data）、ssetsRoot()、modelsRoot("llm"|"mt"|"ocr")。
  放在后端是因为本地 MT、CLI、自测程序都不知道 UI 存在，也该算出同一份路径。
- rontend/src/AssetPaths.cpp 的 dataRoot() 改成一行转发（ssetsRoot() 仍保留
  「设置里可以改安装根」这一层 UI 覆盖）。
- ackend/src/translators/LocalMtTranslator.cpp 删掉自己的 dataRootCandidate() /
  modelsRootFor()，defaultModelsRoot() 用 studious::asset::modelsRoot("mt")，
  %APPDATA%/studious/assets/models/mt 存在而新位置不存在时才回落旧路径。
- LocalEngine::modelsDir() 全量改名 llmModelsDir()：它只代表"GGUF 在哪"，
  名字含糊时被拿去扫语言包是必然的。四处 MT 扫描 + --mt 诊断改用 paths::modelsMtDir()。

**验证**：构建 `[24/24] Linking`；`--asset-verify mt.opus.en-zh` → `ok`；
`--smoke ... --engine local-mt` → 命中 1/1、「你好,世界观!」；配置页四行状态全绿
（本地 MT「就绪 · 英语 → 中文(简体) · 本地语言包」）。

**坑**：用 PowerShell 的字符串替换插 include，单引号里的 ` 
 ` 会当字面量写进源码
（这次两次都是），编译报 `'dataRoot' is not a member of 'studious::asset'`。改源码一律用
`edit`。
## 「关闭软件」改成真退出（2026-11-09）

**现象**：右上角 ✕ 的弹窗里点「关闭软件」，窗口没了但进程还在——托盘图标、llama-server
都留着。原因是那条分支只 oot.close() 关了窗口，onClosing 之外的清场谁也没跑。

**改法**
- Main.qml 的 quitApplication() 改成调 ackend.quitApplication()（原来是 `root.close()`）。
- `AppBackend::quitApplication()`（原本只摘托盘）补成完整收尾：
  1. `m_quitting = true`：之后排队的本地引擎启动一律不执行；
  2. `shutdownLocalEngine()`：正常停 llama-server；
  3. 新增 `platform::killChildProcesses()`（`ProcessGuard.cpp`，Toolhelp32 快照按父进程号
     `TerminateProcess`）：引擎还在启动、runner 还没交给 `LocalEngine` 的情况下，
     正常 stop 是空操作，必须按父进程兜底清一遍；
  4. `logsink::flushToDisk()` 再退事件循环；
  5. 2 秒兜底 `QTimer`：还没退就再补一刀子进程后 `std::_Exit(0)`。
- `runLocalServerStart()` / `startLocalServer()` 开头加 `m_quitting` 短路。

**验证**（`STUDIOUS_UI_SET="engineId=llm-local"` + `STUDIOUS_UI_CALL="quitApplication@9000"`）：
- 改前：`app_alive=0 llama_left=1`（主进程没了，llama-server 孤儿，Vulkan 常驻显存）；
- 改后：`app_alive=0 llama_left=0`。
- `--ui-shot` 模式下会看到 `graceful shutdown timed out, forcing exit`：那条分支用的是
  自己的 `QEventLoop`，`QCoreApplication::quit()` 不会退它，只能等兜底 `_Exit`；
  真实 GUI 走 `exec()`，`quit()` 立即返回，不会走兜底。

**坑**：`tlhelp32.h` 必须在 `windows.h` 之后 include，否则整片 `HANDLE/DWORD` 报未声明。
## 托盘不出现：hide() 之前没人把图标挂上（2026-11-09）

**现象**：✕ 弹窗里点「最小化到托盘」，窗口直接消失，通知区没有图标 —— 等于把程序藏没了。

**根因**：托盘图标在 Desktop::createTray() 里是**先建成隐藏态**（NIS_HIDDEN），
只有 setTrayEnabled(true) 才 NIM_MODIFY 取消隐藏。而 syncTrayState() 只在
「设置里的最小化到托盘 / 关闭到托盘」为真时才显示；弹窗那条路是临时决定，两个开关都关着，
于是 oot.hide() 之后什么都没有。

**改法**
- AppBackend::ensureTrayIcon()（新，`Q_INVOKABLE bool`）：先 setTrayEnabled(true)，
  再 	rayAvailable() 复核；失败给一句「这个会话里装不上托盘图标，窗口先不藏。」
- Main.qml 的 minimizeToTray()、标题栏「−」、标题栏「✕」（设置里开了托盘时）三条路
  全部改成「ensureTrayIcon() 成功才 hide()」，失败退回 showMinimized() / 现状。
- m_trayForced：临时挂过一次之后，syncTrayState() 里也算「需要」，
  否则下次改任何设置都会 setTrayEnabled(false) 把图标摘掉，窗口又没了。
- Connections { target: backend; function onRestoreWindowRequested() { show/raise/requestActivate } }：
  之前 Desktop::restoreRequested → AppBackend::restoreWindowRequested 有信号但 QML 没人接，
  点托盘也唤不回窗口。
- Desktop::setTrayEnabled() 加一行 `[tray] show|hide -> N` 日志，便于判断是 Explorer 拒绝还是逻辑没走到。

**验证**：`STUDIOUS_UI_CALL="ensureTrayIcon@1200"` → 日志 `[tray] show -> 1`（Explorer 接受）；
常态启动只有 `[tray] hide -> 1`（设置没开，保持隐藏），QML 零报错。

## 本地 MT 路由：目标语言是英语时一条路都不试（2026-11-09）

**报的现象**：`pt→en` 直接报

    缺少本地翻译模型：需要 mt.opus.pt-en，或「pt→en + en→en」，或多语言模型 mt.opus.mul-en / mt.opus.en-mul

明明装了 `mt.opus.mul-en`，而且「en→en」这种语言对根本不存在。

**两个根因**

1. `planFor()` 里所有「经英语中转」的分支都带 `d != "en"` 前置条件 —— 目标是英语时
   提前把整条链排除了，连 mul-en 都不试。落到 resolve() 的兜底文案后，模板写死
   `"%2→en + en→%3"`，d 就是 en，于是印出 `pt→en + en→en`。
2. 兜底文案不区分方向：`s == "en"` 时会印出 `en→en + en→pt`，同样不存在。

**改法**

- `planFor()`：直连之后加「目标是英语」这一支 —— `d == "en" && s != "en" && mulEnReady`：
  - `mulCoversLanguage(s)` 为真 → `RouteKind::MulEn`（mul-en 本来就是"任意源→英"，一步到位）；
  - 否则记 `mulUnsupportedReason(s, false)`，报"多语言模型不支持 XX 作为源语言"。
- `translate()` 的 MulEn：出英文后若 `dst == "en"` 直接 `outText = middle; return true;`
  （以前会去 `loadOrFail("en","en")`，那对模型根本不存在）。
- `routeDescription()`：MulEn + `d == "en"` 给「多语言模型 mul-en（自动识别源语言→en）」。
- `resolve()` 兜底文案按方向分三支：`d == "en"` / `s == "en"` / 其余。
  `missingAssets` 同步只列这条方向真正缺的东西（目标英语时不再把 en-mul 算进来）。
- `TranslatorRouter` 的强制引擎分支：`unsupportedDsts` 非空时先 `resolve(src, bad.first())`，
  有 reason 就用 `指定的引擎 %1 翻不了这个方向：%2`，没有才退回旧的"不支持目标语言"。
  因为本地 MT 这里多半是**源语言**读不了（mul-en 词表里没有韩语）。

**顺手补的一道闸**（`LocalMtTranslator::translate()`）：整段译文 `trimmed().isEmpty()`
时不再当成功，报「本地 MT 这轮没有产出文字：模型可能不认识这段的语言」——
以前会返回 ok + 空串，界面看着像"翻完了但什么都没显示"。

**验证**

| 方向 | 结果 |
| --- | --- |
| `pt→en` | 30 ms / `Hi, world!`（缓存命中）；冷跑 1088 ms / `Good morning, all right?` |
| `fr→en` | 1068 ms / `Hello everyone. How are you going today?` |
| `en→pt` | `Olá, mundo!`（en-mul + `>>por<<`） |
| `zh→en` / `ja→en` / `en→zh` | 直连对，回归正常 |
| `ko→en`、`ko→pt` | `指定的引擎 local-mt 翻不了这个方向：多语言模型不支持韩语作为源语言（…）` |

**排错记录（别踩）**：中途两次以为「译文是空的」，其实是我在 PowerShell 里用
`Select-String '耗时|译文|FAIL'` 过滤，把不带这些关键词的译文行自己滤掉了。
查这类问题要么看全量输出，要么用下标取 `译文:` 的下一行。

## 小窗「保存到记录」→「重新翻译」：保存不该有第二个开关（2026-11-09）

**问题**：小窗结果出来之后主按钮写的是「保存到记录」，可记录本来就由「设置 · 缓存 →
保存翻译历史」控制。等于同一件事两个入口：开关关着点按钮报一句"没存进去"，
开关开着还得手动点一次才存 —— 前者是误导，后者是多余。

**改法**

- `frontend/src/MiniSession.cpp` 的 `onFinished()`：翻完（`phase == Done`）时
  `if (AppSettings::load().historyEnabled) saveRecord();` —— 存不存只看那一个开关。
- 主按钮 Done 态改成 `qsTr("重新翻译")`，调用新增的 `MiniSession::retranslate()`；
  `retranslate()` 置 `m_forceFresh = true` 再走 `translate()`。
- `frontend/src/QuickTranslate.cpp` 的 `translate()` 多一个 `bool bypassCache = false`：
  为真时 `req.cache = nullptr` / `req.cacheEnabled = false`，**这一次**不读也不写缓存。
  不绕过的话「重新翻译」会立刻拿回上一份结果，30 ms 就回来了，按下去像没反应。
- `frontend/qml/overlay/MiniWindow.qml`：按钮文案与 `onTapped` 分支同步改掉；
  `saveRecordRequested` 的信号链保留（现在是自动保存的回执），
  `saveHint` 仍会显示「已保存到记录。」。

**验证**（`STUDIOUS_UI_CALL` 驱动小窗，读 `data/records/mini.db` 行数）

| 场景 | 结果 |
| --- | --- |
| `history/enabled = 1` 翻完 | 提示「已保存到记录。」，`mini.db` 行数 4 → 4（本次命中原行，未重复插） |
| `history/enabled = 0` 翻完 | 无提示，行数保持 4 —— 开关确实独占保存 |
| Done 态按钮 | 文案「重新翻译」 |
| 点「重新翻译」 | 面板耗时 **1051 ms**（同一段此前是 30 ms 缓存命中）→ 真重发了请求 |

**顺带发现的限制**（未改）：`srcLang = auto` + 很短的英文（如 `Hello, world!`）时，
语言检测置信度不到 0.5，本地 MT 会直接拒：「无法确定源语言：本地 MT 需要明确的语言对」。
不是新问题，但小窗默认 auto 时用户第一次按「翻译」很容易撞上。

## 清单未验签告警降噪（2026-11-09）

- 现象：每次解析内置清单都打一句「清单未验签：调用方关闭了 requireSignature…请勿在生产环境使用」，正式包里属于噪声（内置清单本来就是未签名的已知状态）。
- 改法：ackend/include/studious/asset/Manifest.h 的 ManifestVerifyOptions 新增 ool warnWhenUnsigned = true;（只影响提示，不影响 equireSignature 的强制验签）；ManifestParser.cpp 的告警块包进 if (opts.warnWhenUnsigned)。
- 调用方：rontend/src/AssetPaths.cpp 设 opts.warnWhenUnsigned = qEnvironmentVariableIsSet("STUDIOUS_DEV_MANIFEST") —— 开发期读仓库那份清单时喊，正式包内置清单不喊。main.cpp 的 --assets-cli / --asset-download 保持默认 true。
- 验证：--asset-verify mt.opus.en-zh 正常模式只输出 [verify] … -> ok；STUDIOUS_DEV_MANIFEST=1 时多一行告警；--ui-shot 启动零告警、零 QML 报错；构建 [1/1] Linking。
- 踩坑：链接被占用 —— studious.exe（PID 15272）与 llama-server（PID 10692）还在跑，ld 报 collect2.exe: error: ld returned 1 exit status；问过用户后结束这两个进程才链上。
## 缺资产的引擎不可选 + 一句轻提示（2026-11-09）

- 诉求：资产没装齐时，主页会警告、截图与实时翻译根本选不了本地 LLM，但小窗还能选、点了到"开始"才报错。要求小窗跟另外两个一致；主页那项也不给选，点了要弹一句"没有对应资产"，带弹出与淡出动画。
- **Segmented**（rontend/qml/components/Segmented.qml）新增 property var blockedCodes 与 signal blocked(string code)：命中 blocked 的按钮标签 opacity 0.42，点它不发 selected、只发 locked（仍然可点 —— 用来解释为什么不行）。
- **翻译页**（rontend/qml/pages/TranslatePage.qml）：lockedEngines 从 ackend.engineStatuses 里筛 state === "missing-assets"（空串「自动」永远可选）；engineSeg 接上 lockedCodes 与红/黄/绿状态点，onBlocked → page.noticeRequested(page.blockedEngineHint(code))；文案两种 —— 「「%1」还差 %2 个组件，先去「设置 · 资产」里下载。」与「「%1」还差运行所需的组件，…」。
- **提示条**：新建 rontend/qml/components/Toast.qml（180ms 淡入 + 上浮 10px → 停 2.6s → 260ms 淡出，hidden() 信号）。主窗口只放一份（Main.qml 的 windowToast，锚在状态条上方 64px），页面用 signal noticeRequested(string) → onNoticeRequested: root.notify(text) 借这份；Main.qml 另加零参 openMini()，因为 --ui-shot 的 CALL 钩子调不了带参 JS 函数。
- **小窗**（rontend/qml/overlay/MiniWindow.qml）：engineChoices() 每项多带 usable（空串或非 missing-assets），菜单行 opacity: usable ? 1 : 0.42，TapHandler 对不可用项直接 return（不关菜单、不改选择）。
- 验证：把 settings/asset/root 指到空目录 → 主页「本地 LLM」「本地 MT」压暗 + 红点，Toast 显示「本地 MT还差 1 个组件…」（截图 doc/docs/ui-shots-qml/engine_blocked_toast.png）；小窗引擎菜单同样压暗（mini_engine_blocked.png，STUDIOUS_UI_SHOT=miniWindow）。恢复设置后四行状态正常、本地 MT 冒烟 你好,世界观!（命中 1/1）、--ui-shot exit 0（engines_ok_again.png）。
- 译文：update_translations 576 条（2 new）→ 补齐 → lrelease Generated 576 translation(s) (576 finished and 0 unfinished)；构建 [11/11]。
- 坑：取证用的 --toast-probe 探针写在 Component.onCompleted 里与页面原有那份撞车 → Property value set multiple times（qmlcachegen 直接失败），改成塞进原有的 handler 才过；取完已删。
## 引擎状态点四处同色（2026-11-09）

- 现象：本地 MT 在主页状态条是黄灯（装好了、但当前语言对走不通），可三个浮层的引擎菜单里是绿灯。
- 根因：后端 engineStatuses 每项同时给 state 与 eady；主页按「state=="ready" 且 eady 才绿」画，三个浮层只看 state，于是同一引擎两种颜色。
- 改法（三处 + 翻译页自己那份 badges）：统一成同一句判定 —— aw = state || (ready ? "ready" : "not-ready")，再 aw === "ready" && !ready → "not-ready"。
  - rontend/qml/overlay/CaptureOverlay.qml 的 engineStatusMap
  - rontend/qml/overlay/LiveControlBar.qml 的 engineOptions()
  - rontend/qml/overlay/MiniWindow.qml 的 engineChoices()（usable 仍只看 missing-assets）
  - rontend/qml/pages/TranslatePage.qml 的 adges
- 验证：doc/docs/ui-shots-qml/mini_engine_colors.png 小窗菜单里「本地 MT」变成黄点（原绿），「本地 LLM」仍压暗 + 红点；status_colors.png 主页状态条不变。构建 [15/15] Linking，无新增文案。
## 缺件提示按原因分类 + 云端 LLM 10 秒无响应判失败（2026-11-09）

- 现象：点到不能用的引擎只弹一句「还差 N 个组件」，缺运行时、缺模型、缺语言包、没填地址被说成同一件事。
- 后端：efreshEngineStatuses() 每条多给 locked（missing-assets / missing-config 都算）；云端 LLM 缺「服务地址」或「模型名」时 state 记 missing-config，界面按红点、不可选。
- 前端：TranslatePage.blockedEngineHint() 直接用后端 detail 拼「「引擎名」：……」，四类原因各自成句；CaptureOverlay / LiveControlBar / MiniWindow 的可用性判定同样认 missing-config。
- 取证钩子：--ui-shot 的 STUDIOUS_UI_CALL 原来只能可靠调窗口根上的方法（页内 Item 方法在元对象里找不到），在 Main.qml 加了一层 showBlockedEngineHint() 转发。
- 云端 LLM 超时：HttpRequest 加 irstByteTimeoutMs（LlmTranslator 固定 10 000 ms），HttpResponse 加 irstByteTimedOut / connectionFailed；QtHttpClient 用单独的计时器判「首字节」，收到响应头或数据就停表。
- 文案与重试：首字节超时 → 「云端 LLM 连接失败：10 秒内没有收到任何响应（地址 …）」；连不上（拒绝连接/域名解析失败）→ 「云端 LLM 连接失败：连不上 …」。这两类不再走 5 次重试，也不再在末尾劝人「换更大的模型」。
- 实测：静默 TCP 服务（只 listen 不回数据）→ 10.1 s 报首字节超时；无监听端口 → 4.2 s 报连不上（改前各要 20~40 s）；正常网关 --smoke → 你好，世界！
- 截图：doc/docs/ui-shots-qml/blocked_hint7.png（「云端 LLM」：缺 服务地址、模型名 · 去下面的「引擎配置」填）。
## 本地 LLM 卡里几行说明把卡片撑出列外（2026-11-09）

- 现象：窗口窄到 1000 逻辑宽（配置卡两列布局）时，「切换引擎时关闭 / 启动时预热 / 空闲自动停 / 空闲阈值」四行的说明文字不换行，整张卡被撑得比列宽还宽，右边的「高级设置」被顶到窗口外。
- 原因：FieldRow 内部是 RowLayout，说明 Text 只写了 Layout.fillWidth: true。Qt 里 Layout.minimumWidth 默认取 implicitWidth，长句子的隐式宽度就是整行文字宽，于是布局的最小宽度被它锁死，卡片只能长胖。
- 改法：rontend/qml/pages/ConfigPage.qml 这四处说明 Text 补 Layout.minimumWidth: 0（配 wrapMode: Text.WordWrap 就会按剩余宽度折行）。
- 验证：--ui-shot config_local_overflow.png --ui-page 3 --ui-size 1000x900 + STUDIOUS_UI_SET="engineId=llm-local"，四行说明都在卡片内折行，右侧不再溢出；随后把临时改过的 configTab 默认值改回 "llm-cloud" 并重建。
- 顺带发现：data/assets/models/mt 与 data/assets/runtime 现在是**空的**（语言包与 llama 运行时都不在了，3B/1.5B/0.5B 的 GGUF 还在 models/llm），所以界面显示本地 MT 缺语言包、本地 LLM 缺运行时。
### 更正：真正原因是少了一个右花括号（同日）

- 上面那条「说明文字 minimumWidth」只是次要问题。真正的病灶在 rontend/qml/pages/ConfigPage.qml 的「空闲阈值」那一行：FieldRow 少了收尾的 }，于是后面的整个「高级设置」SubSection 被解析成这一行 FieldRow 内部的子项 —— FieldRow 内部是 RowLayout，所以「高级设置」被排到了「分钟。只在…」的右边，整张卡于是横向边长，看起来像"内容被顶到卡外"。
- 定位手段：qmlformat.exe（Qt 自带）格式化一份副本，看真实缩进 —— SubSection 缩进只有 20，而它被写在了 24 缩进的 FieldRow 里面，说明括号层级本来就是错的；再逐个试删尾部多余的 }，用 qmlformat 的解析结果确认哪一种组合能过。
- 改法：在「分钟。只在…」那段 Text 之后补上 FieldRow 的 }，并删掉尾部那个原本用来收 FieldRow 的错位 }（原 2419 行，缩进 16）。文件其余部分没动。
- 验证：qmlformat 解析通过；构建 [9/9]；--ui-shot config_local_fix3.png --ui-page 3 --ui-size 1000x900（STUDIOUS_UI_SET="engineId=llm-local"）—— 「高级设置」回到卡内、位于「空闲阈值」下方，上下文/线程数两行完整可见，右侧不再溢出。
## 去重第一批：模型短名 / 步进器 / 关闭按钮 / 引擎顺序（2026-11-09）

三路子代理只读审计（前端 C++、QML、后端）各出一份重复清单。本轮先落"零行为变化"的那几条：

- **模型路径与短名收口到 LocalEngine**：新增 static QString pathForName(const QString &)（空串原样返回空，不落到"第一个模型"），esolveModelPath() 改成先走它再兜底；AppBackend.cpp 与 QuickTranslate.cpp 里各一份 localModelPathFrom / shortModelName 全删，只留一行转发。以前三份后缀表不一致（一份认 .q4_km、一份不认，一份会顺手去掉 -instruct），界面短名以 LocalEngine::shortModelName 为准。
- **component NumStepper 从 OverlayPage.qml 搬到 components/NumStepper.qml**：9 个调用点写法不变（同名类型 + 同样的 rom/to/step/value/unit/modified），新增 alueWidth 属性留出宽度调节口。页内副本删掉，注释改成"只有一份"。
- **component CloseButton 同理搬到 components/CloseButton.qml**：CaptureOverlay / MiniWindow / CaptureSettingsPanel 三处各写一份的 ✕ 合并，差异（uttonRadius、nimated）做成属性；实时控制条那份本来圆角与动画就不同，暂不替换，等下一批连菜单外壳一起动。
- **命令行自动顺序不再自留一份**：main.cpp 的 esolveEngineId() 用 engine::defaultPriority()，再把 llm-local 摘掉并注明原因（--engine auto 是脚本/冒烟用的，不该顺手拉起 1~3 GB 的本地模型）。
- 顺手清了两处过期注释：AppSettings.h/.cpp 与 LogSink.h 里还写着默认路径是 %APPDATA%\studious\assets。

验证：构建 [87/87] Linking（exe 60.8 MB）；--smoke "Hello, world!" --from en --to zh --engine llm-cloud → 你好，世界！[DONE] exit 0；--ui-shot 拍第 0/2/4 页零 ReferenceError / Unable / Syntax / is not a type，浮层页三组步进与译文不透明度都正常。

下一批（审计已列出，按风险排）：FieldRow 加 hint 槽 + 干掉 18 处手写提示与 6 处 132 + Theme.s3；Theme 加 engineStateColor/engineUsable/canonicalEngineId 收口 6 份状态映射；MenuPanel/MenuRow 合 5 处窗内下拉；缓存单例与 splitParagraphs 合并；后端 LocalMt 的 planFor/supports/pivotsThroughEnglish/resolve 四份路由判定收成一份。
## 重复接口收口（2026-11-09）

- 三路只读审计（前端 C++ 16 组 / QML 12 组 / 后端 14 组）后，本轮落地三批：
- 前端基础层：`Theme` 新增 `formLabelWidth` / `engineIds` / `canonicalEngineId` / `engineStateOf` / `engineUsable` / `engineStateColor` / `engineShortName`；新组件 `MenuPanel`、`MenuRow`、`NumStepper`（默认 34 高 + `compact: true` 22 高）、`CloseButton`。
- 浮层三处引擎下拉（截图浮层 / 实时控制条 / 小窗）改吃 `MenuPanel + MenuRow`，状态点颜色与置灰判断全部走 `Theme`；`ConfigPage` / `TranslatePage` 的状态色同样改成 `Theme.engineStateColor`。
- 前端 C++：`Paragraphs::split`（唯一段落切分）、`CacheHandle::sharedCache`（唯一缓存单例）、`langs::normalizeTargets / normalizeSource` 全面接管；`LocalEngine::pathForName / shortModelName` 成为模型路径唯一实现。
- 后端：`LocalMtTranslator` 的 `supports / pivotsThroughEnglish / resolve` 全部由 `planFor` 派生；`LlmRequest.{h,cpp}` 合并 `LlmTranslator` 与 `LlmCatalog` 的 endpoint / Authorization / 错误文案表。
- 验证：构建 `[105/105]`；`--ui-shot` 六页零 QML 报错；`--smoke --engine llm-cloud` → 你好，世界！
- 已知环境问题：`data/assets/models/mt` 与 `data/assets/runtime` 为空（早前把 assetRoot 指到空目录的模拟测试后没恢复），所以 `--engine local-mt` 报缺语言包；需要跑 `--asset-download` 重下。

## 去重收尾：Hint 组件 + 资产已装判定收口（2026-11-09）

- QML 说明行：ConfigPage 与 OverlayPage 各加一个页内 `component Hint: Text`（左缩进 `Theme.formLabelWidth + Theme.s3`、tiny、`inkVariant`、自动换行），把两页 13 处手写六行块收成 `Hint { text: ... }`。现在两页里 `formLabelWidth + Theme.s3` 只剩组件定义那一处。
- 教训：想用一条正则跨 100 KB QML 抓多行块（`(?s)` + lazy）会灾难性回溯，120 s 不返回；改成逐块 `edit`。多行 QML 一律手动改。
- 后端「已安装/可用」判定收口（子代理 `59c3e0d5` 交付）：`AssetRegistry` 新增 `isPresent(id)` 与私有 `fileLooksInstalled(QFileInfo)`（`isFile && size>0`）；**目录型资产补上这条判据**——以前只看 `exists()`，0 字节文件也算已装，与 `verifyInstalled()` 打脸；缺文件与空文件现在分开报（「缺少关键文件」/「关键文件为空或不是普通文件」）。`OcrEngine` 三件套统一 `isFile && size>0`，0 字节单独报「OCR 模型文件是空的」。`LocalEngine::availableModels()` 改吃 `AssetRegistry::listStatus()`（清单 `type==model` + `isPresent` + 位于 `models/llm`），删掉后缀白名单与 `ocr.` 前缀特判。
- 两处故意收紧：目录型资产的 0 字节占位文件由「已安装」变「损坏」；`LocalEngine::ensureStarted()` 对显式模型路径也拦 0 字节（`LocalEngine.cpp` 里那个 `if` 可单独回退）。
- 验证：构建 `[30/30] Linking`（backend 改动一并编入）；`qmlformat` 解析 Theme / FieldRow / ConfigPage / OverlayPage 全通过；`--smoke --engine llm-cloud` → 你好，世界！`[DONE] exit 0`；`--ui-shot` 第 2/3 页零 QML 报错，产物 `doc/docs/ui-shots-qml/dedupe3_p2.png` / `dedupe3_p3.png`。
- 仍待办：窗内语言下拉（小窗那份带多选勾选框，不能直接换 `MenuRow`）、浮层页与齿轮面板双套 UI 的文案漂移、`AppBackend::configuredModelExists` 仍只判 `isFile()`。
## 小窗目标语言改单选 + 浮层设置 UI 合一（2026-11-09）

- 小窗（`MiniWindow.qml`）：目标语言菜单去掉多选勾选框，点一项就是换到它并收起；`toggleDst()` 只写 `[code]`，`dstSummary()` 只显示第一个。`MiniSession::setDstLangs()` 再夹一次（多于一个只留第一个），避免别处写多值后冒出点不掉的"语言 +1"标签。
- 浮层设置合一：新建 `frontend/qml/overlay/OverlaySettingsGroups.qml`（`mode: mini|capture|live`），把主界面「浮层」页三组控件整段搬进去；`OverlayPage.qml` 与 `CaptureSettingsPanel.qml`（齿轮面板）都改用这一个实例，标签与后端绑定只剩一份 —— 之前两边各写一遍、文案已经漂移。
- 拆分时的坑：子代理只做了一半就被我打断接手，`OverlayPage.qml` 残留半截旧控件（`label:` 悬空、`H3` 断行）导致 `qmlcachegen` 报 `Incomplete binding`；按大括号配平逐段删干净、补上 `SectionCard` 的收尾 `}` 才通过。以后这种大段搬迁必须"先整体删、再插入"，不能边挪边删。
- 因为控件换了文件上下文，`lupdate` 把 25 条文案算作新增；用「旧 ts 里同 source 的译文」批量回填（脚本按 `<message>` 块处理，不跨块正则），`unfinished = 0`，`lrelease` 557 条全译。
- 验证：构建 `[9/9] Linking`；`--smoke --engine llm-cloud` → 你好，世界！；`--ui-shot` 浮层页（`unified_overlay.png`）与小窗齿轮面板（`unified_mini_panel.png`，用 `STUDIOUS_UI_TARGET=miniWindow STUDIOUS_UI_CALL="miniWindow:show@600|miniWindow:openSettingsPanel@1100"`）均零 QML 报错。
## 小窗语言下拉换 MenuRow + 记录详情加复制（2026-11-09）

- 小窗语言菜单不再自己搭外壳：`Rectangle + Flickable + Column + Repeater` 换成 `MenuPanel + MenuRow`（与引擎菜单同一套），目标语言那列勾选框随之消失；`selected` 由 `srcLang` / `dstLangs[0]` 决定，选中项由 `MenuRow` 统一加粗变色。
- 记录详情头部加「复制原文」「复制翻译」两个小按钮（`backend.copyText()`）；没有译文时「复制翻译」退回原文，两边都空时按钮变灰不响应。
- `AppBackend::configuredModelExists` 补上 `size() > 0`：资产层已经把 0 字节判成损坏，这里若只判 `isFile()` 会出现「配置页说就绪、一点翻译就报缺模型」。
- 取证钩子：`MiniWindow` 加零参 `openDstMenu()` / `selectFirstRecord()`（`--ui-shot` 的 CALL 只认零参方法，`STUDIOUS_UI_SET` 只打 backend）。
- 验证：构建 `[9/9] Linking`；`--smoke` → 你好，世界！；`--ui-shot` 出 `mini_lang_menu.png`（目标语言菜单，单选高亮）与 `mini_records_copy.png`（记录详情 + 两个复制按钮），零 QML 报错；文案 2 条新增补齐，`lrelease` 559 条 / 0 unfinished。
## 小窗三处修正（2026-11-09）

- 目标语言输入框右侧的「多选 / 单选」徽章：改成 `visible: backend.dstMultiSupported` —— 支持多选时才出现，本地 MT 这种单选场景不再露半个"单选"字样（截图里那个「多选」也一并消失）。
- 语言菜单位置：`srcField.x/y` 是**父容器坐标**，菜单挂在窗口根上，直接当锚点会飘到左上角压住标签栏。改成打开时 `mapToItem(win.contentItem, 0, 0)` 换算一次存进 `langMenuX/Y`（写在绑定里不会有依赖，这是第二次踩），宽度取字段实际宽度。实测 `kind=2 x=286 y=62`、右半区差 75,466 px / 左半区仅 7,645 px，菜单确实移到了目标字段下方。
- 记录详情两个复制按钮：加按下反馈（`scale 0.94` + `opacity 0.65` + 120 ms 回弹、手型光标），复制结果走 `Toast`（与主界面同一组件、底部居中）。`AppBackend::copyText` 返回值从 `void` 改成 `bool`，空文本/无剪贴板返回 false，界面据此提示「已复制」或「复制失败：剪贴板不可用」。
- 验证：构建 `[9/9] Linking`；`--smoke` → 你好，世界！；`--ui-shot` 出 `mini_lang_menu3.png`（菜单在目标字段下方）、`mini_records_copy2.png`（两个复制按钮）、`mini_copy_toast.png`（底部提示条，底部 840×197 区域变化 4,338 px）；零 QML 报错；文案 2 条补齐，`lrelease` 560 条 / 0 unfinished。
## 目标语言规则收口：没有多选、没有自动、不许和源语言重复（2026-11-09）

- 规则进 langs：
ormalizeTargets() 直接丢掉「自动」（目标语言不能是自动检测）；新增 
ormalizeTargetsAgainstSource(targets, source)——源不是 auto 时把同一个语言剔掉，剔空后按 {zh, en} 取第一个不等于源语言的兜底（源是 zh 就给 en）。
- 后端：AppBackend 删掉 dstMultiSupported，新增只读 	argetLanguages（languages 去掉 auto）；setSrcLang 改完源语言立刻修目标语言并落盘；setDstLangs / setEngineId / 构造 / eloadLanguagesFromSettings 全部走「空兜底 + local-mt 单选 + 不许与源重复」。
- 小窗：MiniWindow 删掉目标语言旁的「多选」小胶囊；语言菜单的 model 按 openMenu 切 languages / 	argetLanguages，与另一端相同的语言压暗且点不动（clash）。
- 翻译页：	argetLanguages 也剔掉当前源语言。
- CLI：--to 归一再发单，被改时打 [WARN] 目标语言已从 X 调整为 Y（目标语言不能是自动检测 / 不能和源语言相同）；header 与 yLang 取值改用归一后的目标语言（改前 --from zh --to zh 会拿着 zh 去取 en 的结果，译文看着是空的）。
- 验证：--to auto → 调整为 zh，译文「你好，世界！」；--from zh --to zh → 调整为 en，译文「Hello, world!」；小窗截图 doc/docs/ui-shots-qml/mini_dst_clash.png（中文(简体) 压暗、无「多选」、列表里没有自动检测）。

## 引擎配置卡片结构修复（2026-11-09）

用户报「配置里引擎配置的显示数据都乱了 + 深色显示也有问题」。看截图以为是数据错位，实际是 **ColumnLayout 的父子关系坏了**：本地 MT 面板被解析进本地 LLM 面板里面，本地 LLM 的「已停止 / 日志」那块又漏在卡片层，所以 MT tab 顶上出现 LLM 的状态行、且整块没有卡片内边距。

- 定位手段：`{`/`}` 计数脚本（末尾 depth=1，少一个右花括号）→ `qmlformat` 报 `Expected token '}'` → 把文件复制到临时目录、补一个 `}` 再 `qmlformat`，看它规范化后的**缩进**，一眼能看出谁是谁的兄弟（`mtPanel` 与 `localPanel` 同缩进才是对的）。
- 修法：`ConfigPage.qml` 里 `localPanel` 的收尾 `}` 补在「本地 MT」注释之前，同时删掉此前试探时在文件末尾多补的那个 `}`；两处平衡后 `qmlformat` 通过、构建 `[9/9]`。
- 中途踩的坑（记下来别再犯）：用 `Get-Content` 读、`WriteAllLines` 写，在 PowerShell 里按 ANSI 解码 UTF-8，**整份中文变乱码**（`本地 MT` → `鏈湴 MT`），且无法完整反转（有 `?` 替换）。恢复办法：用之前 `qmlformat` 生成的临时副本（内容完好）覆盖回来——那次是运气好，以后改源码只用 `edit` 或 `[System.IO.File]::ReadAllText/WriteAllText`。
- 顺手清理：`qmlformat` 为了规范化插了大量空行（2230 → 4549 行），去掉空行回到 2080 行；`ConfigPage` 实例加 `objectName: "configPage"`（`--ui-shot` 的 CALL 钩子能直接调页面函数）。
- 验证（都用 `ocrTest` 读截图里的文字坐标，不靠肉眼）：浅色 LLM tab 顺序为 模型文件 → 可用的加速组件 → 优先 GPU → 切换引擎时关闭 → 启动时预热 → 空闲自动停 → 空闲阈值 → 高级设置 → 回复风格 → 状态行 → 日志；MT tab 只有 MT 自己的内容；深色两边同样正确。截图 `cfg_llm_fix2.png` / `cfg_mt_fix2.png` / `cfg_llm_final.png` / `cfg_dark_llm.png` / `cfg_dark_mt.png`。
### 补充：本地 MT 面板仍在卡片外（2026-11-09）

上一轮只把「MT 面板在 LLM 面板里面」拆开了，但拆的时候多留下了一个右花括号：localPanel 收尾后紧跟着又一个 }，把 CollapsibleCard 的内容区提前关掉，于是 mtPanel 变成了页面级兄弟 —— 表现就是「引擎配置」卡片在分段控件下面直接结束，MT 的整块内容画在页面背景上、右端还会溢出到窗口边缘。

- 定位：绝对深度脚本在关键行打分 —— CollapsibleCard 打开后 depth=4，webPanel/cloudPanel/localPanel 都在 5，唯独 mtPanel 是 4，一眼看出它在卡外。
- 修法：删掉 localPanel 之后多出来的那个 }（原 1828 行），并在文件末尾补一个 } 补齐 CollapsibleCard 的收尾；qmlformat 通过、构建 [9/9]。
- 验证（浅色 + 深色都拍）：cfg_llm_v2.png（模型文件 → 可用加速组件 → 优先 GPU → 切换引擎时关闭 → 启动时预热 → 空闲自动停 → 空闲阈值 → 高级设置 → 回复风格 → 状态行 → 日志）、cfg_mt_v2.png / cfg_mt_v3.png（按顺序使用… → 语言对表 → 保存位置 → 开机自启 → 空闲释放，全部在卡片内边距里）、cfg_mt_dark_v2.png。第 0 页复拍零 QML 警告。
### 深色主题说明文字看不清（2026-11-09）

深色主题下配置页 MT 面板的说明行、表格空态行、右侧「未加载 / 到点释放内存」几乎是黑的。采样用户截图：文字像素 (50,46,65)、背景 (23,22,30) —— 不是对比度不够，是这些字根本没跟主题走。

- 根因：ConfigPage.qml 里有一批 Text {} **没写 color:**，走 Qt 默认色（QPalette / 控件默认），在本机拿到的是浅色那套的深字；Hint 组件虽然写了 color: Theme.inkVariant，外层又套了 opacity: 0.45（MT 未装语言包 / 未开预热时的"不可用"淡化），叠加后彻底不可读。
- 修法：① component Hint 改 color: Theme.dark ? "#FFFFFF" : Theme.inkVariant；② 删掉两处 Hint 上的 opacity: … ? 1.0 : 0.45（说明文字不该跟"能不能用"一起淡出，控件级的 enabled 淡化保留）；③ MT 面板三段无色说明补显式色；④ 脚本按花括号配对找出 ConfigPage.qml 里 **34 处**无色 Text，逐块插入 color: Theme.ink（SettingsPage / OverlayPage / AppearancePage 同类扫描为 0）。
- 验证：深色 cfg_mt_dark_v6.png、浅色 cfg_mt_light_v6.png（都 1120×1200）—— 说明行变白、浅色仍是深字；构建 [9/9] Linking；qmlformat 通过。
- 教训：QML 里凡是正文 Text 都显式给色，别依赖 QPalette；透明度只用来表达"禁用"，别拿来压说明文字。
## DSH 环境修：图片卸载在 rewind 标记上崩（2026-11-10）

本轮「Cannot read properties of null (reading 'content')」不是本项目代码，是 DSH 自身的图片卸载插件。
链路（会话 `session-9cbd45d0` 第 244 轮 step 18）：请求图片 base64 超 20 MB → pi-ai 抛
`IMAGE_OFFLOAD_REQUIRED`（要求卸载最旧 7 处）→ `dsh-compaction-image-offload` 的
`offloadOldestImages()` 遍历 surface → 撞上 seq 40521 的 `rewind-mw86495d-5` 回滚标记 →
`deriveEventMessage()` 对 `rewind-*` 返回 `null`（`dsh-session` 里 `[dsh-purge]` 打的那行）→
`visit(message.content)` 抛 TypeError，把真正的错误盖掉，整轮失败；重试同因再挂一次。

- 定位手法：解压 `~/.dsh/sessions/.../session.v4.jsonl.zstd`（Node 24 的 `zlib.zstdDecompressSync`），
  按 `seq` 找 `turn/end` 的 error，再看它前一条 `assistant/attempt` 的 `finish` 失败原因；扫描全量
  `tool/result` 的 `message` 无 null、`user/message` 里只有一条 `rewind-*`，正好是崩溃点。
- 修法（`@deepseek-ai/dsh-compaction-image-offload/lib/{index.js,types/image-offload.js}`）：
  `deriveEventMessage()` 结果为空或 `content` 不是数组就 `continue`（回滚标记本来没有图片）；
  `types/projection.js` 同步改成具名报错，别再读 `.content` of null。三处都留 `[image-offload-null-fix]` 注释，
  原件备份为同名 `.bak-imagefix`。
- 验证：把备份拷成 `.mjs` 跑同一组假数据 → 旧码复现 `TypeError: Cannot read properties of null (reading 'content')`；
  新码返回 `true` 并 `append("image/offload", {targets:[{seq:2,imageIndexes:[0,1]}]})`；`lib/index.js` 动态 import 正常。
- 同类排查：`dsh-compaction-basic` 已 `.filter(null!==)`，`tool-result-pruner` 只取 `tool/result`，
  `token-meter` 有 `message === null ? 0`，只有图片卸载这一处没防。
- 教训：surface 节点里混着 `rewind-*` 这种「合法但推导结果为 null」的标记；凡是对
  `deriveEventMessage()` 结果直接读字段的插件，都要先判空。
- 生效：DSH 桌面端要重启（模块已常驻内存）；重启后在那条会话继续发一条，卸载逻辑会自动放掉最旧图片再重发。

## 发版 1.1.1（2026-11-10）

- ersion.json 1.1.0 → 1.1.1；README 状态行与「版本信息」同步，新增 1.1.1 变更条目。
- 内容：内存/资源修复批（截图临时 PNG 与常驻整屏位图、MT 单例跨线程竞态、语言检测表懒初始化竞态、
  HTTP 客户端 parent 误用、未初始化捕获矩形、MtModel 张量形状校验）、只装 Vulkan 时锁定「优先 GPU」
  并把已安装加速组件标绿、启动开场动画（logo → 进度条 → 主界面弹出）。
- 提交 elease: 1.1.1 修复bug，打标签 1.1.1。
## 本地 LLM 自定义导入（2026-11-10）

- 需求：把自己的 .gguf 导进来就能用；**复制，不移动**原文件。
- `frontend\src\LocalEngine.{h,cpp}`：新增 `importModelFile()` / `importedModelNames()`。
  校验只有两级——文件 ≥1 MB + 头 4 字节是 `GGUF`；再往下就等于自己重写一遍 llama.cpp 的加载器，
  而且民间量化版本来就和清单 sha256 对不上，硬校验等于禁止导入。
  落盘用 `QFile::copy`（复制），登记写 `data\assets\state\imported-llm.json`（`QSaveFile` 原子提交）。
  `availableModels()` 在清单之外再并一轮「登记过且文件还在」的名字，仍然不从目录里瞎猜。
- `frontend\src\AppBackend.{h,cpp}`：`Q_INVOKABLE importLocalModel(QUrl)`，复制丢进 `QtConcurrent`，
  结果经 `statusText` / `errorRaised` 回传；导入成功顺手 `setLocalModel()` 选中它；
  `m_modelImporting` 挡住连点两次复制出两份。`localModels` 属性从 `CONSTANT` 改成 `NOTIFY localStateChanged`
  （否则下拉框不会刷新——这是"导入完看不到"的经典坑）。
- `frontend\qml\pages\ConfigPage.qml`：加 `import QtQuick.Dialogs`，`浏览…` 打开 `FileDialog`
  （`GGUF 模型 (*.gguf)` / `所有文件 (*)`），`onAccepted: backend.importLocalModel(selectedFile)`。
- `frontend\src\main.cpp`：新增 `--import-model <模型.gguf>`，和界面同一套逻辑，方便脚本验收。
- 翻译：lupdate 3 条新增（`选择 GGUF 模型文件` / `GGUF 模型 (*.gguf)` / `所有文件 (*)`），
  en 与 zh_CN 都补齐，lrelease 输出 563 finished / 0 unfinished。
- 验证（真跑）：
  - 下载 `https://hf-mirror.com/bartowski/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf`
    → `C:\Users\hua\Downloads\Qwen2.5-0.5B-Instruct-Q4_K_M.gguf`（379.4 MB）。huggingface.co 在本机连不上（443 超时），
    hf-mirror.com / modelscope.cn 可通。
  - `studious.exe --import-model <该文件>` → `[import] ok -> Qwen2.5-0.5B-Instruct-Q4_K_M.gguf`，
    下拉列表里出现；源文件仍在（`Test-Path` = True，源/目标同为 397808192 字节）。
  - 重复导入 → 报「同名文件已存在」；塞一个 2 MB 假 .gguf → 报「文件头不是 GGUF」；不存在的路径 → 报「找不到」。
  - 端到端：`--smoke "Hello, how are you today?" --to zh --engine llm-local --local-model Qwen2.5-0.5B-Instruct-Q4_K_M.gguf --cache off`
    → vulkan 后端 1529 ms 拉起 llama-server（pid 有值、端口 18080），译文「你好，<<<」（0.5B 小模型的水平，链路是通的）。
  - 界面：`--ui-page 3 --ui-shot` 拍到「模型文件」下拉显示 `Qwen2.5-0.5B-Instruct-Q4_K_M.gguf`、右侧「浏览…」按钮在位；
    `QtQuick.Dialogs` 模块解析正常（页面能加载出来就说明插件在）。
- 坑：改成 GUI 子系统后，PowerShell 的 `& exe args` 不再同步等待，CLI 验收必须
  `Start-Process -Wait -RedirectStandardOutput`，否则读到的是空文件。
## 资产页：驱动 / 模型分组 + 移除 7B 占位（2026-11-10）

- `frontend\qml\pages\SettingsPage.qml`：新增 `assetRows`（引擎驱动 runtime 排在前、模型跟在后，第一行模型带 `dividerBefore`），表格 delegate 改成 `ColumnLayout` + 分割线；`importTargetFor` 里 7B 的举例注释同步去掉。
- `backend\asset\manifest.json`：删掉 `model.qwen2.5-7b-instruct.q4km`（原 `urls: []`，只用来接「导入文件」）；清单 16 → 15 条。
- `data\assets\state\asset-origins.json`：删掉指向该条目、已经失效的 `"import"` 记录。
- 验证：Python 解析清单 = 15 条、无 7b；`cmake --build frontend\build` 通过；真机点「设置 → 资产」和「LLM」标签截图确认分割线与条目顺序；新日志段无 qml 报错。
## 资产页：用户导入条目内嵌进分类表格（2026-11-10）

- 用户自己导入的模型与清单资产彻底分家：GGUF 落 `data/assets/models/llm-user/`，
  语言包落 `data/assets/models/mt-user/mt.opus.<src>-<dst>/`（六件套齐全才算一条）。
  这样清单资产的 sha256 校验永远只作用于线上那份，用户文件不参与。
- `LocalEngine`：新增 `userLlmModelsDir()` / `userModelFiles()` / `removeUserModel()`；
  `importModelFile()` 的目标目录从 `models/llm` 改到 `models/llm-user`；
  `availableModels()` 收录用户目录里的文件；`pathForName()` 先查用户目录再回落清单目录。
- `AppBackend::userModels()` 汇总 LLM 与 MT 的用户条目（MT 用 `LocalMtTranslator::requiredFiles()`
  判齐），`removeUserModel()` 分别处理单文件删除与语言包目录递归删除。
- 资产页不再单开「用户导入」标签（第一版这样做，用户否了）：改成在「LLM」「本地 MT」两个分类的
  清单条目下面加一条分割线，接用户导入的行（徽章「用户导入」、操作「删除」）；一条都没有时
  整段连分割线一起隐藏。「全部」栏混排说不清归属，不显示用户行。
- 验证：`--import-model` 导入 Qwen2.5-0.5B 改名版落在 llm-user 并出现在 LLM 栏；
  造 `mt.opus.en-test` 六件套目录后出现在 MT 栏，确认弹窗删掉后目录消失、分割线一起消失。
## 资产页导入：用户模型不再套清单哈希（2026-11-10）

- 现象：资产页「从文件导入」选自己的 gguf，仍报「导入文件与清单哈希不一致：实际 …，期望 …，未做任何安装」。
- 根因：`beginImport("llm")` 会先算 `importTargetFor("llm")`；三条模型都装好后它回退到清单第一条（0.5b），
  于是导入走的是 `importAssetFromFile` → `Downloader::installFromFile` 的清单 sha256 校验，用户文件必然对不上。
- 改法：
  - `SettingsPage.qml` 的 `beginImport(category)`：`llm` → 直接 `openUserModelDialog()`
    （新 `FileDialog{id:userModelFileDialog}`，标题「选择 GGUF 模型文件」，过滤器只留 gguf），
    `onAccepted` 交给 `backend.importLocalModel()`；`mt` → `backend.notifyMtImportUnavailable()` 说明六件套目录；
    其余类别（图像识别等）保持原来的「清单条目 + 哈希」路径。
  - 行内「导入文件」对 `type === "model"` 的占位条目也改走 `openUserModelDialog()`。
  - `AppBackend::notifyMtImportUnavailable()`：状态栏提示把六件套目录放到 `models/mt-user/`。
- 验证：LLM 栏点「从文件导入」弹的是「选择 GGUF 模型文件」（GGUF 过滤器），不再是「选择要装进 model.qwen2.5-0.5b…」；
  走完文件框后 `Qwen2.5-0.5B-user2-Q4_K_M.gguf`（379.4 MB）落进 `models/llm-user/`，
  资产页底部出现「用户导入」行（分割线之上是清单条目），`--import-model` CLI 同样正常。
- 测试期间在 `C:\Users\hua\Downloads` 造的副本已删除，用户自己的 `Qwen2.5-0.5B-test-Instruct-Q4_K_M.gguf` 未动。
## 弹窗布局：长文本不再把按钮顶出弹窗（2026-11-10）

- 现象：资产页「删除用户模型」确认弹窗里，文件名一长（`Qwen2.5-0.5B-user2-Q4_K_M.gguf` 这种），
  正文不换行、整段往右顶，`取消`/`删除` 两个按钮被挤出弹窗右边界。
- 根因：`Popup { width: 460 }` 只定了外框宽度，内部 `ColumnLayout` 没有宽度约束，
  Qt 会按子项 `implicitWidth`（一整行不换行的文本宽度）撑开布局，`Layout.fillWidth` 只在这个
  被撑开的宽度里分配，于是按钮跟着一起跑出去。
- 改法：所有 `Popup` 的内容 `ColumnLayout` 显式写 `width: <popupId>.availableWidth`
  （SettingsPage.qml 里 7 处：uninstallConfirm / userModelRemoveConfirm / importCategoryPicker /
  importPicker / resetConfirm / licensePopup / thirdPartyPopup；后三个原本已有 contentWidth）。
  标题那行同时加 `Layout.fillWidth: true` + `wrapMode: Text.Wrap`。
- 验证：拿 64 字符的假模型名 `Qwen2.5-14B-Instruct-AWQ-Longname-Layout-Stress-Test-Q4_K_M.gguf`
  做压力测试 —— 文件名折成两行，按钮完整留在弹窗内（截图 `%TEMP%\d-confirm4-crop.png`）。
- 顺手修英文挤压：`暂不支持` 的英文译文 `Not supported yet` 比列宽还长，会压到「路径」列上，
  改成 `Unavailable`；lrelease 重新生成 569 条。
## 数据页目录树同步（2026-11-10）

「设置 · 数据」里那棵树只画了旧的六行，`models/` 下新加的 `llm-user/`、`mt-user/`
和 `state/` 都没露脸，用户看不出自定义模型该放哪儿。补齐并同步 `data/README.md`。

- `frontend/qml/pages/SettingsPage.qml` 的 `dirTree`：补 `models/llm-user/`（只验文件头）、
  `models/mt-user/`（六件套齐全才认）、`models/ocr/run/`、`runtime/runtime.llama.cpu.win-x64/`、
  `state/`（导入记录与资产来源）；说明列对齐到第 35 列，中英都排得下。
- 顺手清掉 3 条「有译文但带 `type="unfinished"`」的文件框串（`选择 GGUF 模型文件` /
  `GGUF 模型 (*.gguf)` / `所有文件 (*)`）—— 这种条目 lrelease 仍当未翻译，英文界面会掉回中文。
  用 `%TEMP%\clear_unfinished.py` 去标记，lrelease 变成 576 finished / 0 unfinished。
- 验证：`studious.exe --ui-shot ... --ui-page 4 --ui-section 5` 中英各拍一张
  （`tree-zh-crop.png` / `tree-en-crop.png`），树对齐、无截断、无 qml 报错。
- `data/README.md`：目录树加 `llm-user/`、`mt-user/`、`imported-llm.json`；「规则」小节
  改写为「清单资产每次启动核对 sha256，用户目录不参与核对，两边互不影响」。


## 回滚 MT 转换器 + 禁用「本地 MT」导入（2026-11-10）

- 用户判断「原始 opus-mt 包 → 本程序格式」的转换器工程量太大、收益不高，决定只支持
  多语言语言包（en-mul / mul-en 已经在清单里），把导入入口先关掉。
- 回滚范围：转换器原型只在 %TEMP%（torchbin.py / mtgraph.py / mkpack.py /
  validate_conv.py + 约 730 MB 的原始包与产物），仓库代码一行未动；
  %APPDATA%\studious\assets\models\mt.opus.en-zh 下我造的转换产物已删，
  仓库自带的 data\assets\models\mt\mt.opus.en-zh 完好（8 个文件未变）。
- 界面改动（frontend/qml/pages/SettingsPage.qml）：
  - 「从文件导入」类别选择弹窗里「本地 MT」按钮置灰，提示改成
    「暂不支持从文件导入：语言包请在资产页下载」。
  - 资产页「本地 MT」分类栏的「从文件导入」按钮同样置灰
    （buttonEnabled: page.assetTab !== "mt"）。
  - 翻译：lupdate 577 条（1 新增），studious_en.ts 填
    "Importing is not supported yet: download language packs from the assets page"。
- 保留的调研结论（下次要做时不用重来）：官方 encoder 图的真实结构是
  x = emb*sqrt(d) + pos → attn(x) → x = x + attn → LN → ffn → x = LN(x + ffn)；
  最初漏了 FFN 之后那次残差，导致 encoder 余弦 0.995 但译文是垃圾。

### 实时翻译控制条「刷新」：强制重翻 + 点击回执（2026-11-10）

- 反馈：点了刷新但同屏文字没变时，浮层逐字不动、状态栏那行一闪就没了，看起来像没生效。
  给 `liveRefreshButton` 加 `primary: flash` + 300 ms `Timer`，点下去主色实心闪一下。
- 强制重翻：`LiveSession` 加 `m_forceNextFrame`，`refresh()` 置位，
  `acceptOcrLines()` 的「画面未变」闸改成 `!forced && liveOnlyOnChange && …`，
  空帧分支就地清掉（没字可翻，别把标志带到下一帧）。
- 踩坑：GUI 子系统下 `--ui-shot` 的 `STUDIOUS_UI_CALL` 才生效；直接起 exe 不会跑走查钩子，
  `begin@60000` 之后的 `refresh` 也要给足等待时间，否则抓到的是「正在翻译」而不是结果。
- 验证：用户实测点击有回执；重编 15 步通过，日志无新增 QML 警告。
## 浮层工具条摆位修复（2026-11-10）

两个由用户报上来的摆位问题，都是 QML 里的定位逻辑，跟后端无关。

### 1. 选区几乎占满整屏时工具栏跑到屏幕外（截图翻译 + 实时翻译）
- 截图翻译：`CaptureOverlay.qml` 的工具栏只有"选区下方 / 选区上方"两条分支，
  上下都没有余量时算出负的 y，整条被窗口裁掉。
- 实时翻译：`LiveControlBar.qml` 虽然把位置 clamp 回桌面，但屏幕上下都贴着边时
  也只能硬塞在屏幕外沿。
- 修法：两条链都加第三条分支——`below` / `roomAbove` 都不成立时"塞进选区贴底"
  （`选区底边 - 工具条高度 - Theme.s3`，再 clamp 进桌面）。
- 侧带：控制条落进选区后必须把它排除出抓屏，否则下一帧 OCR 会把
  「暂停 / 刷新 / 设置」当原文再翻一遍。新增 `LiveSession::setControlBarOverlapping()`，
  只在状态变化时调 `setExcludeFromCapture()`（每改一次 DWM 要重算，会闪一帧）；
  拖动结束也结算一次，新一局开始重置回"可被拍到"，否则上次留下的空洞会跟着用户。

### 2. 点「暂停」时控制条被拽回初始位置
- 根因不是代码里显式复位：`visible: running || paused || errored` 三个都是从 phase
  派生的 bool，phase 从 Running 变 Paused 的瞬间 `running` 先变 false、`paused` 还没变 true，
  Qt 按这个中间态重算了一次 visible → 窗口瞬隐现；`onVisibleChanged(false)` 里正好
  在复位摆位状态，紧接着 `schedulePlace()` 又按"首次出现"摆回自动位置。
- 修法：`visible` 改成直接比 phase 字符串（消灭中间态）；摆位状态只在 phase 回到
  Idle 时复位，不再挂在 visible 上。
- 日志实证：修前 `visible=false phase=Paused placed=true` 紧跟 `visible=true phase=Paused
  placed=false` 再 `place -> 242,242`；修后一次 place，pause/resume 全程无 visible 抖动。

### 验证（真跑）
- 普通选区 `300,300,760,320`：控制条 `place -> 242,242 size=876x50` 只出现一次，
  pause/resume 无重摆；截图流程 `confirmSelection` 走完 9 行正常出译文。
- 近全屏 `0,0,1707,1060`：控制条 `place -> 823,969`（落在屏内）+ 日志
  `control bar excluded from capture (overlap=1)`；截图工具栏
  `inside=true y=1004`，整屏截图里能看到它贴在选区底部。
- 编译：`cmake --build frontend\build` 全过；临时插桩日志（`[barplace]`/`[ovbarplace]`）已清干净。
### 3. 工具栏被"框选框"盖住（2026-11-10 补）
- 截图翻译：`CaptureOverlay.qml` 的工具栏 `z` 从 5 提到 12，压在选框（z=3）、手柄/尺寸标签（z=4）之上；
  菜单是 20/21，仍在最上层。
- 实时翻译：真正压住它的是**译文浮层窗口**——原文底色（默认 70%）画在那个整块窗口上，
  控制条被摆进选区后就在它下面，看上去糊成一片。
  - 新增 `LiveSession::raiseControlWindow()`（`SetWindowPos(HWND_TOP)`，不激活、不动 WS_EX_NOACTIVATE），
    在 `placeWindows()` 摆完浮层后调用。
  - QML 侧 `LiveControlBar.qml` 在 `onVisibleChanged(visible)`、`placeNearRegion()` 末尾抬一次，
    并用 `Connections.onPhaseChanged → Qt.callLater(bar.raise)` 兜住"浮层随 phase 显隐又盖上来"。
- 验证：`EnumWindows` 按 z 序枚举该进程的可见窗口 ——
  控制条 `z=10 (501,969)-(1377,1019)`（在选区 0,0-1707,1060 之内）、译文浮层 `z=11 (0,0)-(1707,1060)`、
  主窗口 `z=35`，索引越小越靠上 → 控制条已压在浮层上面；pause 之后仍是 10 / 11。
  截图翻译整屏选区：工具栏「处理中… 本地 LLM 自动检测 → 英语 设置 重新框选」可见且不被框线遮住。
## 截图浮层：译文比原文长时的排版（2026-11-10）

问题：译文比原文长时，贴纸按原文框宽定尺寸 + 单行 + ElideRight，长句直接被省略
（例：「Based on your tech stack」→「基于你的技术栈…」），相邻块还会互相压字。

改法（只动 `frontend/qml/overlay/CaptureOverlay.qml`，块装饰器 + 一个 JS 辅助函数）：
1. 先向右吃空白：`limitW = min(原文宽*1.6, 选区右边界 - 块x)`，宽度取 `min(limitW, 实测单行宽+4)`。
2. 再等比缩字号：`fitPx = clamp(basePx * limitW / 单行宽, max(9, basePx*0.7), basePx)`。
3. 缩到底仍放不下就换行（`maximumLineCount: 2`），块高最多长到「正下方那一块的顶边」之前
   （`nextPlateTop()` 只统计横向有重叠的后续块），吃掉空隙的 80%，避免两块叠在一起。
4. 实在放不下才保留 elide 兜底。
尺寸测量用三个 `visible: false` 的 Text 做（Qt 照样排版），避免每帧去问 QFontMetrics。

踩坑：
- `Repeater` 的 `index` 在老代码里没声明，直接用会是 `ReferenceError: index is not defined`，
  而且不会中断渲染 —— 只是 `gapBelow` 静默退化成 Infinity，块高会算成 1.8 倍行高去压下一块。
  正解：delegate 里写 `required property int index`。
- 验证时别用 `--ui-shot` 结尾那张图（那时浮层已经收起），要在运行中另一路 `CopyFromScreen` 连拍。
- 压测用「目标语言 ru」：俄文比中文长得多，最容易压出换行/省略分支。
## 横排标签被并成一行（导航栏压字）2026-11-11

现象：GitHub 顶栏那种「一行里十几个互不相干的标签」，被 OCR 并成一行送进模型，
翻回来是一整句中文，落回浮层就是一个横跨全宽的长块，压在相邻标签上；行太长时
模型还会漏掉后半段（`Security and quality / Insights / Settings` 保留英文）。

三层改动（缺一层就还会犯）：

1. `backend/src/ocr/OcrEngine.cpp` `sliceAtGaps()`：按空白列切片时，除了「装得下
   （宽度 <= sliceW）」，还要求相邻词间隙 `<= 0.75 × 字高`，间隙再大就另起一片
   —— 一个检测框横跨两三个标签时，别再拼成一串。
2. 同文件 `recognizeBox()`：签名改成输出 `QVector<OcrWord>`，**每片一个词条**
   （自带矩形与置信度），不再把多片合成一个字符串。`recognize()` 直接收。
   以前拼成一串就丢了各片坐标，上层既没法按间隙分段，浮层也只能横跨。
3. `frontend/src/OcrLines.cpp` `groupWordsIntoLines()`：聚行之后再按横向间隙切段，
   判据 `间隙 > 0.9 × 词高`（下限 20px）。跟**单词**高度比，不能跟并出来的行高比
   —— 行高是并集，越并越大，阈值跟着虚胖就再也断不开了（第一版就是这么翻车的）。
4. `frontend/qml/overlay/CaptureOverlay.qml`：新增 `nextPlateLeft()`，译文块往右
   吃空白时只能长到同一行右边那一块的左边（以前只受选区右边界约束，会横跨过去）。

实测（`%TEMP%\nav-board.png` 1600×100 的 GitHub 顶栏，viewer 1:1 摊在屏幕左上，
`begin@1200|rect`）：改前 3~6 行（`问题 8拉取请求 代理` 粘在一起）；改后 7 行，
每个标签一段，浮层逐块贴回、互不重叠。回归：`gh.png` 正文页仍是 12 段、
中文设置页仍是整句（`三个浮层共用的观感在上半页；…` 没被切碎）。

已知限制：`田项目` 这种「图标 + 短词」仍会被并成一个词（图标把两片之间的空白
填掉了），模型偶尔会把它当句子翻（`田项目的中文译文是：田径项目`）—— 属于短标签
的提示词问题，另修。

## 默认提示词：界面短文字与专名（2026-11-11）

现象：截图里一排标签（Code / Issues / Pull requests）译出来是这样的 ——`<<<⏎行动⏎>>>`、
`田项目的中文译文是：田径项目`；中文界面翻中文时还会重试 5 次后弹一条"没有翻成"的告警。

改动（`backend/src/translators/LlmTranslator.cpp`）：

1. `defaultSystemPrompt()` 加三条硬规则：不加任何前后缀/引号/语言标注、数字与版本号与
   文件名与 URL 与品牌名原样、界面文字只给简短短语不扩写。
2. 新增 `looksLikeUiLabel()`（≤24 字且无句读）：命中的条目走独立模板"给出译名，
   不要解释、不要加引号或句号、不要重复原文"，并把 token 上限压到 96。
3. 标签不再套 `<<< >>>` 边界符号 —— 3B 模型会把标记抄回来。
4. 原文已是目标语言时直接原样返回（`i18n::detectLanguage` 置信度 ≥0.6），不再发请求、
   也不再算"软失败"。
5. `stripWrapper()` 剥掉残留的 `<<<` / `>>>` 和"翻译如下："/"X 的中文译文是："引导语。
6. 缓存键加 `kPromptRevision`（当前 4）：以后改模板必须 +1，否则旧结果会被继续命中
   （这次就是被 `<<<代码>>>` 的旧缓存骗了一轮）。

实测（qwen2.5-3b，本地）：英文导航栏 6 段 →`代码/问题/拉取请求/行动/项目/维基`，438 ms，
零重试、无标记；云端批量路径（同一台 llama-server 当网关，batchSize=8）一次请求返回
6 条 JSON，457 ms；GitHub 正文页 12 段全部译出，1577 ms，无软失败告警。中文导航栏
7 段中 5 段直接原样返回（语言判据命中），不再产生虚假告警。
## 截图翻译浮层：译文块可点选 / 双击复制 / Ctrl+C（2026-11-11）

- 单击一块 = 选中/取消（主色 2px 描边）；双击 = 直接复制这一块（边框闪一下当回执）。
- 选中若干块后，工具栏那颗按钮变成「复制 N 块」，只复制这几块；没选中时仍是整屏复制。
- Ctrl+C 三档：有块选中 → 复制选中的块（按原文先后拼，不是点击顺序）；没选块但拖选了
  文字 → 复制那段；都没有 → 整屏复制。
- 复制内容跟着「显示原文/译文」走：显示译文复制译文，某块没翻出来就退回原文。
- 正文从 Text 换成 TextEdit（readOnly + selectByMouse + activeFocusOnPress: false）：
  能拖选文字；关掉 activeFocusOnPress 是为了不抢根上的 Keys（Esc / Enter / 方向键
  都挂在那儿）。单击/双击用 TapHandler + DragThreshold，有拖拽就交给 TextEdit 拖选。
- 双击会先触发一次 singleTapped，所以单击走 240ms 的 Timer 去抖，否则会
  "选中→复制→取消选中"。
- 新增 ScreenFlow::copyText(text, closeAfter)：内容由 QML 拼好传进来，落剪贴板并打
  `[copy] N char(s) (M line(s)) from overlay selection, close-after-copy=K`。
  closeAfter 只给工具栏那颗用，仍受「复制后关闭」开关约束；双击与拖选不收起浮层。

验证（真鼠标 + 真键盘，viewer 把 gh.png 1:1 摊在 0,0，选区 0,0,602x844）：

- 双击第一块 → 剪贴板 `基于你的技术栈`。
- 单击一块 + Ctrl+C → 剪贴板 `基于你的技术栈`（选中态同时被清掉）。
- 在同一块里从 x=30 拖到 x=150 + Ctrl+C → `于你的技术栈`（纯文本拖选，不联动块选中）。
- 选两块后点工具栏「复制 2 块」→ 两行、顺序按原文（`基于你的技术栈` / `MSBuild 基础的项目`）。
- OCR 回读工具栏：按钮文案确实变成 `复制1块`。
### 追加调整（同日，用户反馈「双击变成全选」）

- 去掉双击复制：TapHandler 只留 onSingleTapped，双击不再复制本块，也不再和
  TextEdit 的"双击选词"打架（双击现在等价于点一下，只切换选中）。
- 选中态不只换描边：底色也换成 Theme.primaryContainer（不透明）——深色原文上
  只加 2px 描边根本看不出选中。
- 复制成功加回执气泡：copyToast（z: 40，贴在工具栏上方，1.1 秒后淡出），
  文案三档 `已复制` / `已复制 N 块` / `已复制选中文字` / `已复制整屏`。
  浮层是独立窗口，主界面的 Toast 过不来，所以自绘。位置用 ovbar 算，永远在屏内。
- 翻译：`已复制 %1 块→Copied %1`、`已复制→Copied`、`已复制整屏→Copied everything on screen`、
  `已复制选中文字→Copied the selected text`；lupdate 594 条，无 unfinished。

实测：双击后剪贴板不变（RESET 值原样）；单击一块 + Ctrl+C → 剪贴板 `基于你的技术栈`，
日志 `[copy] 7 char(s) (1 line(s)) from overlay selection, close-after-copy=0`；
截图 OCR 回读到气泡文案 `√已复制2块`。