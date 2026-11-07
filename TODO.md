# Studious 待做清单（自定义接口 / 可配置项）

> 这份清单只记「还没做成用户可调设置」的东西。已经做完的写在 README 与 `_log.md` 里。
> 状态列：`已实现` = 代码在跑但入口还没做；`待接线` = 后端/平台有接口、界面没用上；`未开始` = 两边都没有。

## 1. 截屏翻译 · 放大镜

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 放大倍率 | 已完成 | `AppSettings::overlayZoom`，键 `settings/overlay/zoom`，2–8，默认 3；设置 → 截图 → 「放大镜倍率」用 − / + 步进 | 无（若要档位选择再改） |
| 放大镜位置 | 已完成 | `AppSettings::magnifierCorner`，键 `settings/overlay/magnifierCorner`，四值枚举，默认 `bottomRight`；「浮层」页 → 放大镜位置（分段按钮）。QML 把枚举拆成 x/y 两轴各自翻折，贴边只翻越界的那一轴 | 无 |
| 放大镜开关 | 已完成 | `AppSettings::overlayMagnifier`，键 `settings/overlay/magnifier`，默认 true；「浮层」页 → 拖框时显示放大镜。关掉后只剩十字光标与尺寸标签 | 无 |
| 放大镜尺寸 | 未开始 | `zoomSize` 写死 104px | 若要跟字号联动可并入上一项；不建议单独暴露 |
| 取样说明 | 未开始 | 只显示 X/Y、色块、`#RRGGBB`、`R G B` | 可选加 `HSL` / 复制色值按钮 |

## 2. 截屏翻译 · 浮层外观

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 底色不透明度 | 已完成 | `AppSettings::overlayOpacity`，键 `settings/overlay/opacity`，40–100，默认 96；设置 → 截图 → 「浮层不透明度」（步进 4） | 无 |
| 译文字号 | 已完成 | `AppSettings::overlayFontSize`，10–20，默认 13；设置 → 截图 → 「浮层字号」 | 无 |
| 保留原文标注 | 已完成 | `settings/overlay/keepOriginal`；设置 → 截图 开关 | 无 |
| 选完直接翻译 | 已完成 | `settings/capture/autoRunOnSelect`；设置 → 截图 开关（关掉走 A2 二次确认） | 无 |
| 自动复制译文 | 已完成 | `settings/capture/autoCopy`；设置 → 截图 开关 | 无 |
| 复制后自动关闭 | 已完成 | `settings/capture/autoHide`（默认开）+ `AppSettings::overlayAutoHideDelay`（键 `settings/overlay/autoHideDelay`，0–10 秒，默认 0）：`ScreenFlow` 两处收浮层都按这个秒数 `QTimer::singleShot` | 无 |
| 遮罩压暗程度 | 已完成 | `AppSettings::overlayDim`，键 `settings/overlay/dim`，20–80，默认 45；「浮层」页 → 压暗程度。`CaptureOverlay` 用 `Qt.rgba(0,0,0, overlayDim/100)` | 无 |
| 选区边框粗细 / 颜色 | 未开始 | 2px + `Theme.primary` | 一般不需要暴露，跟随强调色即可 |

## 3. 截屏翻译 · 操作与热键

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 截图热键 | 已实现 | `AppSettings::hotkeyCapture`，默认 `Ctrl+Alt+S` | 「快捷键」页做成录制控件（现在整页是占位） |
| 小窗热键 | 已实现 | `hotkeyMini`，默认 `Ctrl+Alt+W` | 同上 |
| 实时热键 | 已实现 | `hotkeyLive`，默认 `Ctrl+Alt+L` | 同上 |
| 二次调整最小尺寸 | 未开始 | 写死 12px | 一般不需要暴露 |
| 方向键微调 | 已完成 | `AppSettings::overlayArrowNudge`，键 `settings/overlay/arrowNudge`，默认开；「浮层」页 → 方向键微调选区。`CaptureOverlay` 在 `Keys.onPressed` 里判开关（不用 `Keys.enabled`，否则同一 Item 的 Esc / Enter 会被一起关掉），1px / Shift 10px，只在「已框好、未开始翻译」时生效 | 无 |

## 4. 小窗翻译（B 页，主链路已通）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 窗口 | 已完成 | `frontend/qml/overlay/MiniWindow.qml`，560×558、圆角 20、无边框可拖、`transientParent: null`（主窗口最小化不带走它） | 记住窗口位置与尺寸（`settings/mini/x/y/w/h`） |
| 引擎 | 已完成 | `frontend/src/MiniSession.{h,cpp}` 持一个独立 `QuickTranslate`（自带 `TranslateJob`），配置每次现读 `AppSettings` | 加就地引擎切换（现在是跟设置走） |
| 输入 | 已完成 | `TextEdit` 双向绑 `miniSession.sourceText`；`Ctrl+Enter` 直接翻；「清空」复位 | 真·划词：选好文字按热键后自动带进来 |
| 划词获取 | 部分完成 | 现在读系统剪贴板（`MiniSession::grabClipboard()`） | 模拟 `Ctrl+C` 抓前台选区（要进 `platform/` 加能力，别把 Win32 写进 `MiniSession`） |
| 目标语言多选 | 已完成 | 带勾选框的窗内菜单，标签页按 `dstLangs` 顺序排；至少留一个 | 「更多语言…」折叠（现在一次列出全部 13 种） |
| 置顶 | 已完成 | `AppSettings::miniAlwaysOnTop` → `AppBackend::miniAlwaysOnTop` → 窗口 `flags`；标题栏按钮与设置小窗都能切 | — |
| 译文带原文 | 已完成 | `backend.showSource`（开关放进设置小窗的 `mode="mini"` 段）；打开时每段译文上方多一行原文 | — |
| 结果与复制 | 已完成 | 逐段 `01/02/03` + 复制单段、「全部复制」、引擎名、耗时 | 段落跳转/高亮 |
| 保存到记录 | 已完成 | 「保存到记录」→ `miniSession.saveRecordRequested` → `backend.saveMiniRecord` 写 `mini.db`；历史没开时按钮下方直接说明原因 | 自动存档（跟截屏一致，翻完就存） |
| 记录页签 | 已完成 | 合并三库列表（时间/来源/摘要），点一行在下方看全文；空态区分「历史没开」与「还没有记录」 | 详情里加复制与跳转原文 |
| 注音（ruby） | 未开始 | 设计稿 B4 的「显示注音」 | 需要后端判断语言（日语注音）；先留开关位 |

## 5. 实时翻译（C 页，主链路已通）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 抓屏 → OCR → 翻译 → 贴回 | 已完成 | `frontend/src/LiveSession.{h,cpp}` 单 QTimer 轮询，帧排不上就丢，绝不排队；行用 `\n\n` 拼给 `QuickTranslate` 再 1:1 贴回 | — |
| 选区入口 | 已完成 | `CaptureOverlay.purpose = "live"`：工具栏只留「开始实时翻译 / 取消」，引擎槽与「设置」隐藏；松手不直接开跑 | — |
| 贴纸窗口 | 已完成 | `frontend/qml/overlay/LiveOverlay.qml`，`visible: liveSession.phase !== "Idle"`（**不能写死 false**，C++ 只摆位置不管显隐）；点击穿透 + 排除截图由 `attachPlateWindow()` 打 | — |
| 控制条 | 已完成 | `frontend/qml/overlay/LiveControlBar.qml`，619×50，贴着选区右下，运行/暂停/出错三态，可拖动，Esc 停止 | 记住位置 |
| 刷新间隔 | 已完成 | `AppSettings::liveIntervalMs`，400–1500，默认 600；「浮层」页 `NumStepper` 可改，改完下一拍生效 | — |
| 只在内容变化时翻译 | 已完成 | `AppSettings::liveOnlyOnChange`，默认开；指纹是原文本身 | — |
| 锁定本地引擎 | 已完成 | `AppSettings::liveEngineLocked`，默认 true（云端/Web 单次 1 s 以上，实时没意义） | 允许强制云端 |
| 慢引擎提示 | 已完成 | `elapsedMs > 1000` → `slowHint`「当前引擎每次约 %1 秒，实时效果受限」 | — |
| 连续无字中断 | 已完成 | 连续 3 帧 OCR 无文字 → `phase=Error` + 停表 + 收单；抓屏失败同路 | — |
| 会话记录 | 已完成 | 收单发 `sessionFinished(originals, blocks)` → `backend.saveLiveRecord(..., liveSession.engineLabel)` 写 `live.db`，一次会话一条 | — |
| 控制条位置 | 未开始 | 现在每次按选区摆 | 记住位置到 `settings/live/ctrlPos` |
| 译文行数上限 | 未开始 | 设计稿 C8「译文过长」内滚 | 加 `liveMaxLines` |

## 6. 记录（B7）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 保存内容 | 已实现 | `AppSettings::recordSaveMode` = `original` / `current` | 二级设置里单选 |
| 条数上限 | 未开始 | `TranslatorRecords` 写死 2000 条 | 加 `recordsMax`（500–10000） |
| 缩略图 | 未开始 | 只存 `imagePath`，删记录不删图 | 加「同时删除图片」开关 + 定期清理 |

## 7. 引擎与网络

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 引擎优先级 | 已实现 | `AppSettings::enginePriority` + 配置页拖动排序 | 无 |
| Web 来源 | 已实现 | `webProviderId` 单值 | 若要「多来源排序」需改后端 `TranslateRequest.webProviders` 语义 |
| 全局 HTTP 超时 | 已实现 | `httpTimeoutMs` → `STUDIOUS_HTTP_TIMEOUT`（只抬下限） | 设置页已有入口 |
| 代理 | 已实现 | `proxyUrl` → `STUDIOUS_HTTP_PROXY`，回环自动直连 | 设置页已有入口 |

## 8. 资产与 OCR

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| OCR 模型下载 | 已完成 | PP-OCRv4 det/rec + `ppocr_keys_v1.txt`，`manifest` 三条填了真 URL / sha256 / size（HF `SWHL/RapidOCR` + PaddleOCR v2.10.0 字典），`--asset-download` 可装 | 若要换模型版本再改 |
| OCR 语言 | 未开始 | `OcrOptions::languages` 有字段，界面没入口 | 加 `ocr/languages`（中文 / 英文 / 中英） |
| OCR 检测阈值 | 未开始 | `detThreshold=0.3`、`boxThreshold=0.5`、`unclipRatio=1.6` 写死默认 | 只在排错时需要，可放「高级」里 |

## 9. 平台层（Linux 预留）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 抓屏 | Windows 已实现 / Linux 未开始 | `ScreenCaptureWin` GDI；X11 需 XComposite，Wayland 只能走 portal | 先做 X11 的 `XGetImage` + 兜底提示 |
| 全局热键 | Windows 已实现 / Linux 未开始 | `HotkeyWin`；X11 用 `XGrabKey`，Wayland 无解 | 同上 |
| 浮层定位 / 截图排除 | Windows 已实现 / Linux 未开始 | `OverlayWin`；X11 无 `WDA_EXCLUDEFROMCAPTURE` 等价物 | 只做「不被自己拍到」的近似方案 |
| 孤儿进程兜底 | Windows 已实现 / Linux 未开始 | `ProcessGuard` 用 Job Object | 换 `prctl(PR_SET_PDEATHSIG)` |

## 10. 主界面 · 翻译字号

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 翻译字号 | 未开始 | 译文栏用 `Theme.fontContent`（霞鹜文楷）+ 写死的 `Theme.fontSizeBody`（13）；外观里的「字号缩放」是全局倍率（80–150%），改不了"只看译文时字太小" | 加 `settings/ui/readingFontSize`（11–24，默认 13），只作用于翻译页双语正文（原文/译文两栏 + 段落操作区），不动界面控件；截屏浮层的字号走 `overlayFontSize`，两者独立 |

## 11. 翻译历史（已做：自动存档 + 三分库 + 合并列表）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 开关 | 已完成 | `AppSettings::historyEnabled`，键 `settings/history/enabled`，默认关；设置 → 缓存 → 「历史对话」→「保存翻译历史」。打开时顺手 `mkpath` 目录 | 无 |
| 目录 | 已完成 | `AppSettings::historyDir`，键 `settings/history/dir`；空串 = `<资产根>/history`（默认 `%APPDATA%\studious\assets\history`），跟着「迁移资产根」一起搬。`AppBackend::openHistoryDir()` 建目录并调 `QDesktopServices` | 无（目录选择器可后加） |
| 写入 | 已完成 | 不是一个总库：`AppBackend::persistRecord()` 按来源分三库 `%APPDATA%\studious\records\{capture,mini,live}.db`（`TranslatorRecords`，`records` 表 10 列）。`persistRecord` 先读开关，关着直接不写 —— 存档全是自动的，开关是唯一总闸。截屏翻译在 `ScreenFlow` 翻译成功回调里自动写（`blocksChanged` 之后、`finished` 之前），小窗走 `saveMiniRecord()`，实时走 `beginLiveSession/appendLiveSession/finishLiveSession`（一次会话写一条，重复行会跳过） | 无。`saveMiniRecord` / 三个 live 接口已实现但 B/C 页还没做，**尚无调用点、未运行期验证** |
| 浏览 | 已完成 | 设置 → 缓存 → 「历史对话」：`最近 %1 条` + 刷新 + 表头（时间/来源/摘要）+ 50 行列表，三库合并按 `createdAt` 倒序；切到缓存分区或页面加载时重读 | 搜索、删除单条、清空、导出、点击回看详情 |
| 旧文件 | 待清理 | `%APPDATA%\studious\records.db`（单库时代的遗留）已不再使用 | 首次启动时若存在则删掉，或写进迁移逻辑 |
| 容量 | 未开始 | 无上限 | 参考 `TranslatorRecords` 的剪枝：超过 N 条（默认 2000）或占用超过 M MB 时删最旧的 |

## 12. 翻译取消（已做，剩三处）

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 后端取消通道 | 已完成 | `ITranslator::cancel()` → 四个引擎的 `std::atomic_bool m_cancelRequested`；`TranslatorRouter::cancel()` 逐个转发 | 无 |
| LLM 的取消粒度 | 已完成 | 批量循环、逐段补救循环、`postChat` 的 5 次换 prompt 重试，三处都在每次发请求前检查取消 | 无 |
| 主界面取消 | 已完成 | 状态行右侧 `AppButton`「取消」`visible: backend.running` → `AppBackend::cancel()`（`TranslateJob::cancel()` 置标志 + 换代号，迟到结果直接丢） | 无 |
| 浮层取消 | 已完成 | 浮层忙碌框里的「取消 Esc」+ Esc 三档（收菜单 / 取消翻译 / 关浮层）→ `ScreenFlow::cancelTranslation()`，保住选区与已出的贴纸，状态显示「已取消。」 | 无 |
| 在飞的 HTTP | 未开始 | 同步 `HttpClient::send` 不可外部打断，点了取消最多还要等手上那一次请求（实测 1.7 s）跑完 | 给 `net::HttpRequest` 加中断回调（或给 `QtHttpClient` 加 `abort()`），让取消能当场掐断连接 |
| Web / 本地 MT 的取消粒度 | 未开始 | 两者只在段落循环顶部检查一次；Web 的失败重试与本地 MT 的单段 decode 不查取消 | 照 LLM 的做法在各自的每次请求 / 每段推理前补检查 |
| 主窗口的翻译入口 | 未开始 | 翻译页只有「查看引擎状态」，`backend.translate()` 没有任何 QML 调用点 —— 双栏只能显示历史结果，"取消"按钮因此平时不出现 | 给翻译页补「翻译」按钮（或 Ctrl+Enter），接 `backend.translate()`；顺带把 FAB 的语义定下来 |

## 13. 浮层「设置」小窗

| 项 | 状态 | 现状 | 要做的 |
| --- | --- | --- | --- |
| 工具栏「设置」按钮 | 已完成 | `CaptureOverlay.qml` 引擎槽右侧的 `gearSlot`，文字「设置」（原来是 `⚙`），再点一下收起 | 无 |
| 点设置不打断这一单 | 已完成 | 只切 `overlay.settingsOpen` + `settingsPanel.openAt(...)`，**不调 `screenFlow.cancel()`**；选区、识别结果、贴纸都留着 | 无 |
| 引擎菜单的状态点 | 已完成 | `CaptureOverlay.qml` 每行右侧 8px 圆点（绿 就绪 / 黄 未就绪 / 红 缺资产），红点那行 `opacity 0.45` 且 `TapHandler.enabled: false`（点不动、光标变箭头）；「自动」看全体；打开菜单前先 `backend.refreshEngineStatuses()` | 无 |
| 子窗口归属 | 已完成 | `CaptureSettingsPanel` 声明在 `CaptureOverlay.qml` 里（owner = 浮层）：`flags: Qt.Tool + Frameless + NoDropShadow`、无 `transientParent`、无 StaysOnTop —— 归属窗口恒在 owner 之上；`visible: overlay.settingsOpen && overlay.engaged`，浮层一收它跟着收 | 无 |
| 标题拖动 | 已完成 | 标题行整条是拖拽把手（`Qt.SizeAllCursor`），先试 `panel.startSystemMove()`，拿不到时退回 `x/y` 跟随 | 无 |
| 截图翻译节 | 已完成 | 选完直接翻译 / 保留原文标注 / 自动复制译文 / 译文不透明度 / 译文字号 / 复制后延迟关闭，全部接 `backend.*` | 无 |
| 更多设置 | 已完成 | 页脚无边框行「更多设置 ›」（强调色文字，悬浮铺 `surfaceHighest`）→ 浮层 `moreSettingsRequested()` → Main 收这一单、主窗口 `show/raise` 并跳「浮层」页 | 无 |
| Esc 分档 | 已完成 | 设置小窗 → 引擎菜单 → 取消翻译 → 关浮层，四档；小窗自己也有 `Keys.onEscapePressed` | 无 |
| 翻译小窗口的选项 | 未开始 | 已按要求从小窗里移除，等 B 页（小窗）落地后再放进主界面「浮层」页 | 随 B 页加 `settings/mini/*` |
| 实时翻译的选项 | 未开始 | 已从小窗里移除，等 C 页（实时）落地后再放进「浮层」页 | 随 C 页加 `settings/live/*` |
