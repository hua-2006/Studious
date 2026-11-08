#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>
#include <mutex>

#include <studious/cache/TranslationCache.h>
#include <studious/config/LlmConfig.h>
#include <studious/contract/Types.h>

class QTimer;

namespace studious::router {
class TranslatorRouter;
}

namespace studious::core {
class ITranslator;
}

// 取消通道：工作线程把 router 指针挂进来，主线程拿着 atomic 标志和同一把锁去置位。
// 锁的意义不是保护 router 的方法，而是保证"读指针 + 调用 cancel()"期间
// 工作线程不会把 router 析构掉（worker 在离开作用域前会先拿锁把指针清空）。
struct TranslateCancelState {
    std::mutex mu;
    studious::router::TranslatorRouter *router = nullptr;
    std::atomic_bool requested{false};

    // 主线程 / 定时器调用：把取消请求转给当前在跑的那个 router。
    void raise();
};

// 把"输入文本 -> 段落列表"的切分规则集中在一处，GUI 与 --smoke 共用同一份实现。
QStringList splitParagraphs(const QString &text);

// 一次翻译作业的完整入参：GUI 侧从 AppSettings + 控件取值后整包丢进工作线程。
//
// 为什么不用后端的 core::TranslateRequest：
//   LlmConfig / webProviders 是"装配参数"，不属于单次请求的语义，
//   混进 TranslateRequest 会污染后端契约（后端只认段落 + 语言对 + 引擎偏好）。
struct TranslateJobRequest {
    QStringList paragraphs;
    QString srcLang = QStringLiteral("auto");
    // 目标语言是**多选**：后端的 TranslateOutcome::byLang 是 dstLang -> 段落译文 的映射，
    // 一次请求可以同时要多个目标语言。为空时后端会回落成 zh。
    QStringList dstLangs = {QStringLiteral("zh")};
    QString engineId;           // 空 = 自动
    // 引擎优先级，原样传给 TranslatorRouter 的 priority 参数。
    // 空 = 用后端默认顺序（mock -> web -> llm -> local-mt）。
    QStringList priority;
    QStringList webProviders;   // 空 = 后端默认顺序 google -> mymemory
    studious::config::LlmConfig llm;   // 空配置 => llm 组件 isReady()==false，Router 自动跳过

    // 翻译缓存实例（**不拥有**，由进程级那个共享对象存活）。
    //
    // 允许为空：为空时 Router 内部 isCacheEnabled()==false，直接走原来的老路径。
    // 指针跨线程传是安全的——TranslationCache 内部按线程各开一条 SQLite 连接，
    // 后端头文件明确承诺可跨线程共享。
    studious::cache::TranslationCache *cache = nullptr;
    bool cacheEnabled = true;   // 界面上「启用缓存」勾选框的状态

    // 进程级共享的本地 MT 实例（**不拥有**）。非空时用它顶替工厂里那个临时
    // LocalMtTranslator —— 模型因此能在多次翻译之间常驻，界面上的「开机自启 /
    // 空闲关闭」也才有意义。指针来自 studious::ui::sharedMt()，寿命覆盖本作业。
    studious::core::ITranslator *sharedMt = nullptr;
};

// 一次翻译作业的产出：后端 outcome + Router 报上来的缓存命中情况。
//
// 为什么单独包一层：core::TranslateOutcome 是后端契约，前端不该往里面塞字段；
// 而命中统计只有前端用得上（状态栏「缓存命中 2/3」与 --smoke 输出）。
struct TranslateJobResult {
    studious::core::TranslateOutcome outcome;
    bool cacheUsed = false;   // Router 这次是否真的走了缓存路径（关缓存时为 false）
    int cacheHits = 0;
    int cacheMisses = 0;
};

// 一次翻译作业：把 router.translate() 丢到工作线程跑，结果通过信号回主线程。
//
// 线程约束（关键）：
//   QtHttpClient / QNetworkAccessManager 是线程亲和的，所以 http 实例、
//   factory 装配出来的 translator 集合、TranslatorRouter 全部在**工作线程内部**
//   构造并在同一线程内使用、销毁，绝不跨线程共享。
//   （TranslationCache 是唯一例外：后端保证它线程安全，见上面 req.cache 的注释。
//     取消用的 CancelState 是第二个例外：里面只有一把互斥量和原子的请求标志。）
//   本类自身一直活在主线程（QFutureWatcher 也在主线程），只跨线程搬运纯数据
//   TranslateJobRequest（入）/ TranslateJobResult（出）。
//
// 取消语义（重要）：
//   cancel() 之后 isRunning() 立刻变 false、界面可以马上再点一次翻译，但工作线程
//   还要过一小会儿才真的退出（背层的同步 HTTP 没法从外部打断，只能在两次请求之间
//   的检查点松手）。为了让"迟到的结果"不覆盖新状态，每次 start() 都递增代号，
//   结果回来时代号对不上就直接丢弃。
class TranslateJob : public QObject {
    Q_OBJECT

public:
    explicit TranslateJob(QObject *parent = nullptr);
    ~TranslateJob() override;

    // 逻辑运行状态：cancel() 之后马上为 false（此时工作线程可能还在收尾）。
    bool isRunning() const;

public slots:
    // 逻辑上已在跑时会被忽略（GUI 侧同时也会禁用按钮，双保险）。
    void start(const TranslateJobRequest &req);

    // 主动取消：置位后端各 translator 的 atomic 取消标志，并让界面立刻解锁。
    // 可以在 start() 之后任意时刻调用；没在跑时是空操作。
    void cancel();

signals:
    // elapsedMs 由 QElapsedTimer 在 start() 起算，回到主线程时取。
    void finished(const TranslateJobResult &result, qint64 elapsedMs);

private:
    void reapplyCancel();

    // 每次 start() 一个 watcher：被取消的旧作业还在收尾时，新作业可以立刻开始。
    QVector<QFutureWatcher<TranslateJobResult> *> m_pending;
    std::shared_ptr<TranslateCancelState> m_cancel;
    QElapsedTimer m_timer;
    QTimer *m_cancelTimer = nullptr;
    int m_generation = 0;
    bool m_running = false;
};
