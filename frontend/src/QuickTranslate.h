#pragma once

// 三页（截屏翻译 / 小窗翻译 / 实时翻译）共用的翻译入口。
//
// 设计约束：
//  * 自带一个 TranslateJob，绝不复用 AppBackend::m_job（那样两处会互相覆盖状态）。
//  * 不持有、也不 include AppBackend；引擎 id 换算与本地引擎懒启动照着 AppBackend 的做法抄，
//    但配置来源只有 AppSettings::load()（每次 translate() 重新读，配置改完立刻生效）。
//  * 头文件里只前向声明 TranslateJob / TranslateJobResult，不 include 任何前端实现头
//    （本地引擎与缓存都藏在 .cpp 的进程级单例里，见 QuickTranslate.cpp）。
//  * 线程：全部在主线程，工作线程由 TranslateJob 自己管。
//
// 用法（C++ 侧）：
//     auto *qt = new QuickTranslate(this);
//     connect(qt, &QuickTranslate::finished, this, [](const QuickTranslateResult &r) { ... });
//     if (!qt->translate(text, "auto", {"zh"})) { 用 qt->lastError() 取原因 }

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

class TranslateJob;
struct TranslateJobResult;

namespace studious::ui {

// 一次请求的结果：按目标语言分组，每组与输入段落 1:1 等长（缺的填空串）。
struct QuickTranslateResult {
    bool ok = false;
    QString error;
    QString engineLabel;                       // 例 "Web 在线 · google" 或 "本地 MT"
    QHash<QString, QStringList> byLang;        // dstLang -> 段落译文
    QStringList paragraphs;                    // 回显输入分段（调用方可能没存）
    int cacheHits = 0;
    int cacheMisses = 0;
    qint64 elapsedMs = 0;
};

class QuickTranslate final : public QObject {
    Q_OBJECT

public:
    explicit QuickTranslate(QObject *parent = nullptr);
    ~QuickTranslate() override;

    bool isBusy() const;

    // 上一次 translate() 被同步拒绝的原因（busy / 空输入 / 没有目标语言 / 本地引擎起不来）。
    // 异步失败的原因在 finished().error 里，不在这里。
    QString lastError() const;

    // text 会按空行切段（连续空行算一个分隔，段内单换行保留）。
    // dstLangs 至少一个；srcLang 可为 "auto"/空。
    // 同一个 QuickTranslate 上并发调用 → 返回 false 并设置 error（不排队）。
    bool translate(const QString &text, const QString &srcLang, const QStringList &dstLangs);

    // 主动取消：立刻 emit 一个 ok=false /「已取消」的 finished，并把 isBusy() 放开。
    // 同时给 TranslateJob 发取消信号 —— 后端 translator 的段落循环会在下一个检查点
    // 退出（已经在飞的那一次 HTTP 请求没法硬打断，最多等它超时/返回）。
    void cancel();

signals:
    void finished(const studious::ui::QuickTranslateResult &result);
    void started();
    void progress(int done, int total);   // 段落级进度，可只发 0/total 与 total/total

    // 同步拒绝（translate() 返回 false）时发一次；这种情况下不会再有 finished()。
    void errorOccurred(const QString &message);

private:
    void onJobFinished(const TranslateJobResult &result, qint64 elapsedMs);

    TranslateJob *m_job = nullptr;
    bool m_busy = false;
    bool m_cancelled = false;

    // 在途请求的快照：结果回来时据此补齐段落 / 回填引擎名（期间设置可能已被改）。
    QString m_lastError;
    QString m_pendingEngineLabel;   // 下发前就拼好，避免回来时设置已变
    QStringList m_pendingDstLangs;
    QStringList m_pendingParagraphs;
};

} // namespace studious::ui

Q_DECLARE_METATYPE(studious::ui::QuickTranslateResult)
