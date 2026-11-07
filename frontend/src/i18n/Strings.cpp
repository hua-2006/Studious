#include "i18n/Strings.h"

#include <QLocale>

namespace studious::ui::i18n {
namespace {

Lang g_lang = Lang::Chinese;
bool g_langInitialized = false;

Strings makeChinese()
{
    Strings t;

    t.nav.translate = QStringLiteral("翻译");
    t.nav.hotkeys = QStringLiteral("快捷键");
    t.nav.config = QStringLiteral("配置");
    t.nav.settings = QStringLiteral("设置");
    t.nav.appearance = QStringLiteral("外观");
    t.nav.appTitle = QStringLiteral("Studious");
    t.nav.tagline = QStringLiteral("截图 / 文本翻译");

    t.common.ok = QStringLiteral("确定");
    t.common.cancel = QStringLiteral("取消");
    t.common.save = QStringLiteral("保存");
    t.common.reset = QStringLiteral("重置");
    t.common.delete_ = QStringLiteral("删除");
    t.common.close = QStringLiteral("关闭");
    t.common.retry = QStringLiteral("重试");
    t.common.apply = QStringLiteral("应用");
    t.common.enabled = QStringLiteral("已启用");
    t.common.disabled = QStringLiteral("已停用");
    t.common.none = QStringLiteral("无");
    t.common.unknown = QStringLiteral("未知");
    t.common.notSet = QStringLiteral("未设置");
    t.common.ready = QStringLiteral("就绪");
    t.common.notReady = QStringLiteral("未就绪");
    t.common.missingAsset = QStringLiteral("缺资产");
    t.common.unsupported = QStringLiteral("不支持");
    t.common.copy = QStringLiteral("复制");
    t.common.copyAll = QStringLiteral("复制全部");
    t.common.paste = QStringLiteral("粘贴");
    t.common.clear = QStringLiteral("清空");
    t.common.refresh = QStringLiteral("刷新");
    t.common.browse = QStringLiteral("浏览…");
    t.common.yes = QStringLiteral("是");
    t.common.no = QStringLiteral("否");
    t.common.confirm = QStringLiteral("确认");
    t.common.back = QStringLiteral("返回");
    t.common.unitParagraph = QStringLiteral("段");
    t.common.unitChars = QStringLiteral("字符");
    t.common.unitItems = QStringLiteral("项");
    t.common.seconds = QStringLiteral("秒");
    t.common.milliseconds = QStringLiteral("毫秒");
    t.common.loading = QStringLiteral("加载中…");

    t.translate.heading = QStringLiteral("欢迎来到 Studious");
    t.translate.subheading =
        QStringLiteral("文字如舟，渡你过语言的河。选好目标语言就能出发。");
    t.translate.srcLang = QStringLiteral("源语言");
    t.translate.dstLang = QStringLiteral("目标语言");
    t.translate.dstLangHint = QStringLiteral("可多选");
    t.translate.autoDetect = QStringLiteral("自动检测");
    t.translate.moreLangs = QStringLiteral("更多语言");
    t.translate.engine = QStringLiteral("引擎");
    t.translate.engineAuto = QStringLiteral("自动");
    t.translate.engineWeb = QStringLiteral("Web");
    t.translate.engineLlmCloud = QStringLiteral("云端 LLM");
    t.translate.engineLlmLocal = QStringLiteral("本地 LLM");
    t.translate.engineMt = QStringLiteral("本地 MT");
    t.translate.originalPane = QStringLiteral("原文");
    t.translate.translatedPane = QStringLiteral("译文");
    t.translate.originalCompare = QStringLiteral("原文对照");
    t.translate.cacheKeyHasEngine = QStringLiteral("缓存键含引擎");
    t.translate.run = QStringLiteral("开始翻译");
    t.translate.runHint = QStringLiteral("点击开始翻译 · 翻译中可取消");
    t.translate.running = QStringLiteral("翻译中…");
    t.translate.cancelHint = QStringLiteral("点击取消");
    t.translate.cancelled = QStringLiteral("已取消");
    t.translate.passthroughBadge = QStringLiteral("这是原文");
    t.translate.copySelection = QStringLiteral("复制选中");
    t.translate.statusLine = QStringLiteral("状态");
    t.translate.engineStatus = QStringLiteral("引擎状态");
    t.translate.viewEngineStatus = QStringLiteral("查看引擎状态");
    t.translate.emptyOriginal = QStringLiteral("在这里输入或粘贴要翻译的文本");
    t.translate.emptyTranslated = QStringLiteral("译文会出现在这里");
    t.translate.detectConfidence = QStringLiteral("检测置信度");

    t.hotkeys.heading = QStringLiteral("快捷键");
    t.hotkeys.subheading =
        QStringLiteral("怎么触发。每行四要素：功能名 / 当前键位 / 作用域 / 状态。");
    t.hotkeys.sceneGroup = QStringLiteral("场景快捷键");
    t.hotkeys.sceneGroupHint =
        QStringLiteral("截图、选区、实时翻译等平台能力尚未接入，键位一律留空。");
    t.hotkeys.captureGroup = QStringLiteral("截图翻译");
    t.hotkeys.clipboardGroup = QStringLiteral("剪贴板");
    t.hotkeys.windowGroup = QStringLiteral("窗口 / 进程");
    t.hotkeys.manageGroup = QStringLiteral("快捷键管理");
    t.hotkeys.conflictCheck = QStringLiteral("冲突检测");
    t.hotkeys.colAction = QStringLiteral("功能");
    t.hotkeys.colKey = QStringLiteral("键位");
    t.hotkeys.colScope = QStringLiteral("作用域");
    t.hotkeys.colStatus = QStringLiteral("状态");
    t.hotkeys.scopeGlobal = QStringLiteral("全局");
    t.hotkeys.scopeApp = QStringLiteral("应用内");
    t.hotkeys.pickKey = QStringLiteral("录制");
    t.hotkeys.clearKey = QStringLiteral("清除");
    t.hotkeys.restoreDefaults = QStringLiteral("恢复默认");
    t.hotkeys.platformUnsupported = QStringLiteral("当前平台不支持");
    t.hotkeys.conflictWith = QStringLiteral("与 %1 冲突");
    t.hotkeys.actionCaptureRegion = QStringLiteral("区域截图翻译");
    t.hotkeys.actionCaptureWindow = QStringLiteral("窗口翻译");
    t.hotkeys.actionCaptureScreen = QStringLiteral("全屏翻译");
    t.hotkeys.actionRepeatLast = QStringLiteral("重复上次区域");
    t.hotkeys.actionTranslateClipboard = QStringLiteral("翻译剪贴板");
    t.hotkeys.actionCopyBack = QStringLiteral("译文写回剪贴板");
    t.hotkeys.actionShowWindow = QStringLiteral("显示 / 隐藏主窗口");
    t.hotkeys.actionCancelTranslate = QStringLiteral("取消当前翻译");
    t.hotkeys.actionQuit = QStringLiteral("退出");
    t.hotkeys.actionToggleOverlay = QStringLiteral("显示 / 隐藏浮层");
    t.hotkeys.actionLockOverlay = QStringLiteral("锁定浮层");
    t.hotkeys.actionClearOverlays = QStringLiteral("清除所有浮层");

    t.config.heading = QStringLiteral("配置");
    t.config.subheading = QStringLiteral("翻译链路怎么接通：选引擎、填凭据、装模型。");
    t.config.engineSection = QStringLiteral("引擎");
    t.config.priorityHint = QStringLiteral("优先级（点击箭头排序，等价于 Router 的 priority）");
    t.config.moveUp = QStringLiteral("上移");
    t.config.moveDown = QStringLiteral("下移");
    t.config.statusPanel = QStringLiteral("引擎状态面板");
    t.config.statusPanelHint = QStringLiteral("按当前语言对实时探测每个引擎是否可用。");
    t.config.webSection = QStringLiteral("Web 来源");
    t.config.providerNote = QStringLiteral("列表顺序即优先级，靠前的先试。");
    t.config.azureKey = QStringLiteral("Azure Key");
    t.config.azureRegion = QStringLiteral("Region");
    t.config.llmSection = QStringLiteral("云端 LLM");
    t.config.baseUrl = QStringLiteral("Base URL");
    t.config.apiKey = QStringLiteral("API Key");
    t.config.model = QStringLiteral("模型");
    t.config.fetchModels = QStringLiteral("获取模型");
    t.config.temperature = QStringLiteral("temperature");
    t.config.maxTokens = QStringLiteral("maxTokens");
    t.config.systemPrompt = QStringLiteral("systemPrompt");
    t.config.systemPromptHint = QStringLiteral("留空使用内置提示词");
    t.config.reasoningSection = QStringLiteral("思考等级");
    t.config.reasoningLevel = QStringLiteral("等级");
    t.config.probeReasoning = QStringLiteral("探测能否关闭");
    t.config.probeHint = QStringLiteral("真发一次极短请求，看是否仍产生思考 token。");
    t.config.batchSize = QStringLiteral("批量合并段数");
    t.config.timeoutMs = QStringLiteral("超时");
    t.config.retryBudgetMs = QStringLiteral("重试预算");
    t.config.stripModelNotes = QStringLiteral("清理模型注记");
    t.config.stripModelNotesHint = QStringLiteral("删掉小模型加的「【中文】」「（译文）」这类整行括注");
    t.config.localMtSection = QStringLiteral("本地 MT");
    t.config.installedPairs = QStringLiteral("已装语言对");
    t.config.routeHint = QStringLiteral("实际走的路径");
    t.config.needDownload = QStringLiteral("需下载");
    t.config.localLlmSection = QStringLiteral("本地 LLM（llama-server）");
    t.config.modelFile = QStringLiteral("模型文件");
    t.config.contextSize = QStringLiteral("上下文");
    t.config.threads = QStringLiteral("线程");
    t.config.gpuLayers = QStringLiteral("GPU 层数");
    t.config.extraArgs = QStringLiteral("附加参数");
    t.config.startServer = QStringLiteral("启动");
    t.config.stopServer = QStringLiteral("停止");
    t.config.port = QStringLiteral("端口");
    t.config.pid = QStringLiteral("PID");
    t.config.logTail = QStringLiteral("日志");
    t.config.probeDisabled = QStringLiteral("已确认关闭思考");
    t.config.probeStillThinking = QStringLiteral("参数不生效，仍在思考");
    t.config.probeUnknown = QStringLiteral("网关未返回足够信息，无法判定");

    t.settings.heading = QStringLiteral("设置");
    t.settings.subheading = QStringLiteral("应用怎么跑：通用行为、缓存、网络、资产。");
    t.settings.general = QStringLiteral("通用");
    t.settings.uiLanguage = QStringLiteral("界面语言");
    t.settings.followSystem = QStringLiteral("跟随系统");
    t.settings.startOnBoot = QStringLiteral("开机自启");
    t.settings.minimizeToTray = QStringLiteral("最小化到托盘");
    t.settings.closeBehavior = QStringLiteral("关闭时");
    t.settings.closeExit = QStringLiteral("退出程序");
    t.settings.closeMinimize = QStringLiteral("最小化到托盘");
    t.settings.singleInstance = QStringLiteral("单实例运行");
    t.settings.behavior = QStringLiteral("翻译行为");
    t.settings.passthroughHint = QStringLiteral("源语言与目标语言相同时提示「这是原文」");
    t.settings.suspiciousWarning = QStringLiteral("源语言可疑时给出警告");
    t.settings.autoDetectSource = QStringLiteral("自动检测源语言");
    t.settings.cacheSection = QStringLiteral("缓存");
    t.settings.cacheEnabled = QStringLiteral("启用翻译缓存");
    t.settings.cacheCount = QStringLiteral("条数");
    t.settings.cacheSize = QStringLiteral("占用");
    t.settings.cachePath = QStringLiteral("数据库路径");
    t.settings.cacheHits = QStringLiteral("命中");
    t.settings.cacheMisses = QStringLiteral("未命中");
    t.settings.cacheClear = QStringLiteral("清空缓存");
    t.settings.cacheResetStats = QStringLiteral("重置统计");
    t.settings.network = QStringLiteral("网络");
    t.settings.proxy = QStringLiteral("代理");
    t.settings.proxyHint = QStringLiteral("留空表示直连。支持 http:// 与 socks5://。");
    t.settings.proxyRestartRequired = QStringLiteral("修改后需重启程序才生效");
    t.settings.timeout = QStringLiteral("超时");
    t.settings.assets = QStringLiteral("资产");
    t.settings.assetList = QStringLiteral("已安装列表");
    t.settings.assetStatus = QStringLiteral("状态");
    t.settings.assetSize = QStringLiteral("大小");
    t.settings.assetPath = QStringLiteral("路径");
    t.settings.download = QStringLiteral("下载");
    t.settings.downloadCancel = QStringLiteral("取消下载");
    t.settings.uninstall = QStringLiteral("卸载");
    t.settings.verify = QStringLiteral("校验");
    t.settings.importFile = QStringLiteral("从文件导入");
    t.settings.resumable = QStringLiteral("可续传");
    t.settings.manifest = QStringLiteral("清单");
    t.settings.manifestKeyId = QStringLiteral("签名 keyId");
    t.settings.manifestSchema = QStringLiteral("schema 版本");
    t.settings.manifestCheckUpdate = QStringLiteral("检查更新");
    t.settings.manifestVerified = QStringLiteral("验签结果");
    t.settings.totalInstalled = QStringLiteral("总占用");
    t.settings.data = QStringLiteral("数据");
    t.settings.installRoot = QStringLiteral("安装根目录");
    t.settings.logDir = QStringLiteral("日志目录");
    t.settings.resetApp = QStringLiteral("重置应用");
    t.settings.about = QStringLiteral("关于");
    t.settings.version = QStringLiteral("版本");
    t.settings.thirdParty = QStringLiteral("第三方组件");
    t.settings.license = QStringLiteral("许可证");
    t.settings.placeholderUnsupported = QStringLiteral("暂不支持");

    t.appearance.heading = QStringLiteral("外观");
    t.appearance.subheading = QStringLiteral("长什么样。");
    t.appearance.theme = QStringLiteral("主题");
    t.appearance.themeSystem = QStringLiteral("跟随系统");
    t.appearance.themeLight = QStringLiteral("浅色");
    t.appearance.themeDark = QStringLiteral("深色");
    t.appearance.font = QStringLiteral("字体");
    t.appearance.uiFont = QStringLiteral("界面字体");
    t.appearance.transFont = QStringLiteral("译文字体");
    t.appearance.scale = QStringLiteral("字号缩放");
    t.appearance.mainWindow = QStringLiteral("主窗口");
    t.appearance.compact = QStringLiteral("紧凑");
    t.appearance.standard = QStringLiteral("标准");
    t.appearance.statusDensity = QStringLiteral("状态栏信息密度");
    t.appearance.langDisplay = QStringLiteral("语言显示名");
    t.appearance.shortForm = QStringLiteral("简短（zh / en）");
    t.appearance.fullForm = QStringLiteral("完整（中文(简体) / English）");
    t.appearance.clearCacheTitle = QStringLiteral("清空翻译缓存？");
    t.appearance.clearCacheBody = QStringLiteral("缓存里的译文会被删除，之后同样的原文需要重新请求引擎。");
    t.appearance.resetAppTitle = QStringLiteral("重置应用？");
    t.appearance.resetAppBody = QStringLiteral("所有设置会恢复默认。已下载的资产不会被删除。");

    return t;
}

Strings makeEnglish()
{
    Strings t;

    t.nav.translate = QStringLiteral("Translate");
    t.nav.hotkeys = QStringLiteral("Hotkeys");
    t.nav.config = QStringLiteral("Engines");
    t.nav.settings = QStringLiteral("Settings");
    t.nav.appearance = QStringLiteral("Appearance");
    t.nav.appTitle = QStringLiteral("Studious");
    t.nav.tagline = QStringLiteral("Screenshot / text translation");

    t.common.ok = QStringLiteral("OK");
    t.common.cancel = QStringLiteral("Cancel");
    t.common.save = QStringLiteral("Save");
    t.common.reset = QStringLiteral("Reset");
    t.common.delete_ = QStringLiteral("Delete");
    t.common.close = QStringLiteral("Close");
    t.common.retry = QStringLiteral("Retry");
    t.common.apply = QStringLiteral("Apply");
    t.common.enabled = QStringLiteral("Enabled");
    t.common.disabled = QStringLiteral("Disabled");
    t.common.none = QStringLiteral("None");
    t.common.unknown = QStringLiteral("Unknown");
    t.common.notSet = QStringLiteral("Not set");
    t.common.ready = QStringLiteral("Ready");
    t.common.notReady = QStringLiteral("Not ready");
    t.common.missingAsset = QStringLiteral("Missing asset");
    t.common.unsupported = QStringLiteral("Unsupported");
    t.common.copy = QStringLiteral("Copy");
    t.common.copyAll = QStringLiteral("Copy all");
    t.common.paste = QStringLiteral("Paste");
    t.common.clear = QStringLiteral("Clear");
    t.common.refresh = QStringLiteral("Refresh");
    t.common.browse = QStringLiteral("Browse…");
    t.common.yes = QStringLiteral("Yes");
    t.common.no = QStringLiteral("No");
    t.common.confirm = QStringLiteral("Confirm");
    t.common.back = QStringLiteral("Back");
    t.common.unitParagraph = QStringLiteral("para");
    t.common.unitChars = QStringLiteral("chars");
    t.common.unitItems = QStringLiteral("items");
    t.common.seconds = QStringLiteral("s");
    t.common.milliseconds = QStringLiteral("ms");
    t.common.loading = QStringLiteral("Loading…");

    t.translate.heading = QStringLiteral("Welcome to Studious");
    t.translate.subheading =
        QStringLiteral("Pick your target language and start translating.");
    t.translate.srcLang = QStringLiteral("Source");
    t.translate.dstLang = QStringLiteral("Target");
    t.translate.dstLangHint = QStringLiteral("multi-select");
    t.translate.autoDetect = QStringLiteral("Auto-detect");
    t.translate.moreLangs = QStringLiteral("More languages");
    t.translate.engine = QStringLiteral("Engine");
    t.translate.engineAuto = QStringLiteral("Auto");
    t.translate.engineWeb = QStringLiteral("Web");
    t.translate.engineLlmCloud = QStringLiteral("Cloud LLM");
    t.translate.engineLlmLocal = QStringLiteral("Local LLM");
    t.translate.engineMt = QStringLiteral("Local MT");
    t.translate.originalPane = QStringLiteral("Source text");
    t.translate.translatedPane = QStringLiteral("Translation");
    t.translate.originalCompare = QStringLiteral("Show source");
    t.translate.cacheKeyHasEngine = QStringLiteral("Cache key includes engine");
    t.translate.run = QStringLiteral("Translate");
    t.translate.runHint = QStringLiteral("Click to translate · click again to cancel");
    t.translate.running = QStringLiteral("Translating…");
    t.translate.cancelHint = QStringLiteral("Click to cancel");
    t.translate.cancelled = QStringLiteral("Cancelled");
    t.translate.passthroughBadge = QStringLiteral("Source text");
    t.translate.copySelection = QStringLiteral("Copy selection");
    t.translate.statusLine = QStringLiteral("Status");
    t.translate.engineStatus = QStringLiteral("Engine status");
    t.translate.viewEngineStatus = QStringLiteral("View engine status");
    t.translate.emptyOriginal = QStringLiteral("Type or paste the text to translate");
    t.translate.emptyTranslated = QStringLiteral("Translation will appear here");
    t.translate.detectConfidence = QStringLiteral("confidence");

    t.hotkeys.heading = QStringLiteral("Hotkeys");
    t.hotkeys.subheading =
        QStringLiteral("How things are triggered. Four fields per row: action / key / scope / status.");
    t.hotkeys.sceneGroup = QStringLiteral("Scene hotkeys");
    t.hotkeys.sceneGroupHint =
        QStringLiteral("Capture, selection and live translation are not wired yet; all keys stay empty.");
    t.hotkeys.captureGroup = QStringLiteral("Screenshot translation");
    t.hotkeys.clipboardGroup = QStringLiteral("Clipboard");
    t.hotkeys.windowGroup = QStringLiteral("Window / process");
    t.hotkeys.manageGroup = QStringLiteral("Hotkey management");
    t.hotkeys.conflictCheck = QStringLiteral("Conflict check");
    t.hotkeys.colAction = QStringLiteral("Action");
    t.hotkeys.colKey = QStringLiteral("Key");
    t.hotkeys.colScope = QStringLiteral("Scope");
    t.hotkeys.colStatus = QStringLiteral("Status");
    t.hotkeys.scopeGlobal = QStringLiteral("Global");
    t.hotkeys.scopeApp = QStringLiteral("In-app");
    t.hotkeys.pickKey = QStringLiteral("Record");
    t.hotkeys.clearKey = QStringLiteral("Clear");
    t.hotkeys.restoreDefaults = QStringLiteral("Restore defaults");
    t.hotkeys.platformUnsupported = QStringLiteral("Not supported on this platform");
    t.hotkeys.conflictWith = QStringLiteral("Conflicts with %1");
    t.hotkeys.actionCaptureRegion = QStringLiteral("Translate screen region");
    t.hotkeys.actionCaptureWindow = QStringLiteral("Translate window");
    t.hotkeys.actionCaptureScreen = QStringLiteral("Translate full screen");
    t.hotkeys.actionRepeatLast = QStringLiteral("Repeat last region");
    t.hotkeys.actionTranslateClipboard = QStringLiteral("Translate clipboard");
    t.hotkeys.actionCopyBack = QStringLiteral("Copy result to clipboard");
    t.hotkeys.actionShowWindow = QStringLiteral("Show / hide main window");
    t.hotkeys.actionCancelTranslate = QStringLiteral("Cancel current translation");
    t.hotkeys.actionQuit = QStringLiteral("Quit");
    t.hotkeys.actionToggleOverlay = QStringLiteral("Show / hide overlay");
    t.hotkeys.actionLockOverlay = QStringLiteral("Lock overlay");
    t.hotkeys.actionClearOverlays = QStringLiteral("Clear all overlays");

    t.config.heading = QStringLiteral("Engines");
    t.config.subheading =
        QStringLiteral("How the translation pipeline is wired: pick engines, fill credentials, install models.");
    t.config.engineSection = QStringLiteral("Engines");
    t.config.priorityHint = QStringLiteral("Priority (reorder with the arrows; maps to Router priority)");
    t.config.moveUp = QStringLiteral("Move up");
    t.config.moveDown = QStringLiteral("Move down");
    t.config.statusPanel = QStringLiteral("Engine status");
    t.config.statusPanelHint = QStringLiteral("Probes every engine for the current language pair.");
    t.config.webSection = QStringLiteral("Web providers");
    t.config.providerNote = QStringLiteral("List order is priority; earlier entries are tried first.");
    t.config.azureKey = QStringLiteral("Azure key");
    t.config.azureRegion = QStringLiteral("Region");
    t.config.llmSection = QStringLiteral("Cloud LLM");
    t.config.baseUrl = QStringLiteral("Base URL");
    t.config.apiKey = QStringLiteral("API key");
    t.config.model = QStringLiteral("Model");
    t.config.fetchModels = QStringLiteral("Fetch models");
    t.config.temperature = QStringLiteral("temperature");
    t.config.maxTokens = QStringLiteral("maxTokens");
    t.config.systemPrompt = QStringLiteral("systemPrompt");
    t.config.systemPromptHint = QStringLiteral("Leave empty to use the built-in prompt");
    t.config.reasoningSection = QStringLiteral("Reasoning effort");
    t.config.reasoningLevel = QStringLiteral("Level");
    t.config.probeReasoning = QStringLiteral("Probe");
    t.config.probeHint = QStringLiteral("Sends one tiny request to check whether thinking can be disabled.");
    t.config.batchSize = QStringLiteral("Batch size");
    t.config.timeoutMs = QStringLiteral("Timeout");
    t.config.retryBudgetMs = QStringLiteral("Retry budget");
    t.config.stripModelNotes = QStringLiteral("Strip model notes");
    t.config.stripModelNotesHint = QStringLiteral("Drops whole-line annotations small models add");
    t.config.localMtSection = QStringLiteral("Local MT");
    t.config.installedPairs = QStringLiteral("Installed pairs");
    t.config.routeHint = QStringLiteral("Route actually used");
    t.config.needDownload = QStringLiteral("Needs download");
    t.config.localLlmSection = QStringLiteral("Local LLM (llama-server)");
    t.config.modelFile = QStringLiteral("Model file");
    t.config.contextSize = QStringLiteral("Context");
    t.config.threads = QStringLiteral("Threads");
    t.config.gpuLayers = QStringLiteral("GPU layers");
    t.config.extraArgs = QStringLiteral("Extra args");
    t.config.startServer = QStringLiteral("Start");
    t.config.stopServer = QStringLiteral("Stop");
    t.config.port = QStringLiteral("Port");
    t.config.pid = QStringLiteral("PID");
    t.config.logTail = QStringLiteral("Log");
    t.config.probeDisabled = QStringLiteral("Thinking confirmed disabled");
    t.config.probeStillThinking = QStringLiteral("Parameter ignored, still thinking");
    t.config.probeUnknown = QStringLiteral("Gateway returned too little information");

    t.settings.heading = QStringLiteral("Settings");
    t.settings.subheading =
        QStringLiteral("How the app behaves: general, cache, network, assets.");
    t.settings.general = QStringLiteral("General");
    t.settings.uiLanguage = QStringLiteral("UI language");
    t.settings.followSystem = QStringLiteral("Follow system");
    t.settings.startOnBoot = QStringLiteral("Start on boot");
    t.settings.minimizeToTray = QStringLiteral("Minimize to tray");
    t.settings.closeBehavior = QStringLiteral("On close");
    t.settings.closeExit = QStringLiteral("Quit");
    t.settings.closeMinimize = QStringLiteral("Minimize to tray");
    t.settings.singleInstance = QStringLiteral("Single instance");
    t.settings.behavior = QStringLiteral("Translation behaviour");
    t.settings.passthroughHint = QStringLiteral("Mark source text when source equals target");
    t.settings.suspiciousWarning = QStringLiteral("Warn when the source language looks wrong");
    t.settings.autoDetectSource = QStringLiteral("Auto-detect source language");
    t.settings.cacheSection = QStringLiteral("Cache");
    t.settings.cacheEnabled = QStringLiteral("Enable translation cache");
    t.settings.cacheCount = QStringLiteral("Entries");
    t.settings.cacheSize = QStringLiteral("On disk");
    t.settings.cachePath = QStringLiteral("Database path");
    t.settings.cacheHits = QStringLiteral("Hits");
    t.settings.cacheMisses = QStringLiteral("Misses");
    t.settings.cacheClear = QStringLiteral("Clear cache");
    t.settings.cacheResetStats = QStringLiteral("Reset stats");
    t.settings.network = QStringLiteral("Network");
    t.settings.proxy = QStringLiteral("Proxy");
    t.settings.proxyHint = QStringLiteral("Empty means direct. http:// and socks5:// are supported.");
    t.settings.proxyRestartRequired = QStringLiteral("Takes effect after restarting the app");
    t.settings.timeout = QStringLiteral("Timeout");
    t.settings.assets = QStringLiteral("Assets");
    t.settings.assetList = QStringLiteral("Installed");
    t.settings.assetStatus = QStringLiteral("Status");
    t.settings.assetSize = QStringLiteral("Size");
    t.settings.assetPath = QStringLiteral("Path");
    t.settings.download = QStringLiteral("Download");
    t.settings.downloadCancel = QStringLiteral("Cancel download");
    t.settings.uninstall = QStringLiteral("Uninstall");
    t.settings.verify = QStringLiteral("Verify");
    t.settings.importFile = QStringLiteral("Import from file");
    t.settings.resumable = QStringLiteral("Resumable");
    t.settings.manifest = QStringLiteral("Manifest");
    t.settings.manifestKeyId = QStringLiteral("Signing keyId");
    t.settings.manifestSchema = QStringLiteral("Schema version");
    t.settings.manifestCheckUpdate = QStringLiteral("Check for updates");
    t.settings.manifestVerified = QStringLiteral("Signature");
    t.settings.totalInstalled = QStringLiteral("Total size");
    t.settings.data = QStringLiteral("Data");
    t.settings.installRoot = QStringLiteral("Install root");
    t.settings.logDir = QStringLiteral("Log directory");
    t.settings.resetApp = QStringLiteral("Reset app");
    t.settings.about = QStringLiteral("About");
    t.settings.version = QStringLiteral("Version");
    t.settings.thirdParty = QStringLiteral("Third-party");
    t.settings.license = QStringLiteral("License");
    t.settings.placeholderUnsupported = QStringLiteral("Not supported yet");

    t.appearance.heading = QStringLiteral("Appearance");
    t.appearance.subheading = QStringLiteral("How it looks.");
    t.appearance.theme = QStringLiteral("Theme");
    t.appearance.themeSystem = QStringLiteral("Follow system");
    t.appearance.themeLight = QStringLiteral("Light");
    t.appearance.themeDark = QStringLiteral("Dark");
    t.appearance.font = QStringLiteral("Fonts");
    t.appearance.uiFont = QStringLiteral("UI font");
    t.appearance.transFont = QStringLiteral("Translation font");
    t.appearance.scale = QStringLiteral("Text scale");
    t.appearance.mainWindow = QStringLiteral("Main window");
    t.appearance.compact = QStringLiteral("Compact");
    t.appearance.standard = QStringLiteral("Standard");
    t.appearance.statusDensity = QStringLiteral("Status bar density");
    t.appearance.langDisplay = QStringLiteral("Language labels");
    t.appearance.shortForm = QStringLiteral("Short (zh / en)");
    t.appearance.fullForm = QStringLiteral("Full (Chinese / English)");
    t.appearance.clearCacheTitle = QStringLiteral("Clear translation cache?");
    t.appearance.clearCacheBody =
        QStringLiteral("Cached translations are deleted; the same source text will be requested again.");
    t.appearance.resetAppTitle = QStringLiteral("Reset app?");
    t.appearance.resetAppBody =
        QStringLiteral("All settings return to defaults. Downloaded assets are kept.");

    return t;
}

const Strings &tableFor(Lang lang)
{
    static const Strings zh = makeChinese();
    static const Strings en = makeEnglish();
    return lang == Lang::English ? en : zh;
}

} // namespace

Lang current()
{
    if (!g_langInitialized) {
        g_lang = detectSystemLanguage();
        g_langInitialized = true;
    }
    return g_lang;
}

void setLanguage(Lang lang)
{
    g_lang = lang;
    g_langInitialized = true;
}

Lang detectSystemLanguage()
{
    return QLocale::system().language() == QLocale::Chinese ? Lang::Chinese : Lang::English;
}

QString displayName(Lang lang)
{
    return lang == Lang::English ? QStringLiteral("English") : QStringLiteral("中文");
}

const Strings &s()
{
    return tableFor(current());
}

} // namespace studious::ui::i18n
