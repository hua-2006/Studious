#include "TranslatorRecords.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace studious::ui {
namespace {

// list() 的默认条数。B7 的列表一屏也就十几行，100 条够翻好一会儿，
// 又不至于把几千条记录（每条带缩略图路径 + 两段文本）一次性拉进内存。
constexpr int kDefaultListLimit = 100;

// 记录库的硬上限。为什么是 2000：
//   · 一条记录含缩略图路径 + 原文 + 译文，平均 1 KB 上下，2000 条约 2 MB，
//     对 SQLite 是小库，整表备份 / 重建索引都很轻快；
//   · 列表默认一次只看 100 条，2000 条已经是 20 屏历史——超过这个量的旧记录
//     用户根本翻不到，留着只让全表扫描变慢；
//   · 又不设成 500：用户偶尔会回头找"上周翻译的那段"，一次翻到的概率比想象中高，
//     2000 是在"翻得回去"和"库里别长胖"之间取得平衡的那个点。
constexpr qint64 kMaxRows = 2000;

// 列顺序与 rowToRecord() 严格对应，改一处必须改两处。
QString selectPrefix()
{
    return QStringLiteral("SELECT id, source, src, dst, engine, image, region, original,"
                          " translated, created FROM records");
}

// 错误文本：QSqlError 有时是空串（比如驱动干脆没初始化），
// 这时给个"未知错误"总比让界面显示"打开记录库失败："强。
QString describe(const QSqlQuery &query)
{
    const QString text = query.lastError().text().trimmed();
    return text.isEmpty() ? QStringLiteral("未知错误") : text;
}

QString describe(const QSqlDatabase &db)
{
    const QString text = db.lastError().text().trimmed();
    return text.isEmpty() ? QStringLiteral("未知错误") : text;
}

// 选区序列化成 "x,y,w,h"。空 / 非法矩形写 NULL（不是空串）：
// 读回来时 NULL 和空串都走同一条"没有选区"的分支，但 NULL 在库里一眼看得出来。
QString regionToText(const QRect &region)
{
    if (!region.isValid() || region.width() <= 0 || region.height() <= 0) {
        return QString();
    }
    return QStringLiteral("%1,%2,%3,%4")
        .arg(region.x())
        .arg(region.y())
        .arg(region.width())
        .arg(region.height());
}

// 反序列化。格式不对就当"没有选区"，不让一条脏数据把整个列表读挂。
QRect textToRegion(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != 4) {
        return QRect();
    }
    int values[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        values[i] = parts.at(i).trimmed().toInt(&ok);
        if (!ok) {
            return QRect();
        }
    }
    // 宽或高 <= 0 的 QRect 自身 isValid() == false，正好当"空选区"。
    return QRect(values[0], values[1], values[2], values[3]);
}

// 把用户输入包成 LIKE 的"包含"模式。
// % 和 _ 必须转义：用户搜 "50%" 时不该变成通配符，否则结果莫名其妙。
QString likeContains(const QString &raw)
{
    QString escaped = raw;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    return QStringLiteral("%%%1%%").arg(escaped);
}

// 建表 + 建索引。幂等：每次开库都跑一遍 CREATE ... IF NOT EXISTS，代价是几微秒。
bool ensureSchema(QSqlDatabase &db, QString *error)
{
    QSqlQuery query(db);
    const QString ddl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS records ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  parent     TEXT,"
        "  source     TEXT NOT NULL,"
        "  src        TEXT,"
        "  dst        TEXT,"
        "  engine     TEXT,"
        "  image      TEXT,"
        "  region     TEXT,"
        "  original   TEXT,"
        "  translated TEXT,"
        "  created    INTEGER"
        ")");
    if (!query.exec(ddl)) {
        if (error) {
            *error = QStringLiteral("建记录表失败：%1").arg(describe(query));
        }
        return false;
    }

    // 列表永远按时间倒序翻，加索引免得每次全表排序。
    QSqlQuery createdIndex(db);
    if (!createdIndex.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_records_created ON records(created DESC)"))) {
        if (error) {
            *error = QStringLiteral("建记录索引失败：%1").arg(describe(createdIndex));
        }
        return false;
    }

    // 实时翻译按 parentKey 取子记录 / 判空，没索引就是全表扫。
    QSqlQuery parentIndex(db);
    if (!parentIndex.exec(
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_records_parent ON records(parent)"))) {
        if (error) {
            *error = QStringLiteral("建记录索引失败：%1").arg(describe(parentIndex));
        }
        return false;
    }
    return true;
}

// 库里一行 -> 结构体。列顺序见 selectPrefix()。
TranslationRecord rowToRecord(const QSqlQuery &query)
{
    TranslationRecord rec;
    rec.id = query.value(0).toLongLong();
    rec.source = query.value(1).toString();
    rec.srcLang = query.value(2).toString();
    rec.dstLangs = query.value(3).toString();
    rec.engine = query.value(4).toString();
    rec.imagePath = query.value(5).toString();
    rec.region = textToRegion(query.value(6).toString());
    rec.originalText = query.value(7).toString();
    rec.translatedText = query.value(8).toString();
    const qint64 seconds = query.value(9).toLongLong();
    if (seconds > 0) {
        // 存的是秒（不是毫秒）：记录列表只显示到分钟，秒级精度足够，
        // 单列还能省一半空间。fromSecsSinceEpoch 默认按本地时区还原。
        rec.createdAt = QDateTime::fromSecsSinceEpoch(seconds);
    }
    return rec;
}

} // namespace

TranslatorRecords::TranslatorRecords(const QString &dbPath)
    : m_dbPath(dbPath),
      // 连接名必须唯一且只属于本对象：
      //   · 用 this 指针而不是 dbPath 的哈希 —— 同一个文件开两个实例
      //     （设置页一个、记录页一个）时不会互相抢连接；
      //   · 不用 QThread::currentThreadId() —— 本类承诺只在主线程用，
      //     连接跟着对象走比跟着线程走更好理解。
      m_connectionName(QStringLiteral("studious-records-%1")
                           .arg(reinterpret_cast<quintptr>(this)))
{
}

TranslatorRecords::~TranslatorRecords()
{
    if (!QSqlDatabase::contains(m_connectionName)) {
        return;
    }
    // 先让这条连接的所有 QSqlDatabase 副本析构，再 removeDatabase，
    // 否则 Qt 会在控制台刷 "connection is still in use" 警告。
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen()) {
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

QString TranslatorRecords::dbPath() const
{
    return m_dbPath;
}

QString TranslatorRecords::lastError() const
{
    return m_lastError;
}

QSqlDatabase TranslatorRecords::connection() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

bool TranslatorRecords::ensureOpen() const
{
    if (m_open) {
        return true;
    }

    const QString driver = QStringLiteral("QSQLITE");
    if (!QSqlDatabase::isDriverAvailable(driver)) {
        m_lastError = QStringLiteral("Qt SQLite 驱动不可用（QSQLITE），无法读写翻译记录");
        return false;
    }

    // 空路径 = 内存库（自测用）。文件路径先补父目录：
    // 首次启动时 AppData 下的目录往往还不存在，SQLite 自己不会建多级目录。
    const QString target = m_dbPath.isEmpty() ? QStringLiteral(":memory:")
                                              : QFileInfo(m_dbPath).absoluteFilePath();
    if (!m_dbPath.isEmpty()) {
        QDir().mkpath(QFileInfo(m_dbPath).absolutePath());
    }

    QSqlDatabase db = QSqlDatabase::contains(m_connectionName)
                          ? QSqlDatabase::database(m_connectionName, false)
                          : QSqlDatabase::addDatabase(driver, m_connectionName);

    if (!db.isOpen()) {
        db.setDatabaseName(target);
        if (!db.open()) {
            m_lastError = QStringLiteral("打开记录库失败：%1").arg(describe(db));
            return false;
        }
    }

    // WAL：记录页一边翻历史、一边可能还有实时翻译在追加，WAL 让读不阻塞写、
    //      写不阻塞读（默认的 rollback journal 会互相卡）；崩溃恢复也更快。
    // synchronous=NORMAL：在 WAL 下仍然保证一致性（断电最多丢最后一个事务），
    //      但比 FULL 少一次 fsync —— 记录不是财务数据，这个取舍划算。
    // busy_timeout：万一同进程里还有别的连接在写，等 3 秒而不是立刻报 SQLITE_BUSY。
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=3000"));

    if (!ensureSchema(db, &m_lastError)) {
        return false;
    }

    m_open = true;
    m_lastError.clear();
    return true;
}

qint64 TranslatorRecords::add(const TranslationRecord &rec)
{
    if (!ensureOpen()) {
        return 0;
    }
    // source 是 NOT NULL：宁可让调用方当场看到错误，也不悄悄写一条 "unknown"
    // 进去——来源徽章是 B7 的第一眼信息，猜错了比没有更糟。
    if (rec.source.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("写记录失败：source 不能为空（capture / mini / live）");
        return 0;
    }

    QSqlDatabase db = connection();
    if (!db.transaction()) {
        m_lastError = QStringLiteral("写记录失败：无法开启事务（%1）").arg(describe(db));
        return 0;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO records(parent, source, src, dst, engine, image, region, original,"
        " translated, created) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    // parent 写空串而不是 NULL：查询里统一写 parent = '' 就够，不用再 OR 一个 IS NULL。
    // 列本身保留（旧库里有），但不再有子记录——父子模型已废弃。
    query.addBindValue(QString());
    query.addBindValue(rec.source.trimmed());
    query.addBindValue(rec.srcLang);
    query.addBindValue(rec.dstLangs);
    query.addBindValue(rec.engine);
    // 空缩略图 / 空选区写 NULL：读回来时 toString() 一样是空，但库里一眼分得清。
    query.addBindValue(rec.imagePath.isEmpty()
                           ? QVariant(QMetaType::fromType<QString>())
                           : QVariant(rec.imagePath));
    const QString region = regionToText(rec.region);
    query.addBindValue(region.isEmpty() ? QVariant(QMetaType::fromType<QString>())
                                        : QVariant(region));
    query.addBindValue(rec.originalText);
    query.addBindValue(rec.translatedText);
    // 秒级精度；调用方没填时间就用当前时间（记录列表靠它排序，不能是 NULL）。
    query.addBindValue((rec.createdAt.isValid() ? rec.createdAt : QDateTime::currentDateTime())
                           .toSecsSinceEpoch());

    if (!query.exec()) {
        m_lastError = QStringLiteral("写记录失败：%1").arg(describe(query));
        db.rollback();
        return 0;
    }

    const qint64 id = query.lastInsertId().toLongLong();
    if (id <= 0) {
        m_lastError = QStringLiteral("写记录失败：插入后拿不到 id，已回滚");
        db.rollback();
        return 0;
    }

    if (!db.commit()) {
        m_lastError = QStringLiteral("写记录失败：提交事务失败（%1）").arg(describe(db));
        db.rollback();
        return 0;
    }

    // 上限剪枝：超过 kMaxRows 就删最旧的那批。
    // 只有真的超了才跑 DELETE（每插一条都跑一次删除 + 排序，写记录会明显变慢）。
    // 剪枝失败**不让这条记录失败**：数据已经落库了，回滚反而丢东西；
    // 错误只写进 lastError()，调用方看得见但不当致命错误处理。
    QSqlQuery countQuery(db);
    if (countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM records")) && countQuery.next()
        && countQuery.value(0).toLongLong() > kMaxRows) {
        // 一条 SQL 搞定：按 (时间, id) 倒序保留最新的 kMaxRows 条，之后的全删。
        // SQLite 的 LIMIT -1 表示"不限条数"（只用 OFFSET），所以不用先算要删多少。
        QSqlQuery prune(db);
        if (!prune.exec(QStringLiteral(
                "DELETE FROM records WHERE id IN ("
                "  SELECT id FROM records ORDER BY created DESC, id DESC"
                "  LIMIT -1 OFFSET %1)")
                .arg(kMaxRows))) {
            m_lastError = QStringLiteral("清理旧记录失败：%1").arg(describe(prune));
        }
    }
    return id;
}

QVector<TranslationRecord> TranslatorRecords::list(int limit, const QString &search) const
{
    QVector<TranslationRecord> rows;
    if (!ensureOpen()) {
        return rows;
    }
    // <= 0 视为调用方漏填：给默认值，绝不生成无 LIMIT 的全表扫描。
    const int clamped = limit > 0 ? limit : kDefaultListLimit;

    // 括号不能省：后面拼的搜索条件是 AND，若写成 "parent IS NULL OR parent = '' AND ..."
    // SQL 会按优先级解析成 "parent IS NULL OR (... AND ...)"，parent 为 NULL 的旧行
    // 就会绕过搜索过滤。
    QString sql = selectPrefix() + QStringLiteral(" WHERE (parent IS NULL OR parent = '')");

    const QString filter = search.trimmed();
    const bool hasFilter = !filter.isEmpty();
    if (hasFilter) {
        // 只搜原文 / 译文。ESCAPE 让 % 和 _ 回到字面量语义（转义在 likeContains 里做）。
        sql += QStringLiteral(" AND (original LIKE ? ESCAPE '\\'"
                              " OR translated LIKE ? ESCAPE '\\')");
    }
    sql += QStringLiteral(" ORDER BY created DESC, id DESC LIMIT ?");

    QSqlQuery query(connection());
    query.prepare(sql);
    if (hasFilter) {
        const QString pattern = likeContains(filter);
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    query.addBindValue(clamped);

    if (!query.exec()) {
        m_lastError = QStringLiteral("读取记录列表失败：%1").arg(describe(query));
        return rows;
    }
    while (query.next()) {
        rows.append(rowToRecord(query));
    }
    return rows;
}

bool TranslatorRecords::remove(qint64 id)
{
    if (id <= 0) {
        m_lastError = QStringLiteral("删除记录失败：id 无效（%1）").arg(id);
        return false;
    }
    if (!ensureOpen()) {
        return false;
    }

    QSqlDatabase db = connection();
    if (!db.transaction()) {
        m_lastError = QStringLiteral("删除记录失败：无法开启事务（%1）").arg(describe(db));
        return false;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM records WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = QStringLiteral("删除记录失败：%1").arg(describe(query));
        db.rollback();
        return false;
    }
    // 删一条不存在的记录不算成功：界面刚点过"删除"却什么都没变，
    // 与其静默返回 true，不如让调用方知道这条记录已经没了。
    if (query.numRowsAffected() == 0) {
        m_lastError = QStringLiteral("删除记录失败：没有找到 id=%1 的记录").arg(id);
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        m_lastError = QStringLiteral("删除记录失败：提交事务失败（%1）").arg(describe(db));
        db.rollback();
        return false;
    }
    return true;
}

int TranslatorRecords::clearAll()
{
    if (!ensureOpen()) {
        return -1;
    }
    QSqlDatabase db = connection();

    // 先数再删：界面要显示"已清空 N 条记录"。
    int removed = 0;
    {
        QSqlQuery countQuery(db);
        if (countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM records"))
            && countQuery.next()) {
            removed = countQuery.value(0).toInt();
        }
    }

    if (!db.transaction()) {
        m_lastError = QStringLiteral("清空记录失败：无法开启事务（%1）").arg(describe(db));
        return -1;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("DELETE FROM records"))) {
        m_lastError = QStringLiteral("清空记录失败：%1").arg(describe(query));
        db.rollback();
        return -1;
    }
    if (!db.commit()) {
        m_lastError = QStringLiteral("清空记录失败：提交事务失败（%1）").arg(describe(db));
        db.rollback();
        return -1;
    }
    return removed;
}

int TranslatorRecords::count() const
{
    if (!ensureOpen()) {
        return 0;
    }
    QSqlQuery query(connection());
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM records")) && query.next()) {
        return query.value(0).toInt();
    }
    m_lastError = QStringLiteral("统计记录条数失败：%1").arg(describe(query));
    return 0;
}

} // namespace studious::ui
