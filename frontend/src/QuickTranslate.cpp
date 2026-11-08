#include "QuickTranslate.h"

#include "AppSettings.h"
#include "LocalEngine.h"
#include "EngineIds.h"
#include "Languages.h"
#include "TranslateJob.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <functional>
#include <memory>

#include <studious/cache/TranslationCache.h>
#include <studious/i18n/L10n.h>
#include <studious/translators/LocalMtTranslator.h>

namespace studious::ui {
namespace {

// 后端 i18n 的别名（studious::ui::i18n 已经被 Strings.h 占了，这里换个名字免得撞）。
namespace l10n = studious::i18n;

// 引擎 id 换算统一在 EngineIds（界面 id ↔ Router id 只有一份实现）。
using studious::ui::engine::sanitizePriority;
using studious::ui::engine::toRouter;
using studious::ui::engine::toRouterPriority;
using studious::ui::engine::toUi;

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
    const QStringList order = sanitizePriority(settings.enginePriority);
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
} // namespace

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

// ─── 进程级共享的本地 MT ────────────────────────────────────────────────────

namespace {
std::function<void()> &mtUsedHook()
{
    static std::function<void()> hook;
    return hook;
}

std::function<void()> &localEngineStarter()
{
    static std::function<void()> starter;
    return starter;
}

// 启动冷却：一次拉起要 2~10 秒，这期间再来多少次「它没在跑」都只能等。
// 光看 isStarting() 不够 —— 上一轮刚失败（比如端口被占）时标志已经落下，
// 调用方会立刻再拉一次，变成一个「失败-重拉」的紧密循环。
bool starterCooldownOver()
{
    static QElapsedTimer clock;   // 默认构造是 invalid，第一次必定放行
    static bool first = true;
    if (first || !clock.isValid() || clock.elapsed() > 5000) {
        first = false;
        clock.restart();
        return true;
    }
    return false;
}
} // namespace

studious::translate::LocalMtTranslator &sharedMt()
{
    // 资产根可能被用户改过（设置里可迁移），路径变了就换一个新实例，
    // 旧实例随之析构，内存也一起还回去。
    static QString root;
    static std::unique_ptr<studious::translate::LocalMtTranslator> instance;
    const QString wanted = LocalEngine::modelsDir();
    if (!instance || root != wanted) {
        instance = std::make_unique<studious::translate::LocalMtTranslator>(wanted);
        root = wanted;
    }
    return *instance;
}

int warmMtAll()
{
    int loaded = 0;
    const QStringList pairs = sharedMt().installedPairs();
    for (const QString &pair : pairs) {
        if (sharedMt().warmPair(pair)) {
            ++loaded;
        }
    }
    qInfo("[mt] warmed %d/%lld language pair(s)", loaded, static_cast<long long>(pairs.size()));
    return loaded;
}

void unloadMtAll()
{
    const int before = sharedMt().loadedCount();
    sharedMt().unloadAll();
    qInfo("[mt] unloaded %d language pair(s)", before);
}

int mtLoadedPairs()
{
    return sharedMt().loadedCount();
}

void setMtUsedHook(std::function<void()> hook)
{
    mtUsedHook() = std::move(hook);
}

void setLocalEngineStarter(std::function<void()> starter)
{
    localEngineStarter() = std::move(starter);
}

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

void QuickTranslate::setEngineOverride(const QString &uiEngineId)
{
    // 存空串也算"覆盖"，语义是「自动」：实时翻译里选「自动/跟随设置」是两件事，
    // 调用方用 clearEngineOverride() 表达后者。
    m_hasEngineOverride = true;
    m_engineOverride = toUi(uiEngineId);
}

void QuickTranslate::clearEngineOverride()
{
    m_hasEngineOverride = false;
    m_engineOverride.clear();
}

QString QuickTranslate::lastError() const
{
    return m_lastError;
}

bool QuickTranslate::lastFailureRetryable() const
{
    return m_retryable;
}

bool QuickTranslate::translate(const QString &text, const QString &srcLang, const QStringList &dstLangs)
{
    m_lastError.clear();
    m_retryable = false;

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

    const QString src = langs::normalizeSource(srcLang);
    // 优先用调用方指定的引擎（实时翻译工具栏的下拉），否则回到全局设置。
    const QString uiEngine = m_hasEngineOverride ? toUi(m_engineOverride)
                                                 : toUi(settings.engineId);   // 空 = 自动
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
    req.engineId = toRouter(uiEngine);
    req.priority = toRouterPriority(sanitizePriority(settings.enginePriority));

    if (settings.cacheEnabled) {
        req.cache = processCache();
        req.cacheEnabled = true;
    } else {
        req.cache = nullptr;
        req.cacheEnabled = false;
    }

    // 本地 MT 一律用进程级共享实例（界面上的「开机自启 / 空闲关闭」就是它的生命周期）。
    // Router 只借指针，所有权仍在这里 —— TranslateJob 里用不删除的 unique_ptr 包一层。
    req.sharedMt = &sharedMt();

    QString label;
    if (wantLocal) {
        // 本地 LLM 必须**已经起来**才走这条分支。
        //
        // 以前这里直接 ensureStarted()：那是同步阻塞（等 /v1 就绪，最长 120 秒），
        // 而且是在 GUI 线程上跑的 —— 实时翻译时切到本地 LLM，界面会整段卡死；
        // 同时 llama-server 正在把 1.5B 模型按 -ngl 99 往显存里塞，机器跟着一起僵。
        // 现在改成「没起就报错」，启动只留在配置页那个显式按钮里做。
        LocalEngine &local = sharedLocalEngine();
        if (!local.isRunning()) {
            // 用户在配置页之外选了本地 LLM（截图 / 实时 / 小窗）时，这里顺手把它拉起来：
            // 启动本身是异步的（不阻塞界面），调用方看到 lastFailureRetryable() 就隔一会儿重试。
            //
            // 但「正在启动」期间绝不能再触发一次：每次启动都要重新加载权重，实时翻译
            // 每 600 ms 一帧全打过来就是一个启动风暴（实测一秒开了 6 个 llama-server，
            // 显存被反复占满，整机卡死）。所以这里既看 isStarting()，又加 5 秒冷却。
            if (auto &starter = localEngineStarter(); starter && !local.isStarting() && starterCooldownOver()) {
                starter();
                m_retryable = true;
                m_lastError = l10n::s("本地 LLM 正在启动，稍后自动重试…",
                                      "Local LLM is starting; retrying in a moment…");
                emit errorOccurred(m_lastError);
                return false;
            }
            if (local.isStarting()) {
                m_retryable = true;
                m_lastError = l10n::s("本地 LLM 正在启动，稍后自动重试…",
                                      "Local LLM is starting; retrying in a moment…");
                emit errorOccurred(m_lastError);
                return false;
            }
            m_lastError = l10n::s(
                "本地 LLM 还没启动：先在「配置」页的「本地 LLM」里点「启动」，"
                "或者换成云端 LLM / 本地 MT。",
                "Local LLM is not running yet: start it on the Configuration page "
                "(Local LLM card), or switch to Cloud LLM / Local MT.");
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
    // 本地 MT 刚干过活：通知外部把「空闲释放」计时器往后推。
    if (auto &hook = mtUsedHook(); hook) {
        hook();
    }
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
