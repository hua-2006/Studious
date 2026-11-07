#include "QuickTranslate.h"

#include "AppSettings.h"
#include "LocalEngine.h"
#include "TranslateJob.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <studious/cache/TranslationCache.h>
#include <studious/i18n/L10n.h>
#include <studious/translators/LocalMtTranslator.h>

namespace studious::ui {
namespace {

// 后端 i18n 的别名（studious::ui::i18n 已经被 Strings.h 占了，这里换个名字免得撞）。
namespace l10n = studious::i18n;

// ─── 引擎 id 换算 ────────────────────────────────────────────────────────────
// 与 AppBackend.cpp 匿名 namespace 里那四个函数（toUiEngineId / toRouterEngineId /
// sanitizedPriority / toRouterPriority）**逐条等价**。它们是 .cpp 内部符号、没导出，
// 为了保证「界面设置 → 后端 id」只有一套语义，这里照抄并保持同一份注释意图：
//   llm-cloud → router "llm"；llm-local → 空（它不经 Router，由 LocalEngine 单独承担）。

QStringList knownUiEngineIds()
{
    return {QStringLiteral("web"), QStringLiteral("llm-cloud"),
            QStringLiteral("llm-local"), QStringLiteral("local-mt")};
}

QStringList defaultUiEnginePriority()
{
    return {QStringLiteral("llm-local"), QStringLiteral("local-mt"),
            QStringLiteral("llm-cloud"), QStringLiteral("web")};
}

// 任意写法 → 规范界面 id。认不出来 / "auto" / 空 → 空串，代表「自动」。
QString toUiEngineId(const QString &raw)
{
    const QString id = raw.trimmed().toLower();
    if (id == QLatin1String("web")) {
        return QStringLiteral("web");
    }
    if (id == QLatin1String("llm-cloud") || id == QLatin1String("llm")) {
        // router 的老 id "llm" 在设置里靠 useLocalEngine 分流；走到这里按云端算（同 AppBackend）。
        return QStringLiteral("llm-cloud");
    }
    if (id == QLatin1String("llm-local") || id == QLatin1String("llamacpp")
        || id == QLatin1String("local-llm")) {
        return QStringLiteral("llm-local");
    }
    if (id == QLatin1String("local-mt") || id == QLatin1String("localmt")) {
        return QStringLiteral("local-mt");
    }
    return QString();
}

// 界面 id → Router 引擎 id。llm-local 返回空：它不进优先级链。
QString toRouterEngineId(const QString &uiId)
{
    if (uiId == QLatin1String("llm-cloud")) {
        return QStringLiteral("llm");
    }
    if (uiId == QLatin1String("llm-local")) {
        return QString();
    }
    return uiId;
}

// 只含已知 id、无重复、不缺项（缺的按默认顺序补在后面）。
QStringList sanitizedPriority(const QStringList &input)
{
    const QStringList known = knownUiEngineIds();
    QStringList out;
    for (const QString &raw : input) {
        const QString uiId = toUiEngineId(raw);
        if (uiId.isEmpty() || out.contains(uiId)) {
            continue;
        }
        if (known.contains(uiId)) {
            out.append(uiId);
        }
    }
    for (const QString &id : defaultUiEnginePriority()) {
        if (!out.contains(id)) {
            out.append(id);
        }
    }
    return out;
}

// 界面优先级 → Router 优先级（自动模式下 req.priority 就用这个，Router 才会降级）。
QStringList toRouterPriority(const QStringList &uiOrder)
{
    QStringList out;
    for (const QString &uiId : uiOrder) {
        const QString id = toRouterEngineId(uiId);
        if (id.isEmpty() || out.contains(id)) {
            continue;
        }
        out.append(id);
    }
    return out;
}

// ─── 段落切分 ────────────────────────────────────────────────────────────────

// 空行切段（连续空行算一个分隔；段内单换行保留），逐段 trimmed 后丢掉空段。
QStringList parseParagraphs(const QString &text)
{
    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    static const QRegularExpression blankLine(QStringLiteral("\\n\\s*\\n+"));
    const QStringList pieces = normalized.split(blankLine);

    QStringList paragraphs;
    paragraphs.reserve(pieces.size());
    for (const QString &piece : pieces) {
        const QString trimmed = piece.trimmed();
        if (!trimmed.isEmpty()) {
            paragraphs.append(trimmed);
        }
    }
    return paragraphs;
}

// 后端约定「某语言段落数与请求一致」，但不一致时不能整单失败：补空串到等长。
QStringList padToSize(const QStringList &in, qsizetype size)
{
    QStringList out = in;
    if (out.size() > size) {
        out = out.mid(0, size);
    }
    while (out.size() < size) {
        out.append(QString());
    }
    return out;
}

// ─── 本地引擎 / 模型名（照抄 AppBackend 的 helper） ──────────────────────────

QString localModelPathFrom(const QString &fileName)
{
    const QString trimmed = fileName.trimmed();
    if (trimmed.isEmpty()) {
        return QString();   // 空必须原样传空：LocalEngine 的「空 = 第一个可用模型」分支只在真·空串时走
    }
    const QFileInfo info(trimmed);
    return info.isAbsolute() ? info.absoluteFilePath()
                             : QDir(LocalEngine::modelsDir()).filePath(trimmed);
}

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

QString localModelLabel(const AppSettings &settings)
{
    QString name = settings.localModelFileName.trimmed();
    if (name.isEmpty()) {
        name = LocalEngine::availableModels().value(0);
    }
    return shortModelName(name);
}

// ─── 「自动」时的引擎探测 ────────────────────────────────────────────────────
// TranslateOutcome 不带引擎 id（后端只回译文），所以自动模式的 engineLabel 只能自己探。
// 判据与 AppBackend::refreshEngineStatuses 一致：看「装好没有」，不看「服务在跑没有」。
// 注意这里只影响**显示文案**，真正的降级由 Router 按 req.priority 自己做。

bool localLlmUsable(const AppSettings &settings)
{
    const bool runtime = LocalEngine::isCpuAvailable() || LocalEngine::isGpuAvailable();
    if (!runtime) {
        return false;
    }
    const QString configured = localModelPathFrom(settings.localModelFileName);
    if (!configured.isEmpty()) {
        return QFileInfo(configured).isFile();
    }
    return !LocalEngine::availableModels().isEmpty();
}

bool localMtUsable(const QString &srcLang, const QString &dstLang)
{
    // ctor 只存路径，installedPairs()/supports() 是几次 stat，探测足够便宜。
    // 注意命名空间是 studious::translate（不是 include 路径里的 translators）。
    const studious::translate::LocalMtTranslator mt(LocalEngine::modelsDir());
    return mt.supports(srcLang, dstLang);
}

bool engineLooksUsable(const QString &uiId, const AppSettings &settings,
                       const QString &srcLang, const QString &dstLang)
{
    if (uiId == QLatin1String("llm-local")) {
        return localLlmUsable(settings);
    }
    if (uiId == QLatin1String("local-mt")) {
        return localMtUsable(srcLang, dstLang);
    }
    if (uiId == QLatin1String("llm-cloud")) {
        return settings.llm.isValid();
    }
    if (uiId == QLatin1String("web")) {
        return true;   // 网络可用性在这儿判不了，交给真正翻译时再说
    }
    return false;
}

// ─── 引擎名文案（与 AppBackend 的状态行同一套词） ───────────────────────────

QString engineBaseName(const QString &uiId)
{
    if (uiId == QLatin1String("web")) {
        return l10n::s("Web 在线", "Web online");
    }
    if (uiId == QLatin1String("llm-cloud")) {
        return l10n::s("云端 LLM", "Cloud LLM");
    }
    if (uiId == QLatin1String("llm-local")) {
        return l10n::s("本地 LLM", "Local LLM");
    }
    if (uiId == QLatin1String("local-mt")) {
        return l10n::s("本地 MT", "Local MT");
    }
    return l10n::s("自动", "Auto");
}

QString describeEngine(const QString &uiId, const AppSettings &settings)
{
    QString label = engineBaseName(uiId);
    if (uiId == QLatin1String("web")) {
        const QString provider = settings.webProviderId.trimmed();
        // 空 = 后端默认顺序（google → mymemory），界面上就说 google。
        label += QStringLiteral(" · ") + (provider.isEmpty() ? QStringLiteral("google") : provider);
    } else if (uiId == QLatin1String("llm-cloud")) {
        const QString model = shortModelName(settings.llm.model);
        if (!model.isEmpty()) {
            label += QStringLiteral(" · ") + model;
        }
    } else if (uiId == QLatin1String("llm-local")) {
        const QString model = localModelLabel(settings);
        if (!model.isEmpty()) {
            label += QStringLiteral(" · ") + model;
        }
    }
    return label;
}

// 自动模式：按界面优先级找出第一个「看起来能用」的引擎，探不出来就老实写「自动」。
QString describeAutoEngine(const AppSettings &settings, const QString &srcLang, const QString &dstLang)
{
    const QStringList order = sanitizedPriority(settings.enginePriority);
    for (const QString &uiId : order) {
        if (engineLooksUsable(uiId, settings, srcLang, dstLang)) {
            return describeEngine(uiId, settings);
        }
    }
    return l10n::s("自动", "Auto");
}

// ─── 缓存 ────────────────────────────────────────────────────────────────────

// 与 main.cpp 的 sharedCache() 指向同一个 db 文件。TranslationCache 的连接名是
// 「路径哈希 + 线程 id」，所以同线程里两个实例共用同一条 QSqlDatabase 连接，不会各开一条。
// 故意不 delete（同 main.cpp）：析构会 close/removeDatabase，把别人还在用的连接拔掉。
studious::cache::TranslationCache *processCache()
{
    static studious::cache::TranslationCache *instance = nullptr;
    if (instance == nullptr) {
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                 .filePath(QStringLiteral("cache.db"));
        instance = new studious::cache::TranslationCache(path);
    }
    return instance;
}

// ─── 本地引擎 ────────────────────────────────────────────────────────────────

// 整个进程一份：三个页面（以及各自持有的 QuickTranslate）共用同一个 llama-server。
// 为什么不每人一份：每个 LocalEngine 实例都会拉起一个 llama-server，三页各来一个
// 就是三份模型内存。引擎配置是全局设置，三页传下来的 LocalEngineConfig 必然相同，
// 所以 ensureStarted() 是幂等命中，不会互相热重启。
// 故意不 delete（与 main.cpp 的 sharedCache 同一套理由）：退出时靠 aboutToQuit 关服务，
// 对象本身留着，避免 QProcess 在静态析构阶段踩到已经拆掉的 Qt 基础设施。
LocalEngine &sharedLocalEngine()
{
    static LocalEngine *engine = nullptr;
    if (engine == nullptr) {
        engine = new LocalEngine;
        if (QCoreApplication *app = QCoreApplication::instance()) {
            // LocalEngine 不是 QObject，所以 context 用 app（它活到进程退出）。
            QObject::connect(app, &QCoreApplication::aboutToQuit, app, [engine] {
                engine->shutdown();   // 退出绝不留下 llama-server 孤儿进程
            });
        }
    }
    return *engine;
}

} // namespace

QuickTranslate::QuickTranslate(QObject *parent)
    : QObject(parent)
{
    // 信号参数是 struct，跨线程排队（或将来被 QML/元对象看到）需要注册一次。
    static const int registered = qRegisterMetaType<QuickTranslateResult>(
        "studious::ui::QuickTranslateResult");
    Q_UNUSED(registered)

    m_job = new TranslateJob(this);
    connect(m_job, &TranslateJob::finished, this, &QuickTranslate::onJobFinished);
}

QuickTranslate::~QuickTranslate() = default;

bool QuickTranslate::isBusy() const
{
    return m_busy;
}

QString QuickTranslate::lastError() const
{
    return m_lastError;
}

bool QuickTranslate::translate(const QString &text, const QString &srcLang, const QStringList &dstLangs)
{
    m_lastError.clear();

    // 并发调用直接拒绝，不排队：三页各自持有一个 QuickTranslate，排队只会让
    // 「刚截的图」等「上一张的译文」，用户看到的是延迟而不是新结果。
    if (m_busy) {
        m_lastError = l10n::s("上一次翻译还没结束", "The previous translation is still running");
        emit errorOccurred(m_lastError);
        return false;
    }

    const QStringList paragraphs = parseParagraphs(text);
    if (paragraphs.isEmpty()) {
        m_lastError = l10n::s("没有可翻译的文本", "Nothing to translate");
        emit errorOccurred(m_lastError);
        return false;
    }

    QStringList dst;
    for (const QString &raw : dstLangs) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty() && !dst.contains(trimmed)) {
            dst.append(trimmed);
        }
    }
    if (dst.isEmpty()) {
        m_lastError = l10n::s("没有选择目标语言", "No target language selected");
        emit errorOccurred(m_lastError);
        return false;
    }

    // 每次都重读设置：用户刚改完引擎/模型/代理就点翻译，必须立刻生效。
    const AppSettings settings = AppSettings::load();

    const QString src = srcLang.trimmed().isEmpty() ? QStringLiteral("auto") : srcLang.trimmed();
    const QString uiEngine = toUiEngineId(settings.engineId);   // 空 = 自动
    const bool wantLocal = uiEngine == QLatin1String("llm-local");

    TranslateJobRequest req;
    req.paragraphs = paragraphs;
    req.srcLang = src;
    req.dstLangs = dst;
    req.llm = settings.llm;

    if (!settings.webProviderId.trimmed().isEmpty()) {
        req.webProviders = QStringList{settings.webProviderId.trimmed()};
    }

    // 自动模式不写 req.engineId（写了 Router 就不降级了），只给优先级；
    // 显式选了云端/本地 MT 才写 routerEngineId（"llm" / "local-mt" / "web"）。
    req.engineId = toRouterEngineId(uiEngine);
    req.priority = toRouterPriority(sanitizedPriority(settings.enginePriority));

    if (settings.cacheEnabled) {
        req.cache = processCache();
        req.cacheEnabled = true;
    } else {
        req.cache = nullptr;
        req.cacheEnabled = false;
    }

    QString label;
    if (wantLocal) {
        // 本地 LLM 走 LocalEngine 懒启动（主线程阻塞到 /v1 就绪），与 AppBackend 同一套参数。
        LocalEngineConfig cfg;
        cfg.modelPath = localModelPathFrom(settings.localModelFileName);
        cfg.preferGpu = settings.localUseGpu;
        cfg.contextSize = settings.localContextSize;
        cfg.threads = settings.localThreads;
        cfg.gpuLayers = settings.localGpuLayers;
        cfg.extraArgs = settings.localExtraArgs;

        LocalEngine &local = sharedLocalEngine();
        QString error;
        if (!local.ensureStarted(cfg, &error)) {
            m_lastError = error.isEmpty()
                              ? l10n::s("本地引擎启动失败", "Failed to start the local engine")
                              : error;
            emit errorOccurred(m_lastError);
            return false;
        }

        req.llm.baseUrl = local.baseUrl();
        req.llm.apiKey = QStringLiteral("local");
        const QString runningModel = local.currentModel();
        req.llm.model = runningModel.isEmpty() ? QStringLiteral("local") : runningModel;
        req.llm.batchSize = 1;              // llama-server 一次一段更稳
        req.llm.reasoningEffort.clear();    // 本地模型不认这个字段
        req.llm.stripModelNotes = true;
        // 服务已经在指定端口跑起来了，绝不能再让 Router 去挑别的引擎。
        req.engineId = QStringLiteral("llm");

        label = describeEngine(QStringLiteral("llm-local"), settings);
    } else if (uiEngine.isEmpty()) {
        label = describeAutoEngine(settings, src, dst.first());
    } else {
        label = describeEngine(uiEngine, settings);
    }

    // 快照：结果回来时按这里补齐段落 / 回填文案（期间设置可能已经被改）。
    m_pendingParagraphs = paragraphs;
    m_pendingDstLangs = dst;
    m_pendingEngineLabel = label;

    m_cancelled = false;
    m_busy = true;

    emit started();
    emit progress(0, static_cast<int>(paragraphs.size()));
    m_job->start(req);   // 工作线程由 TranslateJob 自己起
    return true;
}

void QuickTranslate::cancel()
{
    if (!m_busy) {
        return;
    }

    // 两层取消：
    //   1) TranslateJob::cancel() 把后端各 translator 的 atomic 标志置位，工作线程在
    //      下一次请求边界退出（正在飞的那一次 HTTP 请求仍然会跑完，没法硬打断）；
    //   2) 这里立刻发一个「已取消」结果并解锁 isBusy()，界面不用等收尾。
    // 真结果回来时代号已经变了，TranslateJob 会直接丢掉，不会再发 finished。
    m_job->cancel();
    m_cancelled = true;
    m_busy = false;

    QuickTranslateResult out;
    out.ok = false;
    out.error = l10n::s("已取消", "Cancelled");
    out.engineLabel = m_pendingEngineLabel;
    out.paragraphs = m_pendingParagraphs;
    for (const QString &dst : m_pendingDstLangs) {
        out.byLang.insert(dst, padToSize(QStringList(), m_pendingParagraphs.size()));
    }

    m_lastError = out.error;
    emit finished(out);
}

void QuickTranslate::onJobFinished(const TranslateJobResult &result, qint64 elapsedMs)
{
    const bool cancelled = m_cancelled;
    m_cancelled = false;
    m_busy = false;   // cancel() 也会复位；这里兜住"正常跑完"的那条路

    if (cancelled) {
        return;   // cancel() 已经给过结果了
    }

    QuickTranslateResult out;
    out.paragraphs = m_pendingParagraphs;
    out.engineLabel = m_pendingEngineLabel;
    out.cacheHits = result.cacheHits;
    out.cacheMisses = result.cacheMisses;
    out.elapsedMs = elapsedMs;

    if (!result.outcome.ok) {
        // 后端的 error 本来就是中文人话，原样透传，别再包一层「翻译失败：」。
        out.ok = false;
        out.error = result.outcome.error;
        m_lastError = out.error;
        emit progress(static_cast<int>(m_pendingParagraphs.size()),
                      static_cast<int>(m_pendingParagraphs.size()));
        emit finished(out);
        return;
    }

    out.ok = true;
    QStringList problems;
    for (const QString &dst : m_pendingDstLangs) {
        const QStringList got = result.outcome.byLang.value(dst);
        if (got.size() != m_pendingParagraphs.size()) {
            // 段落数对不上：补空串对齐（1:1 是调用方的硬约定，缺段不能让它整体失败）。
            problems.append(l10n::sf("%1 返回 %2 段，与原文 %3 段不一致，已按空串补齐",
                                     "%1 returned %2 paragraph(s) for %3 source paragraph(s); "
                                     "padded with empty strings",
                                     {dst, QString::number(got.size()),
                                      QString::number(m_pendingParagraphs.size())}));
            out.byLang.insert(dst, padToSize(got, m_pendingParagraphs.size()));
        } else {
            out.byLang.insert(dst, got);
        }
    }
    if (!problems.isEmpty()) {
        out.error = problems.join(l10n::s("；", "; "));   // 不整体失败，只把问题挂出来
        m_lastError = out.error;
    }

    emit progress(static_cast<int>(m_pendingParagraphs.size()),
                  static_cast<int>(m_pendingParagraphs.size()));
    emit finished(out);
}

} // namespace studious::ui
