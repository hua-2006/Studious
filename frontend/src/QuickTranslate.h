#pragma once

// 三页（截屏翻译 / 小窗翻译 / 实时翻译）共用的翻译入口。
//
// 设计约束：
//  * 自带一个 TranslateJob，绝不复用 AppBackend::m_job（那样两处会互相覆盖状态）。
//  * 不持有、也不 include AppBackend；引擎 id 换算与本地引擎懒启动照着 AppBackend 的做法抄，
//    但配置来源只有 AppSettings::load()（每次 translate() 重新读，配置改完立刻生效）。
//  * 头文件里只前向声明 TranslateJob / TranslateJobResult，不 include 任何前端实现头
//    （本地引擎藏在 .cpp 的进程级单例里，翻译缓存走 CacheHandle.h 的 sharedCache()）。
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

#include <functional>

class TranslateJob;
struct TranslateJobResult;
class LocalEngine;   // 定义在 LocalEngine.h（全局命名空间）

namespace studious::translate {
class LocalMtTranslator;
}

namespace studious::ui {

// ─── 进程级共享的本地 LLM 引擎 ──────────────────────────────────────────────
// 以前 AppBackend 自己持有一个 LocalEngine、这里的 CLI/快捷入口另建一个：
// 界面从配置页启动的是 A，截图/实时/小窗检查的是 B —— B 永远「没在跑」，
// 于是「本地 LLM 没启动」这条错误永远消不掉，还会每次重试都重复拉起一遍。
// 现在全进程只有这一个实例（AppBackend 也用它的引用）。
LocalEngine &sharedLocalEngine();
// ─── 进程级共享的本地 MT 实例 ───────────────────────────────────────────────
// 为什么要有：本地 MT 的 ONNX 会话建一次要几百毫秒、占几十 MB。以前每个翻译作业
// 都新建一个 LocalMtTranslator，作业结束就释放 —— 每次翻译都重新加载。
// 现在三页共用这一个实例（LocalMtTranslator 内部加了锁），界面上：
//   「开机自启」= 启动时就 warmMtAll()；「空闲关闭」= 到点 unloadMtAll()。
studious::translate::LocalMtTranslator &sharedMt();
int warmMtAll();                       // 预热所有已安装的语言对，返回成功数
void unloadMtAll();                    // 释放全部常驻语言对
int mtLoadedPairs();                   // 当前常驻了几个语言对
// 每次真正用本地 MT 翻完一次就回调一次，给 AppBackend 的空闲计时器续命。
void setMtUsedHook(std::function<void()> hook);

// 本地 LLM 没起来时，由 AppBackend 注入一个「异步启动」的动作。
// 注入之后 QuickTranslate 不再让用户自己去配置页点启动，而是发起一次并让调用方重试。
void setLocalEngineStarter(std::function<void()> starter);

// 一次请求的结果：按目标语言分组，每组与输入段落 1:1 等长（缺的填空串）。
struct QuickTranslateResult {
    bool ok = false;
    QString error;
    // 非阻断提示：翻译成功，但有段落没翻成目标语言、按原文显示了。
    // 界面必须用显眼但不报错的方式说一句，否则用户会把"原文"当成"译文"。
    QString warning;
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

    // 上一次 translate() 被同步拒绝的原因（busy / 空输入 / 本地引擎起不来）。
    // 异步失败的原因在 finished().error 里，不在这里。
    QString lastError() const;

    // 上一次同步拒绝是不是「等一下就会好」的（目前只有本地 LLM 正在启动这一种）。
    // 调用方看到 true 应该自己安排重试，而不是把错误摆到用户脸上。
    bool lastFailureRetryable() const;

    // text 按空行切段（规则见 Paragraphs.h，与主页 / 小窗完全同一份实现）。
    // dstLangs 走 langs::normalizeTargets：trim / 去重 / 空则回落 {zh} / 本地 MT 只留第一个。
    // srcLang 可为 "auto"/空（内部折成 "auto"）。
    // 同一个 QuickTranslate 上并发调用 → 返回 false 并设置 error（不排队）。
    // bypassCache=true：这一次不读也不写翻译缓存（界面上「重新翻译」用它 ——
    // 走缓存会立刻把上一份结果原样拿回来，按了像没反应）。
    bool translate(const QString &text, const QString &srcLang, const QStringList &dstLangs,
                   bool bypassCache = false);

    // 这一次调用用哪个引擎（界面 id：web / llm-cloud / llm-local / local-mt），
    // 空串 = 跟设置走。实时翻译的引擎下拉用它 —— 只影响这一处，不动全局设置。
    void setEngineOverride(const QString &uiEngineId);
    void clearEngineOverride();

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
    bool m_retryable = false;
    QString m_pendingEngineLabel;   // 下发前就拼好，避免回来时设置已变
    QStringList m_pendingDstLangs;
    QStringList m_pendingParagraphs;

    // 引擎覆盖：实时翻译下拉选的界面 id。hasEngineOverride=false 时完全跟设置走。
    bool m_hasEngineOverride = false;
    QString m_engineOverride;
};

} // namespace studious::ui

Q_DECLARE_METATYPE(studious::ui::QuickTranslateResult)
