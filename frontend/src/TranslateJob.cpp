#include "TranslateJob.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QRegularExpression>

#include <studious/Factory.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>

#include <utility>

namespace {

// 工作线程入口：所有线程亲和的 Qt 对象都在这函数里生灭。
//
// 为什么不能把 http / translators / router 提到外面共享：
//   QNetworkAccessManager 只能在创建它的线程使用（Qt 文档明确要求），
//   跨线程共享会踩 "Cannot create children for a parent in a different thread"
//   以及请求随机不回调。所以每次翻译都现造一套，代价只有几微秒。
studious::core::TranslateOutcome translateInWorkerThread(const TranslateJobRequest &job)
{
    studious::net::QtHttpClient http;   // 工作线程内构造

    // 装配参数（LLM 配置 / Web 来源顺序）在这里落地：
    //   空 LlmConfig -> llm 组件未就绪，Router 自动跳过；
    //   空 webProviders -> web 组件走默认 google -> mymemory。
    auto translators =
        studious::factory::makeDefaultTranslators(http, job.llm, job.webProviders);
    studious::router::TranslatorRouter router(std::move(translators));

    // 后端契约只认段落 + 语言对 + 引擎偏好，装配参数不进 TranslateRequest。
    studious::core::TranslateRequest req;
    req.paragraphs = job.paragraphs;
    req.srcLang = job.srcLang;
    req.dstLangs = QStringList{job.dstLang};
    req.preferredEngine = job.engineId;
    return router.translate(req);
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

TranslateJob::TranslateJob(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFutureWatcher<studious::core::TranslateOutcome>(this))
{
    connect(m_watcher,
            &QFutureWatcher<studious::core::TranslateOutcome>::finished,
            this,
            &TranslateJob::onFutureFinished);
}

TranslateJob::~TranslateJob()
{
    // 关窗时若还有请求在飞：等它结束（HttpClient 自带超时，不会无限挂）。
    // 这样可避免工作线程在 QCoreApplication 销毁后仍在摸 Qt 网络栈。
    if (m_watcher->isRunning()) {
        m_watcher->waitForFinished();
    }
}

bool TranslateJob::isRunning() const
{
    return m_watcher->isRunning();
}

void TranslateJob::start(const TranslateJobRequest &req)
{
    if (m_watcher->isRunning()) {
        return;
    }

    m_timer.start();
    // 按值拷贝进线程：req 是纯数据，跨线程不能带引用。
    m_watcher->setFuture(QtConcurrent::run(&translateInWorkerThread, req));
}

void TranslateJob::onFutureFinished()
{
    const qint64 elapsedMs = m_timer.isValid() ? m_timer.elapsed() : 0;
    emit finished(m_watcher->result(), elapsedMs);
}
