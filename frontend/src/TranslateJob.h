#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>

#include <studious/contract/Types.h>

// 把"输入文本 -> 段落列表"的切分规则集中在一处，GUI 与 --smoke 共用同一份实现。
QStringList splitParagraphs(const QString &text);

// 一次翻译作业：把 router.translate() 丢到工作线程跑，结果通过信号回主线程。
//
// 线程约束（关键）：
//   QtHttpClient / QNetworkAccessManager 是线程亲和的，所以 http 实例、
//   factory 装配出来的 translator 集合、TranslatorRouter 全部在**工作线程内部**
//   构造并在同一线程内使用、销毁，绝不跨线程共享。
//   本类自身一直活在主线程（QFutureWatcher 也在主线程），只跨线程搬运纯数据
//   TranslateRequest / TranslateOutcome。
class TranslateJob : public QObject {
    Q_OBJECT

public:
    explicit TranslateJob(QObject *parent = nullptr);
    ~TranslateJob() override;

    bool isRunning() const;

public slots:
    // 重复调用在跑动期间会被忽略（GUI 侧同时也会禁用按钮，双保险）。
    void start(const studious::core::TranslateRequest &req);

signals:
    // elapsedMs 由 QElapsedTimer 在 start() 起算，回到主线程时取。
    void finished(const studious::core::TranslateOutcome &outcome, qint64 elapsedMs);

private:
    void onFutureFinished();

    QFutureWatcher<studious::core::TranslateOutcome> *m_watcher = nullptr;
    QElapsedTimer m_timer;
};
