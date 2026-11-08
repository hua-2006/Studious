#include "AppBackend.h"

#include "AppSettings.h"
#include "AssetFormat.h"
#include "AssetPaths.h"
#include "EngineIds.h"
#include "Languages.h"
#include "LocalEngine.h"
#include "LogSink.h"
#include "QuickTranslate.h"
#include "TranslateJob.h"
#include "i18n/Strings.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtConcurrent>

#include <studious/Factory.h>
#include <studious/asset/AssetRegistry.h>
#include <studious/asset/Downloader.h>
#include <studious/asset/Manifest.h>
#include <studious/cache/TranslationCache.h>
#include <studious/contract/LanguageRegistry.h>
#include <studious/i18n/L10n.h>
#include <studious/i18n/LanguageDetector.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>
#include <studious/translators/LocalMtTranslator.h>
#include <studious/translators/WebProvider.h>

#include <algorithm>
#include <utility>

namespace studious::ui {
namespace {

// 引擎 id 换算只有一份实现（frontend/src/EngineIds.cpp）。这里用 using 把它引进
// 匿名 namespace，调用点保持原来的短名字，避免出现第二套 if 链。
using studious::ui::engine::defaultPriority;
using studious::ui::engine::known;
using studious::ui::engine::sanitizePriority;
using studious::ui::engine::toRouter;
using studious::ui::engine::toRouterPriority;
using studious::ui::engine::toUi;

// 单实例的名字：QLocalServer 在 Windows 上就是命名管道名，别用带空格的路径。
constexpr const char *kInstanceName = "studious-single-instance";


QVariantMap entry(const QString &code, const QString &label)
{
    QVariantMap map;
    map.insert(QStringLiteral("code"), code);
    map.insert(QStringLiteral("label"), label);
    return map;
}

// 资产归类。依据是清单里的 family 和 id 前缀，不靠猜：
//   · opus-mt 家族、mt.opus.* 语言对、onnxruntime 运行时 → 本地 MT
//   · 其余（qwen 模型、llama.cpp 运行时）→ LLM


// LlmConfig <-> QVariantMap。QML 侧只认 QVariantMap，这里做双向转换，
// 键名跟 config::LlmConfig 的成员一一对应（QML 里用这些键读写）。
QVariantMap llmConfigToMap(const studious::config::LlmConfig &config)
{
    QVariantMap map;
    map.insert(QStringLiteral("baseUrl"), config.baseUrl);
    map.insert(QStringLiteral("apiKey"), config.apiKey);
    map.insert(QStringLiteral("model"), config.model);
    map.insert(QStringLiteral("timeoutMs"), config.timeoutMs);
    map.insert(QStringLiteral("maxTokens"), config.maxTokens);
    map.insert(QStringLiteral("temperature"), config.temperature);
    map.insert(QStringLiteral("systemPrompt"), config.systemPrompt);
    map.insert(QStringLiteral("retryBudgetMs"), config.retryBudgetMs);
    map.insert(QStringLiteral("reasoningEffort"), config.reasoningEffort);
    map.insert(QStringLiteral("batchSize"), config.batchSize);
    map.insert(QStringLiteral("stripModelNotes"), config.stripModelNotes);
    return map;
}

// 只覆盖 map 里出现的键，其余保持 base 的值 —— 这样 QML 可以只提交改过的字段。
studious::config::LlmConfig llmConfigFromMap(const QVariantMap &map,
                                             const studious::config::LlmConfig &base)
{
    studious::config::LlmConfig config = base;

    const auto str = [&map](const char *key, const QString &fallback) {
        const QVariant value = map.value(QLatin1String(key));
        return value.isValid() ? value.toString() : fallback;
    };

    config.baseUrl = str("baseUrl", config.baseUrl);
    config.apiKey = str("apiKey", config.apiKey);
    config.model = str("model", config.model);
    config.systemPrompt = str("systemPrompt", config.systemPrompt);
    config.reasoningEffort = str("reasoningEffort", config.reasoningEffort);

    if (map.contains(QStringLiteral("timeoutMs"))) {
        config.timeoutMs = map.value(QStringLiteral("timeoutMs")).toInt();
    }
    if (map.contains(QStringLiteral("maxTokens"))) {
        config.maxTokens = map.value(QStringLiteral("maxTokens")).toInt();
    }
    if (map.contains(QStringLiteral("temperature"))) {
        config.temperature = map.value(QStringLiteral("temperature")).toDouble();
    }
    if (map.contains(QStringLiteral("retryBudgetMs"))) {
        config.retryBudgetMs = map.value(QStringLiteral("retryBudgetMs")).toInt();
    }
    if (map.contains(QStringLiteral("batchSize"))) {
        config.batchSize = map.value(QStringLiteral("batchSize")).toInt();
    }
    if (map.contains(QStringLiteral("stripModelNotes"))) {
        config.stripModelNotes = map.value(QStringLiteral("stripModelNotes")).toBool();
    }
    return config;
}

QString assetCategory(const QString &id, const QString &family)
{
    // 视觉（OCR）是第三个家族：三件套 det/rec/字典，跟 LLM 权重完全无关。
    // 之前它落到 else 分支被归到 "llm"，于是资产页把 ocr.* 混进了本地 LLM 分组。
    if (family == QLatin1String("ppocr") || family == QLatin1String("ocr")
        || id.startsWith(QLatin1String("ocr."))) {
        return QStringLiteral("ocr");
    }
    if (family == QLatin1String("opus-mt")
        || id.startsWith(QLatin1String("mt.opus."))
        || id.contains(QLatin1String("onnxruntime"))) {
        return QStringLiteral("mt");
    }
    return QStringLiteral("llm");
}

// 路径与清单解析统一在 AssetPaths（资产根 / 清单来源 / 安装后复查只有一份实现）。
// 这里保留三个薄壳，调用点不动。
QString findManifestPath()
{
    return paths::manifestPath();
}

QString assetInstallRootPath()
{
    return paths::assetsRoot();
}

bool parseBundledManifest(studious::asset::Manifest *out)
{
    return paths::loadManifest(out);
}
// 跨盘迁移时的慢路径：递归复制目录内容（不保留属主，保留时间戳）。
bool copyDirRecursive(const QString &from, const QString &to)
{
    const QDir source(from);
    if (!QDir().mkpath(to)) {
        return false;
    }

    const QFileInfoList entries =
        source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirRecursive(entry.absoluteFilePath(), target)) {
                return false;
            }
            continue;
        }
        // 已存在就覆盖：迁移期间目标目录本该是空的，有残留说明上次半途而废。
        if (QFile::exists(target) && !QFile::remove(target)) {
            return false;
        }
        if (!QFile::copy(entry.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

// 把「设置里存的模型文件名」翻成 LocalEngineConfig.modelPath。// 空文件名必须原样传空串：LocalEngine::resolveModelPath() 的「空 = 第一个可用模型」
// 分支只在真正收到空串时才走。以前这里写的是 modelsDir() + "/" + fileName，空名字就变成
// "…\assets\models\"（一个目录），绕过那条分支后 llama-server 拿到 -m <目录>，
// 直接 gguf_init_from_file: failed to open GGUF file —— 界面就只剩一串英文日志。
QString localModelPathFrom(const QString &fileName)
{
    const QString trimmed = fileName.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QFileInfo info(trimmed);
    return info.isAbsolute() ? info.absoluteFilePath()
                             : QDir(LocalEngine::modelsDir()).filePath(trimmed);
}

// "model.qwen2.5-0.5b-instruct.q4km" → "qwen2.5-0.5b-instruct"
// 引擎列表的副标题、翻译页状态条都只有一行宽度，只留"能认出是哪个模型"的中间那段。
QString shortModelName(const QString &raw)
{
    QString name = raw.trimmed();
    if (name.isEmpty()) {
        return QString();
    }
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')), name.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0) {
        name = name.mid(slash + 1);
    }
    if (name.startsWith(QLatin1String("model."))) {
        name = name.mid(6);
    }
    for (const QLatin1String suffix : {QLatin1String(".q4km"), QLatin1String(".q8_0"),
                                      QLatin1String(".gguf"), QLatin1String(".bin")}) {
        if (name.endsWith(suffix)) {
            name.chop(static_cast<int>(suffix.size()));
            break;
        }
    }
    return name;
}

// 本地引擎实际会加载哪个模型：设置里选了就用它，选「自动」就是第一个可用模型。
QString localModelLabel(const AppSettings &settings)
{
    QString name = settings.localModelFileName.trimmed();
    if (name.isEmpty()) {
        name = LocalEngine::availableModels().value(0);
    }
    return shortModelName(name);
}

} // namespace

AppBackend::AppBackend(studious::cache::TranslationCache *cache, QObject *parent)
    : QObject(parent)
    , m_cache(cache)
    , m_job(new TranslateJob(this))
{
    // ---- 日志汇 ----
    // main() 一般已经装过（install 幂等）；这里兜底，保证「new AppBackend 就有日志」。
    logsink::install();

    // 新日志 → 一次 UI 刷新。节流放在这一侧（而不是 LogSink 里）：
    //   · handler 跑在任意线程，甚至一秒几千行；每行 emit 一次会让日志窗口疯狂重绘；
    //   · 这里先用 atomic 去重（同一时刻最多排一个 queued 调用），
    //     再用 200 ms 的成员 QTimer 合并，最坏 200 ms 刷一次 logText；
    //   · 跨线程必须 QueuedConnection：notify 可能来自工作线程，直接 emit 到 QML 会崩。
    m_logUiTimer = new QTimer(this);
    m_logUiTimer->setSingleShot(true);
    m_logUiTimer->setInterval(200);
    connect(m_logUiTimer, &QTimer::timeout, this, [this] {
        // 通知标志在这里才放下一个：合并窗口内的后续日志不会再排新调用，
        // 但它们的文本已经进了缓冲，emit 时读到的是最新全文。
        m_logNotifyPosted.store(false);
        if (!m_logDirty) {
            return;
        }
        m_logDirty = false;
        emit logTextChanged();
    });
    // 抓 QPointer 而不是裸 this：notify 可能正被工作线程拷走时主线程开始析构，
    // 那一份拷贝落地时对象已经没了。QPointer 判空是线程安全的，命中就安静丢掉。
    logsink::setNotify([self = QPointer<AppBackend>(this)] {
        if (self.isNull() || self->m_logNotifyPosted.exchange(true)) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self] {
                if (self.isNull()) {
                    return;
                }
                self->m_logDirty = true;
                if (self->m_logUiTimer != nullptr && !self->m_logUiTimer->isActive()) {
                    self->m_logUiTimer->start();
                }
            },
            Qt::QueuedConnection);
    });

    AppSettings settings = AppSettings::load();

    m_srcLang = langs::normalizeSource(settings.srcLang);
    m_dstLangs = langs::split(settings.dstLang);
    if (m_dstLangs.isEmpty()) {
        m_dstLangs = {QStringLiteral("zh")};
    }

    m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
    m_themeMode = settings.themeMode;
    m_fontScale = settings.fontScale;
    m_readingFontSize = settings.readingFontSize;
    m_uiFont = settings.fontUi;
    m_contentFont = settings.fontContent;
    // 启动时把选择打出来：查"字体没生效"这类问题时，先看这一行和 [font] choices。
    {
        const QStringList installed = QFontDatabase::families();
        const auto mark = [&installed](const QString &family) {
            if (family.isEmpty())
                return QStringLiteral("(auto)");
            if (family == QLatin1String("__system__"))
                return QStringLiteral("(system)");
            return installed.contains(family) ? family : family + QStringLiteral(" (MISSING)");
        };
        qInfo("[font] selected ui=%s content=%s",
              qUtf8Printable(mark(m_uiFont)), qUtf8Printable(mark(m_contentFont)));
    }
    // 热键默认空：装了就用默认组合键会跟别人的软件打架，宁可让用户自己录。
    m_hotkeyCapture = settings.hotkeyCapture;
    m_hotkeyMini = settings.hotkeyMini;
    m_hotkeyLive = settings.hotkeyLive;
    m_hotkeySelection = settings.hotkeySelection;
    m_hotkeyHome = settings.hotkeyHome;
    // 设置里空串 = 跟随系统，界面用 "system" 表达。
    m_uiLanguage = settings.uiLanguage.isEmpty() ? QStringLiteral("system")
                                                 : settings.uiLanguage;
    // 自动引擎顺序：设置里没有就按默认，有就整理一遍（防手改注册表）。
    m_enginePriority = sanitizePriority(settings.enginePriority);

    m_llmConfig = llmConfigToMap(settings.llm);
    m_webProviderId = settings.webProviderId;
    m_localModel = settings.localModelFileName;
    m_localUseGpu = settings.localUseGpu;
    m_localContextSize = settings.localContextSize;
    m_localThreads = settings.localThreads;
    m_localGpuLayers = settings.localGpuLayers;
    m_localExtraArgs = settings.localExtraArgs;
    m_localStopOnSwitch = settings.localStopOnSwitch;
    m_localPreheat = settings.localPreheat;
    m_mtPreload = settings.mtPreload;
    m_mtIdleMinutes = settings.mtIdleMinutes;
    m_localIdleStop = settings.localIdleStop;
    m_localIdleMinutes = settings.localIdleMinutes;
    // load() 已经过滤过非法值，这里拿到的要么是 "#RRGGBB"，要么是空（= 默认紫）。
    m_accentColor = settings.accentSeed;

    // ---- 设置页开关 ----
    m_cacheEnabled = settings.cacheEnabled;
    m_sameLangHint = settings.sameLangHint;
    m_suspiciousLangWarn = settings.suspiciousLangWarn;
    m_autoDetectSource = settings.autoDetectSource;
    m_proxyUrl = settings.proxyUrl;
    m_httpTimeoutMs = settings.httpTimeoutMs;
    m_minimizeToTray = settings.minimizeToTray;
    m_miniAlwaysOnTop = settings.miniAlwaysOnTop;
    m_captureAutoRun = settings.captureAutoRun;
    m_captureAutoCopy = settings.captureAutoCopy;
    m_captureAutoHide = settings.captureAutoHide;
    m_overlayOpacity = settings.overlayOpacity;
    m_overlayFontSize = settings.overlayFontSize;
    m_overlayKeepOriginal = settings.overlayKeepOriginal;
    m_overlayZoom = settings.overlayZoom;
    m_overlayMagnifier = settings.overlayMagnifier;
    m_magnifierCorner = settings.magnifierCorner;
    m_overlayDim = settings.overlayDim;
    m_overlayArrowNudge = settings.overlayArrowNudge;
    m_overlayAutoHideDelay = settings.overlayAutoHideDelay;
    m_overlayBackdrop = settings.overlayBackdrop;
    m_liveIntervalMs = settings.liveIntervalMs;
    m_liveOnlyOnChange = settings.liveOnlyOnChange;
    m_historyEnabled = settings.historyEnabled;
    m_closeToTray = settings.closeBehavior == QLatin1String("tray");
    m_singleInstance = settings.singleInstance;
    // 开机自启以注册表为准：用户可能在任务管理器 / 别的工具里删过，
    // 界面不该还显示"已开启"然后点了没反应。
    m_autoStart = studious::platform::Desktop::autoStartRegistered();

    // 网络设置在第一次请求之前就得落成环境变量 —— 后端的 HttpClient 只在构造时读。
    applyNetworkEnvironment();

    // 设置里存的就是界面 id（web / llm-cloud / llm-local / local-mt，空 = 自动）。
    // 老版本只存过 Router id "llm"，那个值是云地和本地共用的，只能靠
    // useLocalEngine 解开歧义 —— 这条兼容分支留着，其余全走 toUi。
    const QString rawEngineId = settings.engineId.trimmed().toLower();
    if (rawEngineId == QLatin1String("llm")) {
        m_engineId = settings.useLocalEngine ? QStringLiteral("llm-local")
                                             : QStringLiteral("llm-cloud");
    } else {
        // toUi() 已经把 "auto" 折成空串，这里只判空即可。
        const QString uiId = toUi(settings.engineId);
        if (!uiId.isEmpty()) {
            m_engineId = uiId;
        }
    }

    // 启动时就守着「本地 MT 只翻一个目标语言」这条规则：注册表里可能是上一次
    // 多选留下来的组合（比如 zh,ja），本地 MT 根本翻不了第二个。
    const QStringList normalized = langs::normalizeTargets(m_dstLangs, m_engineId);
    if (normalized != m_dstLangs) {
        m_dstLangs = normalized;
        m_activeDst = m_dstLangs.first();
        settings.dstLang = langs::join(m_dstLangs);
        settings.save();
    }

    m_statusText = i18n::s().common.ready;

    // 首次启动的示例原文：三段，段间用空行分隔（splitParagraphs 按空行切段）。
    // 用户输入不会覆盖它 —— 这里只做初始值。
    m_sourceText = QStringLiteral("Hello, world!\n"
                                  "\n"
                                  "This is a simple text translation test.\n"
                                  "\n"
                                  "The quick brown fox jumps over the lazy dog.");

    connect(m_job, &TranslateJob::finished, this, &AppBackend::onTranslationFinished);

    // ---- 单实例 / 托盘 ----
    // 先 ping 再 listen：活着的实例会被 ping 通，这时直接认作重复实例并退出；
    // 反之 ping 不通才去占名字（startSingleInstance 内部会顺手清死 socket）。
    // 顺序反过来的话，removeServer 会把正在跑的实例踢出命名管道表，多开就防不住了。
    if (m_singleInstance) {
        if (studious::platform::Desktop::pingRunningInstance(kInstanceName)) {
            m_duplicateInstance = true;
        } else if (!m_desktop.startSingleInstance(kInstanceName)) {
            setStatus(studious::i18n::s("占不上单实例通道，这次启动可能允许多开。",
                                        "Could not claim the single-instance channel; "
                                        "duplicate launches may be allowed this time."),
                      true);
        }
    }
    connect(&m_desktop,
            &studious::platform::Desktop::restoreRequested,
            this,
            &AppBackend::restoreWindowRequested);
    connect(&m_desktop,
            &studious::platform::Desktop::quitRequested,
            this,
            &AppBackend::quitApplication);
    syncTrayState();

    refreshAssets();
    // 本地 MT 的语言对表同样来自磁盘扫描，和资产一起在首屏前刷一次。
    refreshMtPairs();

    // 首屏就要有状态：构造函数里先探一次，别等用户去动语言选择器。
    refreshEngineStatuses();
    refreshCacheStats();

    // 工作线程发来的下载进度，排队回主线程再改成员（成员只归主线程所有）。
    connect(this,
            &AppBackend::assetProgressTick,
            this,
            [this](const QString &id, int percent) {
                QVariantMap updated = m_assetProgress;
                updated.insert(id, percent);
                m_assetProgress = updated;
                emit assetProgressChanged();
            },
            Qt::QueuedConnection);

    // 空闲自动停用：只在开关打开（且服务在跑）时有意义，其余时候定时器闲着。
    m_localIdleTimer = new QTimer(this);

    // 本地 MT 的空闲释放：和上面那个本地 LLM 的定时器同一套思路 ——
    // 常驻语言对闲着到点就放掉，内存还给系统。0 分钟 = 不自动释放。
    m_mtIdleTimer = new QTimer(this);
    m_mtIdleTimer->setSingleShot(true);
    connect(m_mtIdleTimer, &QTimer::timeout, this, [this] {
        if (studious::ui::mtLoadedPairs() <= 0) {
            return;
        }
        studious::ui::unloadMtAll();
        qInfo("[mt] idle for %d min - resident pairs released", m_mtIdleMinutes);
        emit mtStateChanged();
    });

    // 「用一次就续命」：本地 MT 每翻完一批，把空闲计时器往后推。
    studious::ui::setMtUsedHook([this] {
        touchMtIdle();
        emit mtStateChanged();
    });

    // 截图 / 实时 / 小窗选中本地 LLM 时，由 QuickTranslate 触发这个动作把服务拉起来
    // （异步，不阻塞界面），调用方会自己重试。
    studious::ui::setLocalEngineStarter([this] { startLocalServer(); });

    // 开机自启：把已安装的语言对全部预热好。放到工作线程，别跟首屏抢 CPU。
    if (m_mtPreload && mtPacksInstalled()) {
        QTimer::singleShot(1500, this, [this] {
            (void)QtConcurrent::run([] {
                const int loaded = studious::ui::warmMtAll();
                qInfo("[mt] preload on start: %d pair(s) resident", loaded);
            });
            touchMtIdle();
        });
    }
    m_localIdleTimer->setSingleShot(true);
    connect(m_localIdleTimer, &QTimer::timeout, this, [this] {
        if (!m_localIdleStop || !studious::ui::sharedLocalEngine().isRunning()) {
            return;
        }
        shutdownLocalEngine(studious::i18n::sf("本地引擎已空闲 %1 分钟，服务已自动停止。",
                                               "Local engine idle for %1 min - service stopped automatically.",
                                               {QString::number(m_localIdleMinutes)}));
    });

    // 启动预热：只在用户明确打开时做，而且延迟 1.5 秒 —— 别跟首屏抢 CPU，
    // 也别让 llama-server 的日志输出插到窗口还没画出来的那一段。
    // 另外：上次退出时引擎就停在本地 LLM，这次开局同样把它拉起来 ——
    // 「选中本地 LLM 就启动」这条规则对"记住的选择"也要成立，否则用户
    // 打开软件看到的是"本地 LLM（未就绪）"，第一次翻译还得再等一次冷启动。
    if (m_localPreheat || m_engineId == QLatin1String("llm-local")) {
        QTimer::singleShot(1500, this, [this] {
            if (studious::ui::sharedLocalEngine().isRunning()) {
                return;
            }
            startLocalServer();
        });
    }
}

AppBackend::~AppBackend()
{
    // 注销日志回调：退出阶段 logsink 可能还在别的线程打最后几行，
    // 不能再往一个正在拆的 QObject 上投事件。
    logsink::setNotify(nullptr);
}

// ---------------------------------------------------------------- 属性 setter

void AppBackend::setSourceText(const QString &text)
{
    if (m_sourceText == text) {
        return;
    }
    m_sourceText = text;
    emit sourceTextChanged();
}

void AppBackend::setSrcLang(const QString &code)
{
    const QString normalized = langs::normalizeSource(code);
    if (m_srcLang == normalized) {
        return;
    }
    m_srcLang = normalized;
    saveSettings();
    emit srcLangChanged();
    refreshEngineStatuses();
}

void AppBackend::setDstLangs(const QStringList &codes)
{
    // 空兜底 + 本地 MT 收成单选，规则只在 langs::normalizeTargets() 里写一遍。
    // 界面已按 dstMultiSupported 禁掉多选，这里兜住其它入口（配置页、设置、脚本）。
    const QStringList next = langs::normalizeTargets(codes, m_engineId);
    if (m_dstLangs == next) {
        return;
    }
    m_dstLangs = next;
    // 当前 tab 若已被取消勾选，回落到第一个目标语言。
    if (!m_dstLangs.contains(m_activeDst)) {
        m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
        emit activeDstChanged();
    }
    saveSettings();
    emit dstLangsChanged();
    emit blocksChanged();
    refreshEngineStatuses();
}

void AppBackend::reloadLanguagesFromSettings()
{
    // 小窗那侧改语言时直接写 AppSettings（它有自己的 MiniSession），改完由 QML
    // 通知到这里，把主页/截屏/实时共用的这份内存值同步过来。反向同步在
    // Main.qml 的 Connections 里：backend 变了就调 miniSession.reloadLanguagesFromSettings()。
    const AppSettings settings = AppSettings::load();
    const QString src = langs::normalizeSource(settings.srcLang);
    const QStringList dsts = langs::normalizeTargets(langs::split(settings.dstLang), m_engineId);

    if (m_srcLang != src) {
        m_srcLang = src;
        emit srcLangChanged();
    }
    if (m_dstLangs != dsts) {
        m_dstLangs = dsts;
        if (!m_dstLangs.contains(m_activeDst)) {
            m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
            emit activeDstChanged();
        }
        emit dstLangsChanged();
        emit blocksChanged();
    }
    refreshEngineStatuses();
}

void AppBackend::setEngineId(const QString &id)
{
    // 只收界面 id；"auto"/空 = 自动。老拼法（llm / localmt 等）由 EngineIds 折平。
    const QString next = toUi(id);
    if (m_engineId == next) {
        return;
    }
    m_engineId = next;
    saveSettings();
    emit engineIdChanged();
    // 换到本地 MT 时把多选的目标语言收成一个 —— 否则下一次翻译会因为
    // "某个目标语言没有对应模型"整单失败，界面上还会留着几个用不上的标签。
    const QStringList clamped = langs::normalizeTargets(m_dstLangs, next);
    if (clamped != m_dstLangs) {
        m_dstLangs = clamped;
        m_activeDst = m_dstLangs.first();
        saveSettings();
        emit dstLangsChanged();
        emit activeDstChanged();
        emit blocksChanged();
    }
    // 「切换引擎时关闭」：用户选的，默认关 —— 关着就保留 llama-server 热着，下次切回来直接翻。
    if (m_localStopOnSwitch && next != QLatin1String("llm-local") && studious::ui::sharedLocalEngine().isRunning()) {
        shutdownLocalEngine(QString());
    }
    // 选中本地 LLM 就把服务拉起来：不然用户第一次翻译还得先吃一次冷启动
    // （模型大的时候十几秒，看起来像"卡住了"）。缺资产时 runLocalServerStart()
    // 会自己给出「去资产页下载」的提示，不会误拉进程 —— 但提示只闪在状态栏里，
    // 日志里补一条现场，方便事后判断"点了没反应"是不是缺运行时。
    if (next == QLatin1String("llm-local") && !llamaRuntimeInstalled()) {
        qWarning("[engine] AppBackend::setEngineId: llm-local selected but llama runtime asset is missing "
                 "(installRoot=%s)",
                 qUtf8Printable(assetInstallRootPath()));
    }
    if (next == QLatin1String("llm-local") && !studious::ui::sharedLocalEngine().isRunning()) {
        startLocalServer();
    }
    refreshEngineStatuses();
}

// ------------------------------------------------------------------ 只读清单

QVariantList AppBackend::languages() const
{
    QVariantList list;
    list.append(entry(QStringLiteral("auto"), i18n::s().translate.autoDetect));
    for (const QString &code : studious::core::LanguageRegistry::allInternal()) {
        if (code == QLatin1String("auto")) {
            continue;
        }
        list.append(entry(code, studious::core::LanguageRegistry::displayName(code)));
    }
    return list;
}

QVariantList AppBackend::engines() const
{
    QVariantList list;
    list.append(entry(QString(), i18n::s().translate.engineAuto));
    list.append(entry(QStringLiteral("web"), i18n::s().translate.engineWeb));
    list.append(entry(QStringLiteral("llm-cloud"), i18n::s().translate.engineLlmCloud));
    list.append(entry(QStringLiteral("llm-local"), i18n::s().translate.engineLlmLocal));
    list.append(entry(QStringLiteral("local-mt"), i18n::s().translate.engineMt));
    return list;
}

int AppBackend::cacheEntries() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->count()) : 0;
}

int AppBackend::cacheHits() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->stats().hits) : 0;
}

int AppBackend::cacheMisses() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->stats().misses) : 0;
}

int AppBackend::paragraphCount() const
{
    return splitParagraphs(m_sourceText).size();
}

int AppBackend::charCount() const
{
    return m_sourceText.length();
}

QString AppBackend::dstLabel() const
{
    QStringList names;
    names.reserve(m_dstLangs.size());
    for (const QString &code : m_dstLangs) {
        names << studious::core::LanguageRegistry::displayName(code);
    }
    return names.join(QStringLiteral(" + "));
}

void AppBackend::setShowSource(bool on)
{
    // 主界面已经没有这个开关了，但小窗口/实时翻译贴纸要不要带原文还得看它，
    // 所以 API 留着（只做状态与通知）。
    if (m_showSource == on) {
        return;
    }
    m_showSource = on;
    emit showSourceChanged();
}

QVariantList AppBackend::blocks() const
{    QVariantList list;
    const QStringList translated = m_byLang.value(m_activeDst);
    const int count = m_lastParagraphs.isEmpty() ? translated.size() : m_lastParagraphs.size();
    list.reserve(count);

    for (int i = 0; i < count; ++i) {
        const QString text = translated.value(i);
        QVariantMap block;
        block.insert(QStringLiteral("index"), i);
        block.insert(QStringLiteral("source"), m_lastParagraphs.value(i));
        block.insert(QStringLiteral("text"), text);
        // 后端只给整次请求一个 passthrough 标志（源 == 目标时原样返回），
        // 所以逐段徽标也按整次请求的状态来。
        block.insert(QStringLiteral("passthrough"), m_lastPassthrough);
        block.insert(QStringLiteral("empty"), text.trimmed().isEmpty());
        list.append(block);
    }
    return list;
}

void AppBackend::setActiveDst(const QString &code)
{
    if (m_activeDst == code || !m_dstLangs.contains(code)) {
        return;
    }
    m_activeDst = code;
    emit activeDstChanged();
    emit blocksChanged();
}

void AppBackend::setThemeMode(const QString &mode)
{
    const QString next = (mode == QLatin1String("light") || mode == QLatin1String("dark"))
                             ? mode
                             : QStringLiteral("system");
    if (m_themeMode == next) {
        return;
    }
    m_themeMode = next;
    AppSettings settings = AppSettings::load();
    settings.themeMode = next;
    settings.save();
    emit themeModeChanged();
}

void AppBackend::setReadingFontSize(int px)
{
    const int next = std::clamp(px, 11, 24);
    if (m_readingFontSize == next) {
        return;
    }
    m_readingFontSize = next;

    AppSettings settings = AppSettings::load();
    settings.readingFontSize = next;
    settings.save();

    // 和 fontScale 共用同一个通知：QML 里两者都是"字号变了要重排"。
    emit fontScaleChanged();
}

// 字体族名白名单：项目自带的两个（界面默认 / 译文默认）+ 系统默认 + 空（等于界面默认）
// + 系统里真实存在的族名。枚举系统族名是用户明确要求的：字体下拉里除自带那两个，
// 其余都应该是这台机器上装了的字体。
static bool knownFontChoice(const QString &code)
{
    if (code.isEmpty() || code == QLatin1String("__system__")
        || code == QLatin1String("软件默认-思源黑体")
        || code == QLatin1String("软件默认-霞鹜文楷")) {
        return true;
    }
    // 等宽那份仍在项目里（热键框、日志窗口、路径都用它），但不放进下拉：
    // 界面/译文正文用等宽字体没有意义。
    static const QSet<QString> families = [] {
        QSet<QString> set;
        const QStringList all = QFontDatabase::families();
        for (const QString &family : all) {
            set.insert(family);
        }
        return set;
    }();
    return families.contains(code);
}

QVariantList AppBackend::fontChoices() const
{
    QVariantList list;
    auto add = [&list](const QString &code, const QString &label) {
        QVariantMap entry;
        entry.insert(QStringLiteral("code"), code);
        entry.insert(QStringLiteral("label"), label);
        list.append(entry);
    };

    // 前两项是项目自带的两套（界面 = 思源黑体，译文 = 霞鹜文楷），也就是默认值。
    add(QStringLiteral("软件默认-思源黑体"), studious::i18n::s("软件默认-思源黑体", "Software default - Source Han Sans SC"));
    add(QStringLiteral("软件默认-霞鹜文楷"), studious::i18n::s("软件默认-霞鹜文楷", "Software default - LXGW WenKai"));
    add(QStringLiteral("__system__"), studious::i18n::s("系统默认字体", "System default font"));

    // 其余全部来自系统已安装字体，按名字排序，方便找。
    QStringList families = QFontDatabase::families();
    families.sort(Qt::CaseInsensitive);
    for (const QString &family : families) {
        if (family == QLatin1String("软件默认-思源黑体")
            || family == QLatin1String("软件默认-霞鹜文楷")
            || family == QLatin1String("软件默认-霞鹜文楷等宽")) {
            continue;
        }
        add(family, family);
    }
    qInfo("[font] choices: %lld (2 bundled + system default + %lld system families)",
          static_cast<long long>(list.size()), static_cast<long long>(list.size() - 3));
    return list;
}

void AppBackend::setUiFont(const QString &family)
{
    const QString next = family.trimmed();
    if (!knownFontChoice(next) || m_uiFont == next) {
        return;
    }
    m_uiFont = next;

    AppSettings settings = AppSettings::load();
    settings.fontUi = next;
    settings.save();

    emit fontsChanged();
}

void AppBackend::setContentFont(const QString &family)
{
    const QString next = family.trimmed();
    if (!knownFontChoice(next) || m_contentFont == next) {
        return;
    }
    m_contentFont = next;

    AppSettings settings = AppSettings::load();
    settings.fontContent = next;
    settings.save();

    emit fontsChanged();
}

// 组合键的规范形："Ctrl+Alt+Shift+Win+键"，修饰键顺序固定、字母大写。
// 平台层（HotkeyWin::parseComboText）按 "+" 拆段、大小写不敏感，但顺序很重要：
// 界面上显示的和注册的必须是同一串，否则"改了没生效"这种问题很难查。
QString AppBackend::normalizeHotkey(const QString &combo)
{
    const QString trimmed = combo.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    QString keyPart;

    const QStringList parts = trimmed.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString part = raw.trimmed();
        const QString upper = part.toUpper();
        if (upper == QLatin1String("CTRL") || upper == QLatin1String("CONTROL")) {
            ctrl = true;
        } else if (upper == QLatin1String("ALT")) {
            alt = true;
        } else if (upper == QLatin1String("SHIFT")) {
            shift = true;
        } else if (upper == QLatin1String("WIN") || upper == QLatin1String("META")
                   || upper == QLatin1String("SUPER")) {
            meta = true;
        } else if (!part.isEmpty()) {
            // 只认最后一段作为主键；出现第二个就说明这串是坏的。
            if (!keyPart.isEmpty()) {
                return QString();
            }
            keyPart = part.toUpper();
        }
    }

    if (keyPart.isEmpty()) {
        return QString();
    }

    // 裸键只有 F13~F24 允许（平台层同一条规则：普通字母当全局热键会把输入法吃掉）。
    const bool functionRow = keyPart.startsWith(QLatin1Char('F'))
        && keyPart.mid(1).toInt() >= 13 && keyPart.mid(1).toInt() <= 24;
    if (!ctrl && !alt && !shift && !meta && !functionRow) {
        return QString();
    }

    QStringList ordered;
    if (ctrl) {
        ordered << QStringLiteral("Ctrl");
    }
    if (alt) {
        ordered << QStringLiteral("Alt");
    }
    if (shift) {
        ordered << QStringLiteral("Shift");
    }
    if (meta) {
        ordered << QStringLiteral("Win");
    }
    ordered << keyPart;
    return ordered.join(QLatin1Char('+'));
}

void AppBackend::setHotkeyStatus(const QString &id, bool ok, const QString &message)
{
    m_hotkeyStatus.insert(id, QVariantMap{{QStringLiteral("ok"), ok},
                                          {QStringLiteral("message"), message}});
    emit hotkeyStatusChanged();
}

void AppBackend::setHotkeyCapture(const QString &combo)
{
    const QString next = normalizeHotkey(combo);
    if (m_hotkeyCapture == next) {
        return;
    }
    m_hotkeyCapture = next;

    AppSettings settings = AppSettings::load();
    settings.hotkeyCapture = next;
    settings.save();

    emit hotkeysChanged();
}

void AppBackend::setHotkeyMini(const QString &combo)
{
    const QString next = normalizeHotkey(combo);
    if (m_hotkeyMini == next) {
        return;
    }
    m_hotkeyMini = next;

    AppSettings settings = AppSettings::load();
    settings.hotkeyMini = next;
    settings.save();

    emit hotkeysChanged();
}

void AppBackend::setHotkeyLive(const QString &combo)
{
    const QString next = normalizeHotkey(combo);
    if (m_hotkeyLive == next) {
        return;
    }
    m_hotkeyLive = next;

    AppSettings settings = AppSettings::load();
    settings.hotkeyLive = next;
    settings.save();

    emit hotkeysChanged();
}

void AppBackend::setHotkeySelection(const QString &combo)
{
    const QString next = normalizeHotkey(combo);
    if (m_hotkeySelection == next) {
        return;
    }
    m_hotkeySelection = next;

    AppSettings settings = AppSettings::load();
    settings.hotkeySelection = next;
    settings.save();

    emit hotkeysChanged();
}

void AppBackend::setHotkeyHome(const QString &combo)
{
    const QString next = normalizeHotkey(combo);
    if (m_hotkeyHome == next) {
        return;
    }
    m_hotkeyHome = next;

    AppSettings settings = AppSettings::load();
    settings.hotkeyHome = next;
    settings.save();

    emit hotkeysChanged();
}

void AppBackend::setFontScale(int percent)
{    // 夹在 80~150，注册表被手改成离谱值时也不会把界面撑爆。
    const int next = std::clamp(percent, 80, 150);
    if (m_fontScale == next) {
        return;
    }
    m_fontScale = next;
    AppSettings settings = AppSettings::load();
    settings.fontScale = next;
    settings.save();
    emit fontScaleChanged();
}

QString AppBackend::accentColor() const
{
    // 空 = 用默认紫。写入口已规范化，这里只做兜底。
    return m_accentColor.isEmpty() ? QStringLiteral("#6750A4") : m_accentColor;
}

void AppBackend::setAccentColor(const QString &color)
{
    const QColor parsed(color.trimmed());
    // 非法值不改配色：否则注册表里一个手误就让整套主题哑掉。
    // QML 侧的输入框标红是它自己的事，后端永远保留最后一次有效值。
    if (!parsed.isValid()) {
        return;
    }
    // 统一成大写 "#RRGGBB"，QML 里比较选中态就不用管大小写。
    const QString normalized = parsed.name(QColor::HexRgb).toUpper();
    if (m_accentColor == normalized) {
        return;
    }
    m_accentColor = normalized;

    AppSettings settings = AppSettings::load();
    settings.accentSeed = m_accentColor;
    settings.save();

    emit accentColorChanged();
}

// ------------------------------------------------------- 设置页：翻译行为开关

void AppBackend::setCacheEnabled(bool on)
{
    if (m_cacheEnabled == on) {
        return;
    }
    m_cacheEnabled = on;

    AppSettings settings = AppSettings::load();
    settings.cacheEnabled = on;
    settings.save();

    // 关缓存不改已存内容，只是以后不再查 / 不再写；打开时条数照旧。
    emit prefsChanged();
    refreshCacheStats();
}

void AppBackend::setSameLangHint(bool on)
{
    if (m_sameLangHint == on) {
        return;
    }
    m_sameLangHint = on;

    AppSettings settings = AppSettings::load();
    settings.sameLangHint = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setSuspiciousLangWarn(bool on)
{
    if (m_suspiciousLangWarn == on) {
        return;
    }
    m_suspiciousLangWarn = on;

    AppSettings settings = AppSettings::load();
    settings.suspiciousLangWarn = on;
    settings.save();

    emit prefsChanged();
    // 原始警告一直攒在 m_statusWarning 里，getter 按开关过滤，
    // 所以这里只要通知一次，界面就能立刻显 / 隐。
    emit statusWarningChanged();
}

void AppBackend::setAutoDetectSource(bool on)
{
    if (m_autoDetectSource == on) {
        return;
    }
    m_autoDetectSource = on;

    AppSettings settings = AppSettings::load();
    settings.autoDetectSource = on;
    settings.save();

    // 关掉自动检测时如果当前就停在 auto，得给个具体语言，否则翻译会被
    // 「本地 MT 需要明确的源语言」这类校验直接拒掉。
    if (!on && langs::isAuto(m_srcLang)) {
        const QString detected = detectLanguage(m_sourceText);
        setSrcLang(detected.isEmpty() ? QStringLiteral("en") : detected);
    }

    emit prefsChanged();
}

// --------------------------------------------------------- 设置页：网络

void AppBackend::applyNetworkEnvironment()
{
    // 代理：空串 = 不覆盖，让后端继续按 HTTPS_PROXY / HTTP_PROXY / ALL_PROXY 探测。
    if (m_proxyUrl.isEmpty()) {
        qunsetenv("STUDIOUS_HTTP_PROXY");
    } else {
        qputenv("STUDIOUS_HTTP_PROXY", m_proxyUrl.toUtf8());
    }

    // 超时是"全局下限"：后端只往上抬各调用点自己的超时，回环地址不受影响。
    qputenv("STUDIOUS_HTTP_TIMEOUT", QByteArray::number(m_httpTimeoutMs));
}

void AppBackend::setProxyUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (m_proxyUrl == trimmed) {
        return;
    }
    m_proxyUrl = trimmed;

    AppSettings settings = AppSettings::load();
    settings.proxyUrl = m_proxyUrl;
    settings.save();

    applyNetworkEnvironment();
    emit prefsChanged();
    // 说清楚生效时机：QtHttpClient 在构造时读环境变量，已建的实例不会回溯。
    setStatus(trimmed.isEmpty() ? i18n::s().common.ready
                                : studious::i18n::s("代理已更新，下一次网络请求生效。",
                                                    "Proxy updated; applies to the next request."),
              false);
}

void AppBackend::setHttpTimeoutMs(int ms)
{
    const int clamped = std::clamp(ms, 1000, 600000);
    if (m_httpTimeoutMs == clamped) {
        return;
    }
    m_httpTimeoutMs = clamped;

    AppSettings settings = AppSettings::load();
    settings.httpTimeoutMs = clamped;
    settings.save();

    applyNetworkEnvironment();
    emit prefsChanged();
}

// ----------------------------------------------------- 设置页：桌面集成

void AppBackend::setAutoStart(bool on)
{
    if (m_autoStart == on) {
        return;
    }

    QString error;
    if (!studious::platform::Desktop::setAutoStart(on, &error)) {
        // 失败不改内存值也不落盘：界面上那个开关应该自己弹回去。
        setStatus(error, true);
        emit prefsChanged();
        return;
    }

    m_autoStart = on;

    AppSettings settings = AppSettings::load();
    settings.autoStart = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setMinimizeToTray(bool on)
{
    if (m_minimizeToTray == on) {
        return;
    }
    m_minimizeToTray = on;

    AppSettings settings = AppSettings::load();
    settings.minimizeToTray = on;
    settings.save();

    syncTrayState();
    emit prefsChanged();
}

void AppBackend::setMiniAlwaysOnTop(bool on)
{
    if (m_miniAlwaysOnTop == on) {
        return;
    }
    m_miniAlwaysOnTop = on;

    AppSettings settings = AppSettings::load();
    settings.miniAlwaysOnTop = on;
    settings.save();

    emit prefsChanged();
}

// ---- 截屏翻译（A 页）：三个开关 + 浮层两个滑块 ----
// 这一组只存不改行为，行为在 QML 的浮层与 ScreenFlow 里读它。
void AppBackend::setCaptureAutoRun(bool on)
{
    if (m_captureAutoRun == on) {
        return;
    }
    m_captureAutoRun = on;

    AppSettings settings = AppSettings::load();
    settings.captureAutoRun = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setCaptureAutoCopy(bool on)
{
    if (m_captureAutoCopy == on) {
        return;
    }
    m_captureAutoCopy = on;

    AppSettings settings = AppSettings::load();
    settings.captureAutoCopy = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setCaptureAutoHide(bool on)
{
    if (m_captureAutoHide == on) {
        return;
    }
    m_captureAutoHide = on;

    AppSettings settings = AppSettings::load();
    settings.captureAutoHide = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayOpacity(int percent)
{
    const int next = std::clamp(percent, 40, 100);
    if (m_overlayOpacity == next) {
        return;
    }
    m_overlayOpacity = next;

    AppSettings settings = AppSettings::load();
    settings.overlayOpacity = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayFontSize(int px)
{
    const int next = std::clamp(px, 10, 20);
    if (m_overlayFontSize == next) {
        return;
    }
    m_overlayFontSize = next;

    AppSettings settings = AppSettings::load();
    settings.overlayFontSize = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayKeepOriginal(bool on)
{
    if (m_overlayKeepOriginal == on) {
        return;
    }
    m_overlayKeepOriginal = on;

    AppSettings settings = AppSettings::load();
    settings.overlayKeepOriginal = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayZoom(int times)
{
    const int next = std::clamp(times, 2, 8);
    if (m_overlayZoom == next) {
        return;
    }
    m_overlayZoom = next;

    AppSettings settings = AppSettings::load();
    settings.overlayZoom = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayMagnifier(bool on)
{
    if (m_overlayMagnifier == on) {
        return;
    }
    m_overlayMagnifier = on;

    AppSettings settings = AppSettings::load();
    settings.overlayMagnifier = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setMagnifierCorner(const QString &corner)
{
    // 与 AppSettings::load() 里的白名单一致：只认四个角，别的一律不落盘。
    static const QStringList kCorners{QStringLiteral("bottomRight"), QStringLiteral("bottomLeft"),
                                      QStringLiteral("topRight"), QStringLiteral("topLeft")};
    const QString next = corner.trimmed();
    if (!kCorners.contains(next) || m_magnifierCorner == next) {
        return;
    }
    m_magnifierCorner = next;

    AppSettings settings = AppSettings::load();
    settings.magnifierCorner = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayDim(int percent)
{
    const int next = std::clamp(percent, 20, 80);
    if (m_overlayDim == next) {
        return;
    }
    m_overlayDim = next;

    AppSettings settings = AppSettings::load();
    settings.overlayDim = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayArrowNudge(bool on)
{
    if (m_overlayArrowNudge == on) {
        return;
    }
    m_overlayArrowNudge = on;

    AppSettings settings = AppSettings::load();
    settings.overlayArrowNudge = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayAutoHideDelay(int seconds)
{
    const int next = std::clamp(seconds, 0, 10);
    if (m_overlayAutoHideDelay == next) {
        return;
    }
    m_overlayAutoHideDelay = next;

    AppSettings settings = AppSettings::load();
    settings.overlayAutoHideDelay = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setOverlayBackdrop(int percent)
{
    const int next = std::clamp(percent, 0, 100);
    if (m_overlayBackdrop == next) {
        return;
    }
    m_overlayBackdrop = next;

    AppSettings settings = AppSettings::load();
    settings.overlayBackdrop = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setLiveIntervalMs(int ms)
{
    const int next = std::clamp(ms, 400, 1500);
    if (m_liveIntervalMs == next) {
        return;
    }
    m_liveIntervalMs = next;

    AppSettings settings = AppSettings::load();
    settings.liveIntervalMs = next;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setLiveOnlyOnChange(bool on)
{
    if (m_liveOnlyOnChange == on) {
        return;
    }
    m_liveOnlyOnChange = on;

    AppSettings settings = AppSettings::load();
    settings.liveOnlyOnChange = on;
    settings.save();

    emit prefsChanged();
}

void AppBackend::setHistoryEnabled(bool on)
{
    if (m_historyEnabled == on) {
        return;
    }
    m_historyEnabled = on;

    AppSettings settings = AppSettings::load();
    settings.historyEnabled = on;
    settings.save();

    // 打开开关时顺手把目录建好：用户下一步多半就是去「打开目录」看看有没有东西。
    if (on) {
        QDir().mkpath(historyDir());
    }

    emit prefsChanged();
}

QString AppBackend::historyDir() const
{
    // 设置里存的是"用户填过的路径"；空串表示用默认位置。
    // 默认放在资产目录旁边而不是 %APPDATA%/studious 根，是为了"迁移资产根"时历史能一起搬走。
    const QString configured = AppSettings::load().historyDir.trimmed();
    if (!configured.isEmpty()) {
        return QDir::cleanPath(configured);
    }
    return QDir::cleanPath(QDir(assetInstallRootPath()).filePath(QStringLiteral("history")));
}

void AppBackend::openHistoryDir()
{
    const QString dir = historyDir();
    if (!QDir().mkpath(dir)) {
        qWarning("[history] AppBackend::openHistoryDir: mkpath failed: %s", qUtf8Printable(dir));
        setStatus(studious::i18n::s("建不出历史目录：%1", "Could not create the history folder: %1").arg(dir), true);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        qWarning("[history] AppBackend::openHistoryDir: openUrl failed: %s", qUtf8Printable(dir));
        setStatus(studious::i18n::s("打不开这个目录（可能已被删除，或被系统策略拦住）。",
                                    "Cannot open the folder (missing, or blocked by policy)."),
                  true);
    }
}

void AppBackend::setCloseToTray(bool on)
{
    if (m_closeToTray == on) {
        return;
    }
    m_closeToTray = on;

    AppSettings settings = AppSettings::load();
    settings.closeBehavior = on ? QStringLiteral("tray") : QStringLiteral("exit");
    settings.save();

    syncTrayState();
    emit prefsChanged();
}

void AppBackend::setSingleInstance(bool on)
{
    if (m_singleInstance == on) {
        return;
    }
    m_singleInstance = on;

    AppSettings settings = AppSettings::load();
    settings.singleInstance = on;
    settings.save();

    emit prefsChanged();
    // 当前进程的本地 socket 已经绑好了，改这个只能下次启动生效 —— 明说，别让人以为没生效。
    setStatus(studious::i18n::s("单实例设置已保存，重启后生效。",
                                "Single-instance setting saved; takes effect after a restart."),
              false);
}

bool AppBackend::trayAvailable()
{
    return m_desktop.trayAvailable();
}

void AppBackend::syncTrayState()
{
    const bool needed = m_minimizeToTray || m_closeToTray;
    m_desktop.setTrayEnabled(needed);
    if (needed && !m_desktop.trayAvailable()) {
        setStatus(studious::i18n::s("这个会话里装不上托盘图标，托盘相关设置暂时无效。",
                                    "Tray icon is unavailable in this session; tray settings are ignored."),
                  true);
    }
}

// --------------------------------------------------------- 设置页：数据

void AppBackend::openPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        qWarning("[shell] AppBackend::openPath: empty path ignored");
        return;
    }
    const QFileInfo info(trimmed);
    const QString target = info.isDir() ? info.absoluteFilePath()
                                        : info.absolutePath();   // 文件就打开它所在的目录
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target))) {
        qWarning("[shell] AppBackend::openPath: openUrl failed: %s", qUtf8Printable(target));
        setStatus(studious::i18n::s("打不开这个目录（可能已被删除，或被系统策略拦住）。",
                                    "Cannot open the folder (missing, or blocked by policy)."),
                  true);
    }
}

int AppBackend::clearOldLogs(int days)
{
    const int keepDays = std::max(1, days);
    const QDir logDir(QDir(assetInstallRootPath()).filePath(QStringLiteral("logs")));
    if (!logDir.exists()) {
        return 0;
    }

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-keepDays);
    int removed = 0;
    const QFileInfoList entries =
        logDir.entryInfoList({QStringLiteral("*.log")}, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &entry : entries) {
        if (entry.lastModified() >= cutoff) {
            continue;
        }
        if (QFile::remove(entry.absoluteFilePath())) {
            ++removed;
        }
    }

    setStatus(studious::i18n::sf("已清理 %1 个 %2 天前的日志文件。",
                                 "Removed %1 log file(s) older than %2 days.",
                                 {removed, keepDays}),
              false);
    return removed;
}

void AppBackend::resetApplication()
{
    // 先把本地服务停干净：它握着模型文件，重启后新旧进程抢同一份会很别扭。
    shutdownLocalEngine(QString());

    // 清配置（QSettings 会一起清掉语言、主题、强调色、引擎选择…），
    // 再清翻译缓存。资产目录刻意不动 —— 那是几 GB 的下载，不是"配置"。
    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));
    store.clear();
    store.sync();

    clearCache();

    restartApplication();
}

void AppBackend::quitApplication()
{
    // 退出前把托盘摘掉，否则任务栏通知区会留一个点不动的幽灵图标，
    // 要等鼠标划过去才消失。
    m_desktop.shutdown();
    QCoreApplication::quit();
}

bool AppBackend::migrateAssetRoot(const QString &newRoot)
{
    const QString target = QDir::cleanPath(newRoot.trimmed());
    const QString current = QDir::cleanPath(assetInstallRootPath());

    if (target.isEmpty()) {
        return false;
    }
    if (target == current) {
        setStatus(studious::i18n::s("新目录和当前目录是同一个。", "The new folder is the same as the current one."),
                  true);
        return false;
    }
    // 不允许把根目录挪进自己里面，否则搬着搬着就把源目录搬没了。
    if (target.startsWith(current + QLatin1Char('/'))) {
        setStatus(studious::i18n::s("新目录不能是当前目录的子目录。",
                                    "The new folder cannot be inside the current one."),
                  true);
        return false;
    }
    if (QDir(target).exists() && !QDir(target).isEmpty()) {
        setStatus(studious::i18n::s("新目录必须是空目录（或不存在）。",
                                    "The target folder must be empty or not exist yet."),
                  true);
        return false;
    }

    // 模型 / 运行时都握在本地服务手里，搬之前必须停。
    shutdownLocalEngine(QString());

    if (!QDir().mkpath(target)) {
        setStatus(studious::i18n::s("建不了目标目录，检查权限或磁盘。",
                                    "Cannot create the target folder — check permissions or disk."),
                  true);
        return false;
    }

    const QDir source(current);
    const QStringList subdirs = {QStringLiteral("runtime"),
                                 QStringLiteral("models"),
                                 QStringLiteral("downloads"),
                                 QStringLiteral("logs")};

    // 同盘 rename 是瞬时的；跨盘会失败（EXDEV），退回 copy + remove 的慢路径。
    for (const QString &name : subdirs) {
        if (!source.exists(name)) {
            continue;
        }
        const QString from = source.filePath(name);
        const QString to = QDir(target).filePath(name);
        if (QDir().rename(from, to)) {
            continue;
        }
        if (QDir(from).exists()) {
            copyDirRecursive(from, to);
        }
        QDir(from).removeRecursively();
    }

    // 落盘新根目录，然后重扫一遍资产表与语言对表（它们都按根目录算路径）。
    AppSettings settings = AppSettings::load();
    settings.assetRoot = target;
    settings.save();

    refreshAssets();
    refreshMtPairs();
    refreshLocalState();

    setStatus(studious::i18n::sf("安装目录已迁移到 %1。",
                                 "Install folder moved to %1.",
                                 {QDir::toNativeSeparators(target)}),
              false);
    return true;
}

void AppBackend::importAssetFromFile(const QString &assetId, const QString &filePath)
{
    const QString id = assetId.trimmed();
    const QString path = filePath.trimmed();
    if (id.isEmpty() || path.isEmpty() || !QFileInfo::exists(path)) {
        setStatus(studious::i18n::s("要导入的资产或文件不存在。",
                                    "The asset id or the file to import does not exist."),
                  true);
        return;
    }

    // 同一个资产别并行装两遍：装的过程会重写目标文件。
    if (m_assetProgress.contains(id)) {
        return;
    }
    m_assetProgress.insert(id, -1);
    emit assetProgressChanged();

    auto *watcher = new QFutureWatcher<studious::asset::DownloadResult>(this);
    connect(watcher,
            &QFutureWatcher<studious::asset::DownloadResult>::finished,
            this,
            [this, watcher, id] {
                const studious::asset::DownloadResult result = watcher->result();
                watcher->deleteLater();

                m_assetProgress.remove(id);
                emit assetProgressChanged();

                if (result.ok) {
                    setStatus(studious::i18n::s("导入完成。", "Import finished."), false);
                } else {
                    setStatus(studious::i18n::sf("导入失败：%1", "Import failed: %1", {result.error}),
                              true);
                    emit errorRaised(i18n::s().settings.assets, result.error);
                }

                refreshAssets();
                refreshMtPairs();
                refreshLocalState();
            });

    // 工作线程里造 HttpClient / Registry / Downloader（QNetworkAccessManager 有线程亲和）。
    // installFromFile 其实不发网络请求，但 Downloader 的构造签名要一个 IHttpClient。
    watcher->setFuture(QtConcurrent::run([id, path]() {
        studious::asset::DownloadResult result;

        studious::asset::Manifest manifest;
        if (!parseBundledManifest(&manifest)) {
            result.ok = false;
            result.error = QStringLiteral("找不到或无法解析资产清单");
            return result;
        }

        studious::net::QtHttpClient http;
        studious::asset::AssetRegistry registry(assetInstallRootPath(), manifest);
        studious::asset::Downloader downloader(http, registry);
        return downloader.installFromFile(id, path);
    }));
}

// -------------------------------------------------------------------- 翻译

bool AppBackend::useLocalEngine() const
{
    return m_engineId == QLatin1String("llm-local");
}

QString AppBackend::routerEngineId() const
{
    return toRouter(m_engineId);   // 空 = 自动，交给 priority 挑
}

void AppBackend::translate()
{
    if (m_job->isRunning()) {
        return;
    }

    const QStringList paragraphs = splitParagraphs(m_sourceText);
    if (paragraphs.isEmpty()) {
        setStatus(i18n::s().translate.emptyOriginal, true);
        return;
    }

    AppSettings settings = AppSettings::load();

    TranslateJobRequest req;
    req.paragraphs = paragraphs;
    req.srcLang = m_srcLang;
    req.dstLangs = m_dstLangs;
    req.engineId = routerEngineId();
    // 自动模式的降级顺序。这里必须是 Router id 空间且**不含 llm-local**
    // （它由 LocalEngine 分支单独承担，Router 不认识它）。换算只有 toRouterPriority 一处。
    req.priority = toRouterPriority(m_enginePriority);
    req.llm = settings.llm;
    req.cache = m_cache;
    req.cacheEnabled = m_cacheEnabled;
    // Web 来源：界面只让选一个（后端 AppSettings 也只存一个 id），
    // 但装配接口要的是"顺序表"，所以包成单元素列表；空串 = 走后端默认顺序。
    if (!settings.webProviderId.isEmpty()) {
        req.webProviders = {settings.webProviderId};
    }

    m_lastParagraphs = paragraphs;
    // 状态行要能看出"这次是谁翻的"：LLM 类引擎带上模型名，光写「本地 LLM」
    // 看不出用的是 0.5B 还是 1.5B。
    {
        QString label;
        if (m_engineId.isEmpty()) {
            label = i18n::s().translate.engineAuto;
        } else if (m_engineId == QLatin1String("web")) {
            label = i18n::s().translate.engineWeb;
        } else if (m_engineId == QLatin1String("llm-cloud")) {
            label = i18n::s().translate.engineLlmCloud;
            const QString model = shortModelName(settings.llm.model);
            if (!model.isEmpty()) {
                label = QStringLiteral("%1 · %2").arg(label, model);
            }
        } else if (m_engineId == QLatin1String("llm-local")) {
            label = i18n::s().translate.engineLlmLocal;
            const QString model = localModelLabel(settings);
            if (!model.isEmpty()) {
                label = QStringLiteral("%1 · %2").arg(label, model);
            }
        } else {
            label = i18n::s().translate.engineMt;
        }
        m_lastEngineLabel = label;
    }
    m_lastCacheEnabled = m_cacheEnabled;

    if (useLocalEngine()) {
        // 本地引擎：先把 llama-server 拉起来（阻塞约 1 秒），再提交翻译。
        LocalEngineConfig cfg;
        cfg.modelPath = localModelPathFrom(settings.localModelFileName);
        cfg.preferGpu = settings.localUseGpu;
        cfg.contextSize = settings.localContextSize;
        cfg.threads = settings.localThreads;
        cfg.gpuLayers = settings.localGpuLayers;
        cfg.extraArgs = settings.localExtraArgs;

        QString error;
        if (!studious::ui::sharedLocalEngine().ensureStarted(cfg, &error)) {
            // 失败时日志面板才是最该刷的地方：llama-server 提前退出、参数写错，
            // 唯一线索都在那条日志尾巴里。
            refreshLocalState();
            emit errorRaised(i18n::s().config.localLlmSection, error);
            setStatus(error, true);
            return;
        }
        // 服务是刚拉起来的：端口 / pid / 日志路径都变了，得让配置页那几个绑定重读。
        // 以前只在「启动」按钮里刷，所以"翻译能跑、日志面板空着"。
        refreshLocalState();
        req.llm.baseUrl = studious::ui::sharedLocalEngine().baseUrl();
        req.llm.apiKey = QStringLiteral("local");
        req.llm.model = studious::ui::sharedLocalEngine().currentModel();
        // 采样参数（temperature / maxTokens / 系统提示词）沿用配置页「云端 LLM」里那份：
        // 它们描述的是"翻译该怎么说话"，换引擎不该换口味，也没必要为此再摆一套 UI。
        // 只覆盖本地特有的三项：
        //   batchSize = 1       小模型做不好"一次返回 JSON 数组"，合并只会白花一次请求；
        //   reasoningEffort=""  本地模型没有思考等级这个概念，别往请求里塞无意义字段；
        //   stripModelNotes     本地 0.5B 爱加「【中文】」「（翻译结果：…）」这类注记，
        //                       该开关只删整行括注，正文一个字不动。
        req.llm.batchSize = 1;
        req.llm.reasoningEffort.clear();
        req.llm.stripModelNotes = true;
        req.engineId = QStringLiteral("llm");
        m_lastEngineLabel = i18n::s().translate.engineLlmLocal;
    }

    m_running = true;
    emit runningChanged();
    setWarning(QString());
    setStatus(i18n::s().translate.running, false);
    // 结果回来时按这一份快照拼装，别用届时的 m_dstLangs（用户可能已经改过）。
    m_inflightDstLangs = req.dstLangs;
    m_inflightSrcLang = req.srcLang;
    // 有请求在跑的时候绝不能被"空闲"回收，所以要重置一次计时；
    // 翻译结束还会再重置一次（见 onTranslationFinished）。
    touchLocalIdle();
    m_job->start(req);
}

void AppBackend::cancel()
{
    if (!m_running && !m_job->isRunning()) {
        return;
    }
    // 真的停：TranslateJob 会置位后端 translator 的 atomic 取消标志，段落循环
    // 在下一个请求边界退出；已经在飞的那一次 HTTP 请求没法硬打断，最多等它自己返回。
    // 迟到的结果按"代号"丢弃（见 TranslateJob::start），不会把界面改回去。
    m_job->cancel();
    m_running = false;
    emit runningChanged();
    setStatus(i18n::s().translate.cancelled, false);
}

void AppBackend::clearSource()
{
    setSourceText(QString());
    m_translatedText.clear();
    emit translatedTextChanged();
}

void AppBackend::copyTranslated()
{
    QGuiApplication::clipboard()->setText(m_translatedText);
}

void AppBackend::pasteFromClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty()) {
        setSourceText(text);
    }
}

void AppBackend::onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs)
{
    m_running = false;
    emit runningChanged();

    if (!result.outcome.ok) {
        m_translatedText.clear();
        emit translatedTextChanged();
        // 失败时把结构化结果一并清掉：否则译文栏会继续按旧块渲染，
        // 用户以为「新原文配新译文」，其实是上一单的残影。
        m_byLang.clear();
        m_lastPassthrough = false;
        emit blocksChanged();
        setStatus(result.outcome.error, true);
        refreshEngineStatuses();
        refreshLocalState();
        return;
    }

    const QStringList dstLangs = m_inflightDstLangs.isEmpty() ? m_dstLangs : m_inflightDstLangs;
    QStringList sections;
    sections.reserve(dstLangs.size());
    for (const QString &dst : dstLangs) {
        const QStringList paragraphs = result.outcome.byLang.value(dst);
        const int count = paragraphs.isEmpty() ? m_lastParagraphs.size() : paragraphs.size();
        QStringList blocks;
        blocks.reserve(count);
        for (int i = 0; i < count; ++i) {
            blocks << paragraphs.value(i);
        }
        const QString block = blocks.join(QStringLiteral("\n\n"));
        if (dstLangs.size() > 1) {
            sections << QStringLiteral("━━ %1 ━━\n%2")
                            .arg(studious::core::LanguageRegistry::displayName(dst), block);
        } else {
            sections << block;
        }
    }
    m_translatedText = sections.join(QStringLiteral("\n\n"));
    emit translatedTextChanged();

    // 保住结构化结果：译文栏的 tab 与逐段徽标都从这里重新切片。
    m_byLang = result.outcome.byLang;
    m_lastPassthrough = result.outcome.passthrough;
    if (!dstLangs.contains(m_activeDst)) {
        m_activeDst = dstLangs.value(0, QStringLiteral("zh"));
        emit activeDstChanged();
    }
    emit blocksChanged();

    setStatus(QStringLiteral("%1 · %2 ms").arg(m_lastEngineLabel).arg(elapsedMs), false);

    if (!result.outcome.warning.isEmpty()) {
        setWarning(result.outcome.warning);
    } else {
        setWarning(QString());
    }

    refreshCacheStats();
    refreshEngineStatuses();
    // 本地引擎在翻译过程中往日志里写了不少东西，跑完补刷一次，别让人对着旧尾巴猜。
    refreshLocalState();
    // 一轮翻译跑完，空闲计时从现在开始重新数。
    touchLocalIdle();
}

// -------------------------------------------------------------------- 状态

void AppBackend::refreshEngineStatuses()
{
    AppSettings settings = AppSettings::load();

    studious::net::QtHttpClient http;
    // 状态探测也要用同一套装配参数，否则会出现「状态说 web 就绪、真翻译却走另一个来源」。
    const QStringList webOrder = settings.webProviderId.isEmpty()
                                     ? QStringList{}
                                     : QStringList{settings.webProviderId};
    auto translators = studious::factory::makeDefaultTranslators(http, settings.llm, webOrder);
    // 用内存里的优先级（界面刚拖完还没落盘时也得一致），只做一次 id 换算。
    // llm-local 走 LocalEngine 分支、不参与 Router 降级链，toRouterPriority 会丢掉它。
    const QStringList priority = toRouterPriority(
        m_enginePriority.isEmpty() ? defaultPriority() : m_enginePriority);

    const studious::router::TranslatorRouter router(std::move(translators), priority);

    const QString dst = m_dstLangs.value(0, QStringLiteral("zh"));
    QVariantList list;

    // 本地 MT 的「装好没有」看的是语言对，而不是当前请求能不能路由：
    // 源语言是「自动检测」时 Router 必然拒绝（本地 MT 不支持自动检测后再选模型），
    // 但引擎本身是能用的，报「未就绪」会让人以为还得去下载东西。
    const studious::translate::LocalMtTranslator mt(LocalEngine::modelsDir());
    const QStringList mtPairs = mt.installedPairs();

    for (const auto &row : router.status(m_srcLang, dst)) {
        if (row.first == QLatin1String("mock")) {
            continue;
        }
        // Router 报的是 "llm"，界面上统一叫 "llm-cloud"。
        const QString uiId = (row.first == QLatin1String("llm"))
                                 ? QStringLiteral("llm-cloud")
                                 : row.first;

        QVariantMap item;
        item.insert(QStringLiteral("id"), uiId);
        item.insert(QStringLiteral("ready"), row.second.ready);
        item.insert(QStringLiteral("detail"), row.second.reason);
        // 三态给界面用：就绪 / 缺资产 / 未就绪。
        // ready=false 且缺资产时，用户能直接去资产页补，这种要让界面区分出来。
        QString state;
        if (row.second.ready) {
            state = QStringLiteral("ready");
        } else if (!row.second.missingAssets.isEmpty()) {
            state = QStringLiteral("missing-assets");
        } else {
            state = QStringLiteral("not-ready");
        }
        item.insert(QStringLiteral("state"), state);
        item.insert(QStringLiteral("missingAssets"), row.second.missingAssets.size());

        // 云端 LLM：配好了就报"实际会用哪个模型"；没配好要说清缺哪一项 ——
        // 后端 resolve() 只有一句"填写 API 地址 / Key / 模型"，用户看不出该补哪个。
        // 注意 isValid() 只查 baseUrl + model，空 Key 是合法的（本地 /v1 网关）。
        if (uiId == QLatin1String("llm-cloud")) {
            const QString model = shortModelName(settings.llm.model);
            if (row.second.ready) {
                item.insert(QStringLiteral("detail"),
                            model.isEmpty()
                                ? studious::i18n::s("没填模型名，去下面的引擎配置填一个",
                                                    "No model name set - pick one in the engine config below")
                                : studious::i18n::sf("%1 · 云端", "%1 · cloud", {model}));
            } else {
                QStringList missing;
                if (settings.llm.baseUrl.trimmed().isEmpty()) {
                    missing << QStringLiteral("Base URL");
                }
                if (settings.llm.model.trimmed().isEmpty()) {
                    missing << studious::i18n::s("模型名", "model");
                }
                if (!missing.isEmpty()) {
                    item.insert(QStringLiteral("detail"),
                                studious::i18n::sf("缺 %1 · 去下面的「引擎配置」填",
                                                   "Missing %1 - fill it in the engine config below",
                                                   {missing.join(studious::i18n::s("、", ", "))}));
                }
                // 都没缺时保留后端原话（比如地址格式不对、请求被拒之类）。
            }
        }

        // 本地 MT 单独改写三件事：
        //   state/available —— 按装了几个语言对算，跟这条请求能不能走无关；
        //   ready           —— 仍然是「这条请求能不能走」（源语言 auto 时为 false，
        //                      自动模式的优先级高亮要靠它，不能拿 state 顶替）；
        //   detail          —— 不再把 resolve() 的拒绝理由当故障文案。
        if (uiId == QLatin1String("local-mt")) {
            const bool hasPairs = !mtPairs.isEmpty();
            QString detail;
            if (!hasPairs) {
                detail = studious::i18n::s(
                    "没装任何语言对，去资产页下载或用「下载 / 导入」",
                    "No language pair installed. Download one on the Assets page.");
            } else if (row.second.ready) {
                // 副标题给用户看的是「英语 → 中文」，而不是资产 id / 路由细节。
                // 真正的诊断信息（哪条模型缺失、为什么拒绝）留在状态栏和日志里。
                const QString srcName =
                    studious::core::LanguageRegistry::displayName(m_srcLang);
                const QString dstName =
                    studious::core::LanguageRegistry::displayName(dst);
                detail = studious::i18n::sf("%1 → %2 · 本地语言包",
                                            "%1 → %2 · on-device pack",
                                            {srcName, dstName});
            } else {
                detail = studious::i18n::sf(
                    "已装 %1 个语言对 · 选定源语言后可用",
                    "%1 language pair(s) installed · pick a source language to use it",
                    {mtPairs.size()});
            }
            item.insert(QStringLiteral("available"), hasPairs);
            item.insert(QStringLiteral("state"),
                        hasPairs ? QStringLiteral("ready") : QStringLiteral("missing-assets"));
            item.insert(QStringLiteral("ready"), hasPairs && row.second.ready);
            item.insert(QStringLiteral("detail"), detail);
            item.insert(QStringLiteral("missingAssets"), hasPairs ? 0 : 1);
            list.append(item);
            continue;
        }

        item.insert(QStringLiteral("available"), row.second.ready);
        list.append(item);
    }

    // 本地 LLM 不在 Router 里，单独按 LocalEngine 的实况补一条。
    //
    // 三态定义（与产品约定一致，看的是「装好没有」，不是「服务现在跑没跑」）：
    //   ready          —— 运行时 + 模型都在，翻译时会自动把 llama-server 拉起来
    //   missing-assets —— 运行时或模型缺一个
    //   not-ready      —— 理论上不会出现（模型在、运行时就一定在），留着兜底
    //
    // 以前是「服务没在跑就报未就绪」，可服务本来就不该常驻：translate() 会
    // ensureStarted()，所以那种报法等于把「能用」说成「不能用」。
    const bool localRuntime = llamaRuntimeInstalled();
    // 设置里点名的模型文件可能已经被删了，而 models/ 里还留着别的模型 ——
    // 只按 availableModels() 判会报"就绪"，一点翻译就变成"缺少模型"，两处打脸。
    const QString configuredModelPath = localModelPathFrom(settings.localModelFileName);
    const bool configuredModelExists =
        !configuredModelPath.isEmpty() && QFileInfo(configuredModelPath).isFile();
    const bool configuredModelMissing = !configuredModelPath.isEmpty() && !configuredModelExists;
    // 绝对路径点到 models/ 外面的模型也算有模型（ensureStarted 认绝对路径）。
    const bool localHasModel = !LocalEngine::availableModels().isEmpty() || configuredModelExists;
    const bool localAvailable = localRuntime && localHasModel;
    const bool localRunning = studious::ui::sharedLocalEngine().isRunning();

    QString localDetail;
    if (localRunning) {
        // 跑起来之后以引擎实况为准（选「自动」时实际加载的可能不是设置里那个）。
        const QString runningModel = shortModelName(studious::ui::sharedLocalEngine().currentModel());
        const QString model = runningModel.isEmpty() ? localModelLabel(settings) : runningModel;
        const QString mode = studious::ui::sharedLocalEngine().currentBackend() == QLatin1String("vulkan")
                                 ? QStringLiteral("GPU")
                                 : QStringLiteral("CPU");
        localDetail = studious::i18n::sf("%1 · %2 · 端口 %3",
                                         "%1 · %2 · port %3",
                                         {model, mode, QString::number(studious::ui::sharedLocalEngine().port())});
    } else if (!localRuntime || !localHasModel) {
        // 先报"资产没装"：这时配置指向的模型当然也不在，说"配置的模型不在了"会误导人
        // 跑去引擎配置改选，而实际该去资产页下载。
        if (!localRuntime && !localHasModel) {
            localDetail = studious::i18n::s("缺 llama 运行时和模型文件",
                                            "Missing both the llama runtime and a model file");
        } else if (!localRuntime) {
            localDetail = studious::i18n::s(
                "缺 llama 运行时，去资产页下载",
                "No llama runtime installed. Download one on the Assets page.");
        } else {
            localDetail = studious::i18n::s(
                "缺模型文件，去资产页下载",
                "No model file installed. Download one on the Assets page.");
        }
    } else if (configuredModelMissing) {
        // 资产齐备但配置指错了：该去引擎配置改选，胶囊给黄（未就绪）而不是红。
        localDetail = studious::i18n::sf("配置的模型 %1 不在了 · 去「引擎配置」改选一个",
                                         "The configured model %1 is gone - pick another one in the engine config",
                                         {shortModelName(settings.localModelFileName)});
    } else {
        // 没跑的时候按"待会儿会用哪份配置"报：模型 + 会不会走 GPU。
        const QString model = localModelLabel(settings);
        const QString mode = (settings.localUseGpu && localGpuAvailable())
                                 ? QStringLiteral("GPU")
                                 : QStringLiteral("CPU");
        localDetail = studious::i18n::sf("%1 · %2 · 翻译时自动启动服务",
                                         "%1 · %2 · the server starts on demand",
                                         {model, mode});
    }

    QVariantMap local;
    // 「配置指向的模型没了」= 资产齐备但配置错，胶囊该是黄的（未就绪），不是绿的。
    const bool localUsable = localAvailable && !configuredModelMissing;
    local.insert(QStringLiteral("id"), QStringLiteral("llm-local"));
    local.insert(QStringLiteral("ready"), localRunning || localUsable);
    local.insert(QStringLiteral("available"), localUsable);
    local.insert(QStringLiteral("detail"), localDetail);
    local.insert(QStringLiteral("state"),
                 localUsable ? QStringLiteral("ready")
                             : (localAvailable ? QStringLiteral("not-ready")
                                               : QStringLiteral("missing-assets")));
    local.insert(QStringLiteral("missingAssets"),
                 (localRuntime ? 0 : 1) + (localHasModel ? 0 : 1));
    list.append(local);

    m_engineStatuses = list;
    emit engineStatusesChanged();
}

QStringList AppBackend::enginePriority() const
{
    return m_enginePriority;
}

void AppBackend::setEnginePriority(const QStringList &order)
{
    const QStringList next = sanitizePriority(order);
    if (next == m_enginePriority) {
        return;
    }

    m_enginePriority = next;

    AppSettings settings = AppSettings::load();
    settings.enginePriority = next;
    settings.save();

    emit enginePriorityChanged();
    // 顺序变了，「自动」的解析结果也变了，状态要重算。
    refreshEngineStatuses();
}

void AppBackend::refreshCacheStats()
{
    emit cacheStatsChanged();
}

void AppBackend::clearCache()
{
    if (m_cache != nullptr) {
        m_cache->clear();
    }
    refreshCacheStats();
}

void AppBackend::resetCacheStats()
{
    if (m_cache != nullptr) {
        m_cache->resetStats();
    }
    refreshCacheStats();
}

void AppBackend::startLocalServer()
{
    // 同一时刻只排一轮：这个函数会被配置页按钮、预热、以及三个翻译入口的
    // 「自动拉起」同时调用。以前每次都排一个 singleShot，串起来就是一串
    // shutdown() + 重新加载权重 + 各挑一个端口 —— 实测能把显存拖到整机卡死。
    const LocalEngine &engine = studious::ui::sharedLocalEngine();
    if (m_localStartQueued || engine.isStarting() || engine.isRunning()) {
        refreshLocalState();
        refreshEngineStatuses();
        return;
    }
    m_localStartQueued = true;
    // 「启动」按钮先给一句反馈再干活：真正的 ensureStarted() 是同步阻塞
    // （等 /v1 就绪，最长 120 秒，模型大的时候十几秒很正常），如果直接调，
    // 这一帧根本画不出来，用户看到的就是「点了没反应，然后整窗卡住」。
    // 用 singleShot(0) 把阻塞挪到下一轮事件循环：状态先上屏，按钮随即变灰。
    setStatus(studious::i18n::s("正在启动本地引擎……模型越大启动越久，期间界面会短暂无响应。",
                                "Starting the local engine… larger models take longer; "
                                "the window stays unresponsive until it's ready."),
              false);
    QTimer::singleShot(0, this, [this] { runLocalServerStart(); });
}

void AppBackend::runLocalServerStart()
{
    m_localStartQueued = false;
    // 没装运行时 / 没模型就别去拉进程：那种失败只会给出一条看不懂的日志尾巴。
    // 界面上「启动」按钮同样会禁用，这里再兜一层（CLI、快捷键、预热都走这个函数）。
    if (!llamaRuntimeInstalled() || LocalEngine::availableModels().isEmpty()) {
        setStatus(studious::i18n::s("本地 LLM 还缺资产：需要 runtime.llama.* 运行时和至少一个模型文件。"
                                    "先去「资产」页下载。",
                                    "Local LLM is missing assets: it needs a runtime.llama.* runtime "
                                    "and at least one model file. Download them on the Assets page first."),
                  true);
        return;
    }

    AppSettings settings = AppSettings::load();
    LocalEngineConfig cfg;
    cfg.modelPath = localModelPathFrom(settings.localModelFileName);
    cfg.preferGpu = settings.localUseGpu;
    cfg.contextSize = settings.localContextSize;
    cfg.threads = settings.localThreads;
    cfg.gpuLayers = settings.localGpuLayers;
    cfg.extraArgs = settings.localExtraArgs;

    QString error;
    if (!studious::ui::sharedLocalEngine().ensureStarted(cfg, &error)) {
        // 翻译页底部状态行就能看见原因；配置页靠「日志」面板看细节（refreshLocalState 会刷新）。
        // errorRaised 目前没有 QML 消费者，留着给以后的通知中心用，别指望它显示。
        setStatus(error, true);
        emit errorRaised(i18n::s().config.localLlmSection, error);
    } else {
        setStatus(i18n::s().common.ready, false);
        // 手动启动也算"活跃"：空闲回收要从现在开始数，不能从上次翻译算。
        touchLocalIdle();
    }
    refreshLocalState();
    refreshEngineStatuses();
}

void AppBackend::stopLocalServer()
{
    if (m_localIdleTimer) {
        m_localIdleTimer->stop();
    }
    studious::ui::sharedLocalEngine().shutdown();
    refreshLocalState();
    refreshEngineStatuses();
}

// -------------------------------------------------------------------- 私有

void AppBackend::setStatus(const QString &text, bool isError)
{
    // 全局通知条只订阅 errorRaised，所以每一条"用户必须看见"的失败都在这里统一冒泡。
    // 以前错误只写在翻译页底部那行状态里，用户在别的页面根本看不到（甚至被忽略）。
    if (isError && !text.isEmpty() && text != m_lastNotifiedError) {
        m_lastNotifiedError = text;
        emit errorRaised(studious::i18n::s("出错了", "Something went wrong"), text);
    } else if (!isError) {
        m_lastNotifiedError.clear();
    }

    if (m_statusText == text && m_statusIsError == isError) {
        return;
    }
    m_statusText = text;
    m_statusIsError = isError;
    emit statusTextChanged();
}

void AppBackend::setWarning(const QString &warning)
{
    if (m_statusWarning == warning) {
        return;
    }
    m_statusWarning = warning;
    emit statusWarningChanged();
}

void AppBackend::saveSettings()
{
    AppSettings settings = AppSettings::load();
    settings.srcLang = m_srcLang;
    settings.dstLang = langs::join(m_dstLangs);
    // 存界面 id（web / llm-cloud / llm-local / local-mt，空 = 自动），不再折成
    // Router 的 "llm" —— 那个折法会让 llm-local 丢成空串、重启后变回自动。
    // useLocalEngine 只是给老版本留的兼容字段，跟着一起写。
    settings.engineId = m_engineId;
    settings.useLocalEngine = useLocalEngine();
    settings.save();
}

void AppBackend::refreshAssets()
{
    m_assets.clear();

    const QString installRoot = assetInstallRootPath();
    m_assetInstallRoot = installRoot;

    studious::asset::Manifest manifest;
    if (!parseBundledManifest(&manifest)) {
        // 清单解析失败 = 资产页会空着；没有这行日志就只能看到"列表是空的"。
        qWarning("[assets] AppBackend::refreshAssets: bundled manifest parse failed (installRoot=%s)",
                 qUtf8Printable(installRoot));
        emit assetsChanged();
        return;
    }

    studious::asset::AssetRegistry registry(installRoot, manifest);
    registry.refresh();

    const QVector<studious::asset::AssetStatus> statuses = registry.listStatus();
    m_assets.reserve(statuses.size());

    for (const studious::asset::AssetStatus &status : statuses) {
        const studious::asset::AssetEntry *entry = manifest.find(status.id);
        if (entry == nullptr) {
            continue;
        }

        const bool placeholder = entry->isPlaceholder();
        const bool installed = status.state == studious::asset::AssetState::Installed;

        QVariantMap item;
        item.insert(QStringLiteral("id"), status.id);
        item.insert(QStringLiteral("category"), assetCategory(status.id, entry->family));
        item.insert(QStringLiteral("type"), entry->type);
        item.insert(QStringLiteral("installed"), installed);
        item.insert(QStringLiteral("placeholder"), placeholder);
        // 已装显示实际占盘，未装显示清单里写的下载体积。
        item.insert(QStringLiteral("sizeBytes"),
                    installed ? status.installedBytes : entry->size);
        item.insert(QStringLiteral("sizeText"),
                    studious::frontend::humanBytes(installed ? status.installedBytes : entry->size));
        item.insert(QStringLiteral("path"), status.installPath);
        // 只给状态码，文案由 QML 按界面语言出。
        // （不能在这儿用 i18n::s —— 这个命名空间里 i18n 指向前端文案表，
        //   而后端 i18n 被产品约定固定成英文，两边都不是界面要的中文。）
        switch (status.state) {
        case studious::asset::AssetState::Installed:
            item.insert(QStringLiteral("state"), QStringLiteral("installed"));
            break;
        case studious::asset::AssetState::Corrupted:
            item.insert(QStringLiteral("state"), QStringLiteral("corrupted"));
            break;
        case studious::asset::AssetState::NotInstalled:
        default:
            item.insert(QStringLiteral("state"), QStringLiteral("missing"));
            break;
        }
        item.insert(QStringLiteral("note"), entry->note);
        // 安装来源：download = 我们自己按清单下的，启动前会做内容校验；
        // import = 用户从文件装的，没有可信哈希可比，只保证文件还在。
        const studious::asset::AssetOrigin origin = registry.originOf(status.id);
        item.insert(QStringLiteral("origin"),
                    origin == studious::asset::AssetOrigin::Import ? QStringLiteral("import")
                                                                   : QStringLiteral("download"));
        item.insert(QStringLiteral("verified"),
                    origin != studious::asset::AssetOrigin::Import);
        m_assets.append(item);
    }

    emit assetsChanged();
}

void AppBackend::downloadAsset(const QString &id)
{
    if (id.isEmpty() || m_assetProgress.contains(id)) {
        return;   // 已经在下了
    }

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_assetCancel.insert(id, cancelFlag);

    QVariantMap started = m_assetProgress;
    started.insert(id, 0);
    m_assetProgress = started;
    emit assetProgressChanged();

    auto *watcher = new QFutureWatcher<studious::asset::DownloadResult>(this);
    connect(watcher,
            &QFutureWatcher<studious::asset::DownloadResult>::finished,
            this,
            [this, watcher, id] {
                const studious::asset::DownloadResult result = watcher->result();
                watcher->deleteLater();

                m_assetCancel.remove(id);
                QVariantMap done = m_assetProgress;
                done.remove(id);
                m_assetProgress = done;
                emit assetProgressChanged();

                if (!result.ok) {
                    emit errorRaised(i18n::s().settings.assets, result.error);
                }
                refreshAssets();
                // 下载/卸载的都是磁盘资产，本地 MT 的语言对表要跟着重扫。
                refreshMtPairs();
            });

    // 工作线程内构造 QtHttpClient / Registry / Downloader：
    // QNetworkAccessManager 是线程亲和的，绝不能跨线程共享。
    // 捕获 QPointer 而不是裸 this：下载可能跑几十分钟，这期间窗口可能已经关了，
    // 裸指针会在 emit 那一刻 UAF。
    const QString installRoot = assetInstallRootPath();
    watcher->setFuture(QtConcurrent::run([self = QPointer<AppBackend>(this), installRoot, id, cancelFlag]() {
        studious::asset::DownloadResult result;

        studious::asset::Manifest manifest;
        if (!parseBundledManifest(&manifest)) {
            result.ok = false;
            result.error = QStringLiteral("找不到或无法解析资产清单");
            return result;
        }

        studious::net::QtHttpClient http;
        studious::asset::AssetRegistry registry(installRoot, manifest);
        studious::asset::Downloader downloader(http, registry);

        const auto onProgress = [self, id](const studious::asset::DownloadProgress &pr) {
            if (!self) {
                return;   // 窗口已经关了，没人要这个进度了
            }
            const int percent = pr.total > 0
                                    ? static_cast<int>(pr.received * 100 / pr.total)
                                    : -1;
            emit self->assetProgressTick(id, percent);
        };

        return downloader.download(id, onProgress, cancelFlag.get());
    }));
}

void AppBackend::cancelAssetDownload(const QString &id)
{
    const auto it = m_assetCancel.find(id);
    if (it != m_assetCancel.end() && it.value()) {
        it.value()->store(true);
    }
}

void AppBackend::uninstallAsset(const QString &id)
{
    studious::asset::Manifest manifest;
    if (!parseBundledManifest(&manifest)) {
        emit errorRaised(i18n::s().settings.assets,
                         QStringLiteral("找不到或无法解析资产清单"));
        return;
    }

    studious::asset::AssetRegistry registry(assetInstallRootPath(), manifest);
    registry.refresh();

    QString error;
    if (!registry.uninstall(id, &error)) {
        emit errorRaised(i18n::s().settings.assets, error);
    }
    refreshAssets();
    refreshMtPairs();
}

bool AppBackend::llamaRuntimeInstalled() const
{
    // 直接查资产列表（refreshAssets 已经在构造和下载/卸载后刷过），
    // 别再扫一遍盘 —— refreshEngineStatuses 是要频繁调的。
    for (const QVariant &value : m_assets) {
        const QVariantMap asset = value.toMap();
        if (asset.value(QStringLiteral("id")).toString().startsWith(
                QLatin1String("runtime.llama."))
            && asset.value(QStringLiteral("installed")).toBool()) {
            return true;
        }
    }
    return false;
}

QString AppBackend::systemLanguage() const{
    return studious::ui::i18n::detectSystemLanguage() == studious::ui::i18n::Lang::Chinese
               ? QStringLiteral("zh")
               : QStringLiteral("en");
}

void AppBackend::setLlmConfig(const QVariantMap &config)
{
    const studious::config::LlmConfig next = llmConfigFromMap(config, studious::config::LlmConfig{});
    const QVariantMap nextMap = llmConfigToMap(next);
    if (nextMap == m_llmConfig) {
        return;
    }

    m_llmConfig = nextMap;

    AppSettings settings = AppSettings::load();
    settings.llm = next;
    settings.save();

    emit llmConfigChanged();
    // 改了连接参数，「云端 LLM 是否就绪」要重算。
    refreshEngineStatuses();
}

void AppBackend::setWebProviderId(const QString &id)
{
    if (m_webProviderId == id) {
        return;
    }
    m_webProviderId = id;

    AppSettings settings = AppSettings::load();
    settings.webProviderId = id;
    settings.save();

    emit webProviderIdChanged();
    refreshEngineStatuses();
}

void AppBackend::setLocalModel(const QString &fileName)
{
    if (m_localModel == fileName) {
        return;
    }
    m_localModel = fileName;

    AppSettings settings = AppSettings::load();
    settings.localModelFileName = fileName;
    settings.save();

    emit localModelChanged();
}

void AppBackend::setLocalUseGpu(bool enabled)
{
    if (m_localUseGpu == enabled) {
        return;
    }
    m_localUseGpu = enabled;

    AppSettings settings = AppSettings::load();
    settings.localUseGpu = enabled;
    settings.save();

    emit localUseGpuChanged();
}

// 本地 LLM 的四个启动参数：统一落盘 + 统一发 localLaunchChanged。
// 它们不立即重启 llama-server —— 服务只在真正要翻译 / 用户点「启动」时才拉起，
// 那时候 ensureStarted() 会拿新配置和正在跑的那份比，不一致才重启（见 LocalEngine）。

void AppBackend::setLocalContextSize(int tokens)
{
    if (m_localContextSize == tokens) {
        return;
    }
    m_localContextSize = tokens;

    AppSettings settings = AppSettings::load();
    settings.localContextSize = tokens;
    settings.save();

    emit localLaunchChanged();
}

void AppBackend::setLocalThreads(int threads)
{
    if (m_localThreads == threads) {
        return;
    }
    m_localThreads = threads;

    AppSettings settings = AppSettings::load();
    settings.localThreads = threads;
    settings.save();

    emit localLaunchChanged();
}

void AppBackend::setLocalGpuLayers(int layers)
{
    if (m_localGpuLayers == layers) {
        return;
    }
    m_localGpuLayers = layers;

    AppSettings settings = AppSettings::load();
    settings.localGpuLayers = layers;
    settings.save();

    emit localLaunchChanged();
}

void AppBackend::setLocalExtraArgs(const QString &args)
{
    const QString next = args.trimmed();
    if (m_localExtraArgs == next) {
        return;
    }
    m_localExtraArgs = next;

    AppSettings settings = AppSettings::load();
    settings.localExtraArgs = next;
    settings.save();

    emit localLaunchChanged();
}

void AppBackend::setLocalStopOnSwitch(bool on)
{
    if (m_localStopOnSwitch == on) {
        return;
    }
    m_localStopOnSwitch = on;

    AppSettings settings = AppSettings::load();
    settings.localStopOnSwitch = on;
    settings.save();

    // 打开「切换引擎时关闭」之后，空闲计时这段路就没意义了 —— 离开本地引擎会立刻停，
    // 停止计时器并把「空闲自动停」那组控件在人机界面上置灰（QML 侧 enabled 跟着这个值）。
    if (on && m_localIdleTimer) {
        m_localIdleTimer->stop();
    } else if (!on) {
        touchLocalIdle();
    }

    emit localLaunchChanged();
}

void AppBackend::setLocalPreheat(bool on)
{
    if (m_localPreheat == on) {
        return;
    }
    m_localPreheat = on;

    AppSettings settings = AppSettings::load();
    settings.localPreheat = on;
    settings.save();

    emit localLaunchChanged();
}

void AppBackend::setLocalIdleStop(bool on)
{
    if (m_localIdleStop == on) {
        return;
    }
    m_localIdleStop = on;

    AppSettings settings = AppSettings::load();
    settings.localIdleStop = on;
    settings.save();

    // 打开就立刻开始数（从"现在"算），关掉就把定时器停掉，别留着半路触发。
    if (on) {
        touchLocalIdle();
    } else if (m_localIdleTimer) {
        m_localIdleTimer->stop();
    }

    emit localLaunchChanged();
}

void AppBackend::setLocalIdleMinutes(int minutes)
{
    const int next = std::clamp(minutes, 1, 240);
    if (m_localIdleMinutes == next) {
        return;
    }
    m_localIdleMinutes = next;

    AppSettings settings = AppSettings::load();
    settings.localIdleMinutes = next;
    settings.save();

    // 改了阈值就按新值重新计时，否则用户会以为设置没生效。
    touchLocalIdle();
    emit localLaunchChanged();
}

void AppBackend::touchLocalIdle()
{
    if (!m_localIdleTimer || !m_localIdleStop || !studious::ui::sharedLocalEngine().isRunning()) {
        return;
    }
    m_localIdleTimer->start(m_localIdleMinutes * 60 * 1000);
}

void AppBackend::shutdownLocalEngine(const QString &reason)
{
    if (!studious::ui::sharedLocalEngine().isRunning()) {
        return;
    }
    if (m_localIdleTimer) {
        m_localIdleTimer->stop();
    }
    studious::ui::sharedLocalEngine().shutdown();
    refreshLocalState();
    refreshEngineStatuses();
    if (!reason.isEmpty()) {
        setStatus(reason, false);
    }
}

void AppBackend::refreshMtPairs()
{
    // 本地 MT 的"装了什么"完全由磁盘决定，所以这里现扫一遍，
    // 不复用资产清单 —— 资产页只管下载，用户也可能手动把目录拷进来。
    const studious::translate::LocalMtTranslator mt(LocalEngine::modelsDir());

    QVariantList list;
    const QStringList pairs = mt.installedPairs();
    list.reserve(pairs.size());
    for (const QString &pair : pairs) {
        const QStringList missing = mt.missingFiles(pair);
        QVariantMap item;
        item.insert(QStringLiteral("pair"), pair);
        item.insert(QStringLiteral("path"),
                    QDir(LocalEngine::modelsDir()).filePath(QStringLiteral("mt.opus.") + pair));
        item.insert(QStringLiteral("missing"), missing);
        // 目录在 ≠ 能用：缺了 config.json / vocab.json 一样加载不了。
        item.insert(QStringLiteral("usable"), missing.isEmpty());
        list.append(item);
    }

    m_mtPairs = list;
    emit mtPairsChanged();
}

bool AppBackend::mtPacksInstalled() const
{
    for (const QVariant &v : m_mtPairs) {
        if (v.toMap().value(QStringLiteral("usable")).toBool()) {
            return true;
        }
    }
    return false;
}

int AppBackend::mtLoadedPairs() const
{
    return studious::ui::mtLoadedPairs();
}

void AppBackend::setMtPreload(bool on)
{
    if (m_mtPreload == on) {
        return;
    }
    m_mtPreload = on;
    AppSettings s = AppSettings::load();
    s.mtPreload = on;
    s.save();
    emit mtStateChanged();

    // 打开就立刻加载（放到工作线程：ONNX 会话构建几百毫秒起，不能卡界面）。
    // 关掉就立刻释放，用户点一下就该看到内存回去。
    if (on) {
        if (!mtPacksInstalled()) {
            return;
        }
        (void)QtConcurrent::run([] {
            const int loaded = studious::ui::warmMtAll();
            qInfo("[mt] preload: %d pair(s) resident", loaded);
        });
        touchMtIdle();
    } else {
        if (m_mtIdleTimer) {
            m_mtIdleTimer->stop();
        }
        studious::ui::unloadMtAll();
        emit mtStateChanged();
    }
}

void AppBackend::setMtIdleMinutes(int minutes)
{
    const int next = std::clamp(minutes, 0, 180);
    if (m_mtIdleMinutes == next) {
        return;
    }
    m_mtIdleMinutes = next;
    AppSettings s = AppSettings::load();
    s.mtIdleMinutes = next;
    s.save();
    emit mtStateChanged();
    touchMtIdle();
}

void AppBackend::touchMtIdle()
{
    if (!m_mtIdleTimer) {
        return;
    }
    // 0 = 用户选了「不自动释放」；没有常驻语言对时也不必计时。
    if (m_mtIdleMinutes <= 0 || studious::ui::mtLoadedPairs() <= 0) {
        m_mtIdleTimer->stop();
        return;
    }
    m_mtIdleTimer->start(m_mtIdleMinutes * 60 * 1000);
}


QString AppBackend::engineName(const QString &engineId) const
{
    return studious::ui::engine::displayName(engineId);
}

QString AppBackend::mtRouteFor(const QString &src, const QString &dst) const
{
    const studious::translate::LocalMtTranslator mt(LocalEngine::modelsDir());
    // routeId() 为空 = 这条语言对根本没有可用路线（含"源语言是 auto"）。
    // 这种情况返回空串，让界面说人话；错误细节（vocab.json 里没有 auto 的 token 之类）
    // 不该直接怼到用户脸上。
    if (mt.routeId(src, dst).isEmpty()) {
        return QString();
    }
    // routeDescription 走 i18n，会跟着界面语言变（后端 i18n 已与界面语言同步）。
    return mt.routeDescription(src, dst);
}

void AppBackend::setUiLanguage(const QString &code)
{
    const QString next = (code == QLatin1String("zh") || code == QLatin1String("en"))
                             ? code
                             : QStringLiteral("system");
    if (m_uiLanguage == next) {
        return;
    }

    m_uiLanguage = next;

    // 落盘：空串交给 AppSettings 表示「跟随系统」。
    AppSettings settings = AppSettings::load();
    settings.uiLanguage = (next == QLatin1String("system")) ? QString() : next;
    settings.save();

    // 这里**故意不热切换**语言。界面文案在 QML 里是写死的，
    // 而且 i18n 必须在加载 QML 之前定下来，热切只会切一半。
    // 由界面上弹出的「重启」按钮完成。
    emit uiLanguageChanged();
}

void AppBackend::restartApplication()
{
    // 先松掉单实例的命名管道再拉起新进程：否则新进程 ping 到的还是这个正活着的
    // 旧实例，会被判成「重复启动」然后静默退出，看起来像重启失败。
    m_desktop.shutdown();
    // 用 detached 拉起自己再退出，这样父进程的退出不会带走新进程。
    // 不传参：避免把 --ui-shot 一类的调试开关带进新实例。
    const QString exe = QCoreApplication::applicationFilePath();
    if (QProcess::startDetached(exe, QStringList())) {
        QCoreApplication::quit();
    }
}

void AppBackend::copyText(const QString &text)
{
    if (text.isEmpty()) {
        qWarning("[clipboard] AppBackend::copyText: empty text ignored");
        return;
    }
    QGuiApplication::clipboard()->setText(text);
}

// ------------------------------------------------------------------ 日志窗口

QString AppBackend::logText() const
{
    return logsink::text();
}

QStringList AppBackend::logLines() const
{
    // 缓冲本来就是 4000 行封顶，这里按上限取，语义上跟 logText() 一致。
    return logsink::tail(4000);
}

QString AppBackend::logFilePath() const
{
    return logsink::filePath();
}

void AppBackend::clearLogs()
{
    logsink::clear();
    emit logTextChanged();
}

void AppBackend::copyLogs()
{
    const QStringList lines = logsink::tail(4000);
    if (lines.isEmpty()) {
        setStatus(studious::i18n::s("日志是空的，没有可复制的内容。",
                                    "The log is empty; nothing to copy."),
                  true);
        return;
    }
    QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    setStatus(studious::i18n::s("已复制 %1 行日志", "Copied %1 log lines").arg(lines.size()), false);
}

void AppBackend::openLogFile()
{
    const QString path = logsink::filePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        qWarning("[logs] openLogFile: log file missing: %s", qUtf8Printable(path));
        setStatus(studious::i18n::s("还没有日志文件：%1", "No log file yet: %1").arg(path), true);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        qWarning("[logs] openLogFile: openUrl failed: %s", qUtf8Printable(path));
        setStatus(studious::i18n::s("打不开日志文件（可能被占用，或被系统策略拦住）。",
                                    "Cannot open the log file (in use, or blocked by policy)."),
                  true);
    }
}

void AppBackend::openLogDir()
{
    // 文件可能还没建出来（一条日志都没有时），所以目录拿 filePath() 的父级再自己建。
    const QString dir = QFileInfo(logsink::filePath()).absolutePath();
    if (!QDir().mkpath(dir)) {
        qWarning("[logs] openLogDir: mkpath failed: %s", qUtf8Printable(dir));
        setStatus(studious::i18n::s("建不出日志目录：%1", "Could not create the log folder: %1").arg(dir),
                  true);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        qWarning("[logs] openLogDir: openUrl failed: %s", qUtf8Printable(dir));
        setStatus(studious::i18n::s("打不开这个目录（可能已被删除，或被系统策略拦住）。",
                                    "Cannot open the folder (missing, or blocked by policy)."),
                  true);
    }
}

void AppBackend::applyPalette(const QVariantMap &colors)
{
    if (colors.isEmpty()) {
        return;
    }

    const auto pick = [&colors](const char *key, const QColor &fallback) {
        const QVariant value = colors.value(QLatin1String(key));
        return value.canConvert<QColor>() && value.value<QColor>().isValid()
                   ? value.value<QColor>()
                   : fallback;
    };

    const QColor window = pick("window", QColor(Qt::white));
    const QColor windowText = pick("windowText", QColor(Qt::black));
    const QColor base = pick("base", window);
    const QColor text = pick("text", windowText);

    QPalette pal;
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, windowText);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, pick("alternateBase", base));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, pick("button", base));
    pal.setColor(QPalette::ButtonText, pick("buttonText", text));
    pal.setColor(QPalette::Highlight, pick("highlight", window));
    pal.setColor(QPalette::HighlightedText, pick("highlightedText", text));
    pal.setColor(QPalette::ToolTipBase, pick("toolTipBase", base));
    pal.setColor(QPalette::ToolTipText, pick("toolTipText", text));
    pal.setColor(QPalette::PlaceholderText, pick("placeholderText", windowText));

    // 禁用态跟着正文色走，避免系统默认的灰在深色主题下看不清。
    QColor disabled = text;
    disabled.setAlpha(110);
    pal.setColor(QPalette::Disabled, QPalette::Text, disabled);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    QGuiApplication::setPalette(pal);
}

// ============================================================================
// 后端能力的界面出口
// ============================================================================

QVariantList AppBackend::webProviders() const
{
    // 直接问后端要，前端不再自己维护一份列表 —— 后端加一个来源，界面自动多一项。
    QVariantList list;
    for (const auto &info : studious::translate::listWebProviders()) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), info.id);
        item.insert(QStringLiteral("displayName"), info.displayName);
        item.insert(QStringLiteral("requiresApiKey"), info.requiresApiKey);
        item.insert(QStringLiteral("configured"), info.configured);
        item.insert(QStringLiteral("note"), info.note);
        list.append(item);
    }
    return list;
}

QStringList AppBackend::localModels() const
{
    return LocalEngine::availableModels();
}

bool AppBackend::localGpuAvailable() const
{
    return LocalEngine::isGpuAvailable();
}

bool AppBackend::localCpuAvailable() const
{
    return LocalEngine::isCpuAvailable();
}

int AppBackend::localPort() const
{
    return studious::ui::sharedLocalEngine().port();
}

qint64 AppBackend::localPid() const
{
    return studious::ui::sharedLocalEngine().processId();
}

QString AppBackend::localBackend() const
{
    return studious::ui::sharedLocalEngine().currentBackend();
}

qint64 AppBackend::localStartupMs() const
{
    return studious::ui::sharedLocalEngine().lastStartupMs();
}

QString AppBackend::localModelPath() const
{
    return studious::ui::sharedLocalEngine().modelPath();
}

QString AppBackend::localLogTail() const
{
    return studious::ui::sharedLocalEngine().logTail(200);
}

QString AppBackend::localLogPath() const
{
    return studious::ui::sharedLocalEngine().logFilePath();
}

void AppBackend::refreshLocalState()
{
    // 端口 / pid / 日志都可能被 llama-server 那一侧改掉，界面定时拉一次就够。
    emit localStateChanged();
}

QString AppBackend::detectLanguage(const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    const auto detected = studious::i18n::detectLanguage(text);
    // 置信度太低就当没测出来，免得给用户一个错的结论。
    if (detected.confidence < 0.3) {
        return QString();
    }
    return detected.code;
}

void AppBackend::fetchCloudModels()
{
    if (m_cloudBusy) {
        return;
    }

    const studious::config::LlmConfig cfg =
        llmConfigFromMap(m_llmConfig, studious::config::LlmConfig{});

    m_cloudBusy = true;
    emit cloudBusyChanged();
    m_cloudProbeResult.clear();
    emit cloudProbeResultChanged();

    auto *watcher = new QFutureWatcher<studious::llm::ModelListResult>(this);
    connect(watcher, &QFutureWatcher<studious::llm::ModelListResult>::finished, this,
            [this, watcher]() {
                const auto result = watcher->result();
                watcher->deleteLater();

                m_cloudBusy = false;
                emit cloudBusyChanged();

                if (!result.ok) {
                    m_cloudModels.clear();
                    emit cloudModelsChanged();
                    emit errorRaised(QStringLiteral("Model list fetch failed"), result.error);
                    return;
                }

                QVariantList list;
                for (const auto &model : result.models) {
                    QVariantMap item;
                    item.insert(QStringLiteral("id"), model.id);
                    item.insert(QStringLiteral("name"),
                                model.name.isEmpty() ? model.id : model.name);
                    item.insert(QStringLiteral("ownedBy"), model.ownedBy);
                    item.insert(QStringLiteral("supportsReasoning"), model.supportsReasoning);
                    item.insert(QStringLiteral("supportsTools"), model.supportsTools);
                    item.insert(QStringLiteral("maxInputTokens"), model.maxInputTokens);
                    item.insert(QStringLiteral("maxOutputTokens"), model.maxOutputTokens);
                    list.append(item);
                }
                m_cloudModels = list;
                emit cloudModelsChanged();
            });

    watcher->setFuture(QtConcurrent::run([cfg]() {
        // QNetworkAccessManager 有线程亲和，必须在工作线程内构造客户端。
        studious::net::QtHttpClient http;
        return studious::llm::listModels(cfg, http);
    }));
}

void AppBackend::probeCloudReasoning()
{
    if (m_cloudBusy) {
        return;
    }

    const studious::config::LlmConfig cfg =
        llmConfigFromMap(m_llmConfig, studious::config::LlmConfig{});

    m_cloudBusy = true;
    emit cloudBusyChanged();
    m_cloudProbeResult.clear();
    emit cloudProbeResultChanged();

    auto *watcher = new QFutureWatcher<studious::llm::ReasoningProbeResult>(this);
    connect(watcher, &QFutureWatcher<studious::llm::ReasoningProbeResult>::finished, this,
            [this, watcher]() {
                const auto result = watcher->result();
                watcher->deleteLater();

                m_cloudBusy = false;
                emit cloudBusyChanged();

                if (!result.ok) {
                    m_cloudProbeResult = QStringLiteral("Probe failed: %1").arg(result.error);
                    emit cloudProbeResultChanged();
                    return;
                }

                using studious::llm::ReasoningProbe;
                QString verdict;
                switch (result.probe) {
                case ReasoningProbe::Disabled:
                    verdict = QStringLiteral("reasoning can be disabled");
                    break;
                case ReasoningProbe::StillThinking:
                    verdict = QStringLiteral("still thinking, parameter ignored");
                    break;
                case ReasoningProbe::Unknown:
                default:
                    verdict = QStringLiteral("inconclusive");
                    break;
                }
                m_cloudProbeResult =
                    QStringLiteral("%1 · reasoning_tokens=%2%3")
                        .arg(verdict)
                        .arg(result.reasoningTokens)
                        .arg(result.hasReasoningContent ? QStringLiteral(" · has reasoning_content")
                                                        : QString());
                emit cloudProbeResultChanged();
            });

    watcher->setFuture(QtConcurrent::run([cfg]() {
        studious::net::QtHttpClient http;
        return studious::llm::probeReasoning(cfg, http);
    }));
}

TranslatorRecords *AppBackend::recordsFor(const QString &kind)
{
    // 只认三个已知来源，其它（老数据里的空串之类）都归到截屏那一档。
    const QString key = (kind == QLatin1String("mini") || kind == QLatin1String("live"))
                            ? kind
                            : QStringLiteral("capture");

    std::unique_ptr<TranslatorRecords> &slot = key == QLatin1String("mini") ? m_recordsMini
                                              : key == QLatin1String("live") ? m_recordsLive
                                                                            : m_recordsCapture;

    if (!slot) {
        // 三个库放在 <dataRoot>/records/ 下：clearAll / 备份 / 删库都能按文件来。
        const QString dir = studious::ui::paths::recordsDir();
        QDir().mkpath(dir);
        slot = std::make_unique<TranslatorRecords>(
            QDir(dir).filePath(key + QStringLiteral(".db")));
    }
    return slot.get();
}

bool AppBackend::persistRecord(const QString &kind,
                               const QString &imagePath,
                               const QRect &region,
                               const QVariantList &originals,
                               const QVariantList &blocks,
                               const QString &engineLabel)
{
    // 历史关着就什么都不写：这个函数的所有调用点都是"自动存档"，
    // 手动存档入口已经拿掉了，所以开关就是唯一的总闸。
    const AppSettings settings = AppSettings::load();
    if (!settings.historyEnabled)
        return false;

    TranslationRecord rec;
    rec.source = kind.isEmpty() ? QStringLiteral("capture") : kind;
    // 截屏这条链的语言要取"发起这一单时的"快照：翻译跑完用户可能已经
    // 改了语言，用全局 settings 会把记录标成另一种语言对。
    const bool captureFlow = rec.source == QStringLiteral("capture");
    rec.srcLang = captureFlow && !m_inflightSrcLang.isEmpty()
                      ? langs::normalizeSource(m_inflightSrcLang)
                      : langs::normalizeSource(settings.srcLang);
    rec.dstLangs = (captureFlow && !m_inflightDstLangs.isEmpty())
                       ? m_inflightDstLangs.join(QLatin1Char(','))
                       : settings.dstLang;
    rec.engine = engineLabel.isEmpty() ? m_lastEngineLabel : engineLabel;
    rec.imagePath = imagePath;
    rec.region = region;

    QStringList originalLines;
    originalLines.reserve(originals.size());
    for (const QVariant &value : originals)
        originalLines.append(value.toString());
    rec.originalText = originalLines.join(QLatin1Char('\n'));

    // 译文按块拼回去：没翻出来的块保留原文，这样记录里不会出现空洞。
    QStringList translated;
    translated.reserve(blocks.size());
    for (const QVariant &value : blocks) {
        const QVariantMap block = value.toMap();
        const QString text = block.value(QStringLiteral("text")).toString();
        translated.append(text.isEmpty() ? block.value(QStringLiteral("original")).toString() : text);
    }
    rec.translatedText = translated.join(QLatin1Char('\n'));
    rec.createdAt = QDateTime::currentDateTime();

    TranslatorRecords *store = recordsFor(rec.source);
    const qint64 id = store->add(rec);
    if (id <= 0) {
        setStatus(store->lastError(), true);
        return false;
    }
    setStatus(studious::i18n::s("已保存到记录。", "Saved to records."), false);
    return true;
}

bool AppBackend::saveCaptureRecord(const QString &source,
                                   const QString &imagePath,
                                   const QRect &region,
                                   const QVariantList &originals,
                                   const QVariantList &blocks)
{
    return persistRecord(source, imagePath, region, originals, blocks);
}

bool AppBackend::saveMiniRecord(const QVariantList &originals,
                                const QVariantList &blocks,
                                const QString &engineLabel)
{
    return persistRecord(QStringLiteral("mini"), QString(), QRect(), originals, blocks, engineLabel);
}

bool AppBackend::saveLiveRecord(const QVariantList &originals,
                                const QVariantList &blocks,
                                const QString &engineLabel)
{
    // 实时刷新会把同一屏反复送上来：同一组里原文与上一行相同的直接丢掉，
    // 只留第一次看到它的那一刻。以前这段去重在 begin/append/finish 三件套里，
    // 那两个入口没人调用，等于没做。
    QVariantList keptOriginals;
    QVariantList keptBlocks;
    keptOriginals.reserve(originals.size());
    keptBlocks.reserve(blocks.size());
    QString previous;
    for (int i = 0; i < originals.size(); ++i) {
        const QString original = originals.at(i).toString().trimmed();
        if (!keptOriginals.isEmpty() && original == previous) {
            continue;
        }
        previous = original;
        keptOriginals.append(originals.at(i));
        keptBlocks.append(i < blocks.size() ? blocks.at(i) : QVariant());
    }
    return persistRecord(QStringLiteral("live"), QString(), QRect(), keptOriginals, keptBlocks,
                         engineLabel);
}

QVariantList AppBackend::historyRecords(int limit)
{
    if (limit <= 0)
        limit = 100;

    QVariantList merged;
    const QStringList kinds = {QStringLiteral("capture"), QStringLiteral("mini"), QStringLiteral("live")};
    for (const QString &kind : kinds) {
        TranslatorRecords *store = recordsFor(kind);
        // 每个库都按自己的上限取，最后再整体截断 —— 不能只取前 limit 条，
        // 否则某个库最近写入很多时，另外两个库的近期记录会被挤出去。
        const QVector<TranslationRecord> rows = store->list(limit);
        for (const TranslationRecord &row : rows) {
            QVariantMap item;
            item.insert(QStringLiteral("kind"), kind);
            item.insert(QStringLiteral("id"), row.id);
            item.insert(QStringLiteral("source"), row.source);
            item.insert(QStringLiteral("srcLang"), row.srcLang);
            item.insert(QStringLiteral("dstLangs"), row.dstLangs);
            item.insert(QStringLiteral("engine"), row.engine);
            item.insert(QStringLiteral("imagePath"), row.imagePath);
            item.insert(QStringLiteral("regionX"), row.region.x());
            item.insert(QStringLiteral("regionY"), row.region.y());
            item.insert(QStringLiteral("regionW"), row.region.width());
            item.insert(QStringLiteral("regionH"), row.region.height());
            item.insert(QStringLiteral("originalText"), row.originalText);
            item.insert(QStringLiteral("translatedText"), row.translatedText);
            item.insert(QStringLiteral("createdAt"), row.createdAt.toString(Qt::ISODate));
            merged.append(item);
        }
    }

    std::sort(merged.begin(), merged.end(), [](const QVariant &lhs, const QVariant &rhs) {
        return lhs.toMap().value(QStringLiteral("createdAt")).toString()
             > rhs.toMap().value(QStringLiteral("createdAt")).toString();
    });

    if (merged.size() > limit)
        merged = merged.mid(0, limit);
    return merged;
}

int AppBackend::recordCount()
{
    int total = 0;
    for (const QString &kind : {QStringLiteral("capture"), QStringLiteral("mini"), QStringLiteral("live")})
        total += recordsFor(kind)->count();
    return total;
}

} // namespace studious::ui
