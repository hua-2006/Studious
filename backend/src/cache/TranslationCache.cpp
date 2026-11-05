#include "cache/TranslationCache.h"

#include "i18n/L10n.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

namespace studious::cache {
namespace {

// 每个线程独立连接名。QSqlDatabase 不允许跨线程复用同一连接。
QString threadConnectionName(const QString &prefix)
{
    return prefix + QStringLiteral("_%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

bool ensureSchema(QSqlDatabase &db, QString *error)
{
    QSqlQuery query(db);
    const QString ddl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS translations ("
        "  key        TEXT PRIMARY KEY,"
        "  source     TEXT NOT NULL,"
        "  translated TEXT NOT NULL,"
        "  engine     TEXT,"
        "  src_lang   TEXT,"
        "  dst_lang   TEXT,"
        "  created_at INTEGER NOT NULL,"
        "  hit_count  INTEGER NOT NULL DEFAULT 0"
        ")");
    if (!query.exec(ddl)) {
        if (error) {
            *error = i18n::s("建表失败：", "Failed to create the cache table: ")
                 + query.lastError().text();
        }
        return false;
    }
    // 剪枝时按时间排序，建个索引免得全表扫。
    QSqlQuery index(db);
    index.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_translations_created "
                              "ON translations(created_at)"));
    return true;
}

} // namespace

TranslationCache::TranslationCache(const QString &dbPath, int maxEntries)
    : dbPath_(dbPath), maxEntries_(maxEntries > 0 ? maxEntries : 100000)
{
}

TranslationCache::~TranslationCache()
{
    const QString name = connectionName();
    if (QSqlDatabase::contains(name)) {
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
}

QString TranslationCache::connectionName() const
{
    // 用 dbPath 的哈希做前缀，保证同一个文件在不同 TranslationCache 实例间也能区分。
    const QString prefix = QStringLiteral("studious_cache_%1")
                               .arg(QString::fromLatin1(
                                   QCryptographicHash::hash(dbPath_.toUtf8(), QCryptographicHash::Md5)
                                       .toHex()));
    return threadConnectionName(prefix);
}

bool TranslationCache::ensureOpen(QString *error) const
{
    const QString name = connectionName();
    if (QSqlDatabase::contains(name)) {
        QSqlDatabase existing = QSqlDatabase::database(name, false);
        if (existing.isOpen()) {
            return true;
        }
    }

    const QString driver = QStringLiteral("QSQLITE");
    if (!QSqlDatabase::isDriverAvailable(driver)) {
        if (error) {
            *error = i18n::s("Qt SQLite 驱动不可用（QSQLITE）",
                         "The Qt SQLite driver (QSQLITE) is not available");
        }
        return false;
    }

    QSqlDatabase db = QSqlDatabase::contains(name)
                          ? QSqlDatabase::database(name, false)
                          : QSqlDatabase::addDatabase(driver, name);

    if (dbPath_.isEmpty()) {
        db.setDatabaseName(QStringLiteral(":memory:"));
    } else {
        const QFileInfo info(dbPath_);
        QDir().mkpath(info.absolutePath());
        db.setDatabaseName(info.absoluteFilePath());
    }

    if (!db.open()) {
        if (error) {
            *error = i18n::s("打开缓存库失败：", "Failed to open the cache database: ")
                 + db.lastError().text();
        }
        return false;
    }

    // 并发写入时不要立刻报 SQLITE_BUSY。
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));

    if (!ensureSchema(db, error)) {
        return false;
    }
    return true;
}

QString TranslationCache::makeKey(const QString &engineTag,
                                  const QString &srcLang,
                                  const QString &dstLang,
                                  const QString &text)
{
    // 用 \x1f 做分隔符：正常文本里不会出现，避免 "a|b" 与 "a" + "|b" 撞键。
    const QString joined = engineTag + QChar(0x1f) + srcLang + QChar(0x1f) + dstLang
                           + QChar(0x1f) + text;
    return QString::fromLatin1(
        QCryptographicHash::hash(joined.toUtf8(), QCryptographicHash::Sha256).toHex());
}

std::optional<QString> TranslationCache::lookup(const QString &key) const
{
    if (!ensureOpen()) {
        ++misses_;
        return std::nullopt;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName(), false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT translated FROM translations WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec()) {
        ++misses_;
        return std::nullopt;
    }
    if (!query.next()) {
        ++misses_;
        return std::nullopt;
    }

    const QString translated = query.value(0).toString();
    ++hits_;

    // 记录命中次数，将来可以用它做淘汰优先级（常用的反而保留）。
    QSqlQuery bump(db);
    bump.prepare(QStringLiteral("UPDATE translations SET hit_count = hit_count + 1 WHERE key = ?"));
    bump.addBindValue(key);
    bump.exec();

    return translated;
}

void TranslationCache::store(const QString &key,
                             const QString &source,
                             const QString &translated,
                             const QString &engine,
                             const QString &srcLang,
                             const QString &dstLang)
{
    if (translated.trimmed().isEmpty()) {
        return;   // 空译文不缓存，否则下次会命中一个空结果
    }
    if (!ensureOpen()) {
        return;
    }

    QSqlDatabase db = QSqlDatabase::database(connectionName(), false);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO translations(key, source, translated, engine, src_lang, dst_lang, created_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET translated = excluded.translated, "
        "                              created_at = excluded.created_at"));
    query.addBindValue(key);
    query.addBindValue(source);
    query.addBindValue(translated);
    query.addBindValue(engine);
    query.addBindValue(srcLang);
    query.addBindValue(dstLang);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.exec();

    // 超过上限就删最旧的一批（一次删 5%，避免每插一条都做统计）。
    if (maxEntries_ > 0) {
        QSqlQuery countQuery(db);
        if (countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) && countQuery.next()) {
            const qint64 total = countQuery.value(0).toLongLong();
            if (total > maxEntries_) {
                const qint64 drop = qMax<qint64>(1, total - maxEntries_);
                QSqlQuery prune(db);
                prune.prepare(QStringLiteral(
                    "DELETE FROM translations WHERE key IN ("
                    "  SELECT key FROM translations ORDER BY created_at ASC LIMIT ?"
                    ")"));
                prune.addBindValue(drop);
                prune.exec();
            }
        }
    }
}

qint64 TranslationCache::count() const
{
    if (!ensureOpen()) {
        return 0;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName(), false);
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM translations")) && query.next()) {
        return query.value(0).toLongLong();
    }
    return 0;
}

void TranslationCache::clear()
{
    if (!ensureOpen()) {
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName(), false);
    QSqlQuery query(db);
    query.exec(QStringLiteral("DELETE FROM translations"));
}

QString TranslationCache::path() const
{
    return dbPath_;
}

CacheStats TranslationCache::stats() const
{
    CacheStats s;
    s.hits = hits_;
    s.misses = misses_;
    return s;
}

void TranslationCache::resetStats() const
{
    hits_ = 0;
    misses_ = 0;
}

} // namespace studious::cache
