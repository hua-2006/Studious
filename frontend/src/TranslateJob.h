#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>

#include <studious/cache/TranslationCache.h>
#include <studious/config/LlmConfig.h>
#include <studious/contract/Types.h>

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
//   （TranslationCache 是唯一例外：后端保证它线程安全，见上面 req.cache 的注释。）
//   本类自身一直活在主线程（QFutureWatcher 也在主线程），只跨线程搬运纯数据
//   TranslateJobRequest（入）/ TranslateJobResult（出）。
class TranslateJob : public QObject {
    Q_OBJECT

public:
    explicit TranslateJob(QObject *parent = nullptr);
    ~TranslateJob() override;

    bool isRunning() const;

public slots:
    // 重复调用在跑动期间会被忽略（GUI 侧同时也会禁用按钮，双保险）。
    void start(const TranslateJobRequest &req);

signals:
    // elapsedMs 由 QElapsedTimer 在 start() 起算，回到主线程时取。
    void finished(const TranslateJobResult &result, qint64 elapsedMs);

private:
    void onFutureFinished();

    QFutureWatcher<TranslateJobResult> *m_watcher = nullptr;
    QElapsedTimer m_timer;
};
