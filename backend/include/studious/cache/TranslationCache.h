#pragma once

#include <QString>

#include <atomic>

#include <optional>

namespace studious::cache {

// 缓存命中统计。前端可以显示"命中 3/5"，让用户看见缓存真的在省时间。
struct CacheStats {
    qint64 hits = 0;
    qint64 misses = 0;
};

// 翻译记忆（Translation Memory）：原文 -> 译文，落 SQLite。
//
// 为什么值得做：LLM 每次请求有约 1.1s 的固定首字延迟，而 galgame/界面的文本
// 重复率极高（同一句台词、菜单项、角色口头禅）。命中一次就省掉一整次往返。
//
// 线程模型：本类可被多个线程共享。QSqlDatabase 是线程亲和的，所以每个线程
// 首次访问时会用自己的连接名单独打开一次（QLockFile 由 SQLite 自己处理并发）。
//
// 关闭方式：调用方不注入本对象、或用 setEnabled(false)，Router 就会完全绕过缓存。
class TranslationCache {
public:
    // dbPath 传空则退化为纯内存（":memory:"），主要用于自测。
    explicit TranslationCache(const QString &dbPath = QString(),
                              int maxEntries = 100000);
    ~TranslationCache();

    TranslationCache(const TranslationCache &) = delete;
    TranslationCache &operator=(const TranslationCache &) = delete;

    // 打开数据库（每个线程第一次调用时自动执行）；失败时 error 带原因。
    bool ensureOpen(QString *error = nullptr) const;

    // 查一条。未命中返回空 optional。
    std::optional<QString> lookup(const QString &key) const;

    // 存一条（同 key 覆盖）。
    void store(const QString &key,
               const QString &source,
               const QString &translated,
               const QString &engine,
               const QString &srcLang,
               const QString &dstLang);

    // 缓存键：引擎标签 + 语言对 + 原文 的 SHA-256。
    //
    // 必须把引擎标签算进去：换个模型/换家翻译源，同一句原文的译文可能完全不同，
    // 混用会让用户以为"翻译坏了"。
    static QString makeKey(const QString &engineTag,
                           const QString &srcLang,
                           const QString &dstLang,
                           const QString &text);

    qint64 count() const;
    void clear();

    QString path() const;
    CacheStats stats() const;
    void resetStats() const;

private:
    QString dbPath_;
    int maxEntries_ = 100000;

    // 统计会被多个工作线程同时累加（命中/未命中都在 worker 里发生），必须是原子的。
    mutable std::atomic<qint64> hits_{0};
    mutable std::atomic<qint64> misses_{0};

    // 每个线程一个连接：连接名 = 前缀 + 线程 id。
    QString connectionName() const;
};

} // namespace studious::cache
