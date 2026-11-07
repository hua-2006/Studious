#include "TranslateJob.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QRegularExpression>
#include <QTimer>

#include <studious/Factory.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace {

// 工作线程入口：所有线程亲和的 Qt 对象都在这函数里生灭。
//
// 为什么不能把 http / translators / router 提到外面共享：
//   QNetworkAccessManager 只能在创建它的线程使用（Qt 文档明确要求），
//   跨线程共享会踩 "Cannot create children for a parent in a different thread"
//   以及请求随机不回调。所以每次翻译都现造一套，代价只有几微秒。
//
// 缓存是唯一的例外：TranslationCache 由后端保证可跨线程共享
// （每个线程用自己的 SQLite 连接），所以这里只借用指针，不负责它的生命周期。
TranslateJobResult translateInWorkerThread(const TranslateJobRequest &job,
                                          const std::shared_ptr<TranslateCancelState> &cancel)
{
    studious::net::QtHttpClient http;   // 工作线程内构造

    // 装配参数（LLM 配置 / Web 来源顺序）在这里落地：
    //   空 LlmConfig -> llm 组件未就绪，Router 自动跳过；
    //   空 webProviders -> web 组件走默认 google -> mymemory。
    // mock 只能是显式选择（--engine mock / 界面选 mock），所以装配时按 engineId 决定
    // 要不要把它注册进来 —— 平时它连注册表都不在，自然不可能被"兜底"选中。
    const bool wantsMock = job.engineId == QLatin1String("mock");
    auto translators = studious::factory::makeDefaultTranslators(
        http, job.llm, job.webProviders, /*includeMock=*/wantsMock);

    // 优先级为空时用兜底顺序：与后端/界面一致的本地优先（local-mt -> llm -> web）。
    // 这里刻意不含 mock —— mock 输出的是【语言码】原文的假译文，只能由用户显式
    // --engine mock / 界面里选 mock 时才用；作为兜底会让"什么都没配好"静默变成假成功。
    // 正常路径上 job.priority 来自 AppBackend 的 settings.enginePriority，走不到这个分支。
    const QStringList priority =
        job.priority.isEmpty()
            ? QStringList{QStringLiteral("local-mt"), QStringLiteral("llm"),
                          QStringLiteral("web")}
            : job.priority;

    studious::router::TranslatorRouter router(std::move(translators), priority);

    // 缓存注入：指针可以为空（= 完全不碰 SQLite），开关另算。
    // 两个都要给，Router 才真正启用：isCacheEnabled() == (cache != nullptr && enabled)。
    router.setTranslationCache(job.cache);
    router.setCacheEnabled(job.cacheEnabled);

    // 取消通道：把 router 的地址挂出去，让 UI 线程能在请求飞行期间置位取消标志。
    // 挂之前先看一眼用户是不是已经按了取消（哪怕只是几百微秒之前），
    // 这样"点取消 -> 立刻又点翻译"的极端时序也不会白跑一整单。
    if (cancel) {
        std::lock_guard<std::mutex> lock(cancel->mu);
        cancel->router = &router;
        if (cancel->requested.load()) {
            router.cancel();
        }
    }

    // 后端契约只认段落 + 语言对 + 引擎偏好，装配参数不进 TranslateRequest。
    studious::core::TranslateRequest req;
    req.paragraphs = job.paragraphs;
    req.srcLang = job.srcLang;
    req.dstLangs = job.dstLangs;
    req.preferredEngine = job.engineId;

    TranslateJobResult result;
    result.outcome = router.translate(req);

    // 先把指针摘下再取统计：此后 cancel() 不会再摸这个 router。
    if (cancel) {
        std::lock_guard<std::mutex> lock(cancel->mu);
        cancel->router = nullptr;
    }

    // 命中统计只反映"上一次 translate()"，必须在同一线程紧跟着取（别挪到主线程读）。
    const studious::router::TranslatorRouter::CacheReport report = router.lastCacheReport();
    result.cacheUsed = report.used;
    result.cacheHits = report.hits;
    result.cacheMisses = report.misses;
    return result;
}

} // namespace

QStringList splitParagraphs(const QString &text)
{
    // 规则：空行（可含空白字符）是段落分隔；连续空行算一个；首尾空白去掉；丢弃空段。
    static const QRegularExpression lineBreak(QStringLiteral("\r\n|\n|\r"));

    QStringList paragraphs;
    QStringList current;

    const auto flush = [&paragraphs, &current]() {
        if (current.isEmpty()) {
            return;
        }
        const QString para = current.join(QLatin1Char('\n')).trimmed();
        current.clear();
        if (!para.isEmpty()) {
            paragraphs << para;
        }
    };

    const QStringList lines = text.split(lineBreak);
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            flush();
        } else {
            current << line;
        }
    }
    flush();

    return paragraphs;
}

void TranslateCancelState::raise()
{
    requested.store(true);
    // 这里的锁只等"worker 正在挂/摘指针"的那一瞬间，不等整单翻译。
    std::lock_guard<std::mutex> lock(mu);
    if (router) {
        router->cancel();
    }
}

TranslateJob::TranslateJob(QObject *parent)
    : QObject(parent)
    , m_cancelTimer(new QTimer(this))
{
    // 各 translator 在每次 translate 入口都会把自己的取消标志清掉，
    // 所以"取消"必须反复置位，直到结果真的回来（见 reapplyCancel）。
    m_cancelTimer->setInterval(150);
    connect(m_cancelTimer, &QTimer::timeout, this, &TranslateJob::reapplyCancel);
}

TranslateJob::~TranslateJob()
{
    // 关窗时若还有请求在飞：等它结束（HttpClient 自带超时，不会无限挂）。
    // 这样可避免工作线程在 QCoreApplication 销毁后仍在摸 Qt 网络栈。
    for (QFutureWatcher<TranslateJobResult> *watcher : std::as_const(m_pending)) {
        if (watcher->isRunning()) {
            watcher->waitForFinished();
        }
    }
}

bool TranslateJob::isRunning() const
{
    return m_running;
}

void TranslateJob::start(const TranslateJobRequest &req)
{
    if (m_running) {
        return;
    }

    m_running = true;
    m_cancelTimer->stop();
    const auto cancel = std::make_shared<TranslateCancelState>();
    m_cancel = cancel;
    const int generation = ++m_generation;
    m_timer.start();

    auto *watcher = new QFutureWatcher<TranslateJobResult>(this);
    m_pending.append(watcher);
    connect(watcher, &QFutureWatcher<TranslateJobResult>::finished, this, [this, watcher, generation]() {
        const TranslateJobResult result = watcher->result();
        const qint64 elapsedMs = m_timer.isValid() ? m_timer.elapsed() : 0;

        m_pending.removeAll(watcher);
        watcher->deleteLater();

        // 代号对不上 = 这一单已经被 cancel() + 新 start() 取代了：结果直接丢，
        // 不能让它把界面上刚恢复的状态再改回去。
        if (generation != m_generation) {
            if (!m_running) {
                m_cancelTimer->stop();
            }
            return;
        }

        m_running = false;
        m_cancelTimer->stop();
        m_cancel.reset();
        emit finished(result, elapsedMs);
    });

    // 按值拷贝进线程：req 是纯数据（缓存指针只是借用，不转移所有权），跨线程不能带引用。
    watcher->setFuture(QtConcurrent::run(&translateInWorkerThread, req, cancel));
}

void TranslateJob::cancel()
{
    if (!m_cancel) {
        return;
    }
    m_cancel->raise();
    // 界面立刻解锁：用户不需要等这一单真正收尾就能改选引擎、重按翻译。
    m_running = false;
    // 代号一变，这一单的结果回来时就会被丢掉 —— 取消之后即使它刚好算完，
    // 也不能再往界面上贴译文（否则"取消"会被结果覆盖成"成功"）。
    ++m_generation;
    if (!m_cancelTimer->isActive()) {
        m_cancelTimer->start();
    }
}

void TranslateJob::reapplyCancel()
{
    if (!m_cancel || !m_cancel->requested.load()) {
        m_cancelTimer->stop();
        return;
    }
    m_cancel->raise();
}
