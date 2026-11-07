#pragma once

#include <QDateTime>
#include <QRect>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

// 翻译记录的持久化（截屏 / 小窗 / 实时三种来源）。
//
// 为什么放在 frontend 而不是 backend：
//   记录是**界面产物**——它带截图缩略图路径、屏幕选区、列表分组（实时翻译的父记录 +
//   子记录），这些都是 UI 语义；backend 的验收线是"纯翻译、零 GUI、零平台头"，
//   把历史记录塞进去只会让后端跟着界面一起长。
//   （backend 里那份 studious::records::RecordStore 是另一套更重的分段模型，
//    本模块刻意不复用它，也不与它共享 db 文件。）
//
// 表结构只有一张表：一条记录一行，原文/译文各一列。
// 不做分段表：B7 的详情页是"一段 OCR 原文 + 一段译文"的整体展示与编辑，
// 分段是 Router 的工作，历史里只留最终结果。
//
// 线程模型（**关键**）：
//   本类**只能在主线程使用，不能跨线程共享该对象**。
//   QSqlDatabase 是线程亲和的，而本类的连接名由 this 指针决定、连接在构造后
//   一直复用，所以它不是后端 TranslationCache 那种"每线程各开一条连接"的写法。
//   需要后台写记录时，请把纯数据（TranslationRecord）搬回主线程再调 add()，
//   不要在工作线程里直接摸这个对象。

namespace studious::ui {

// 一条记录。字段铺平（而不是直接塞 Router 的请求/响应对象）：
// 记录是给"历史"用的，格式稳定比传参方便更重要——后端契约改结构时这里不用跟着动。
struct TranslationRecord {
    qint64 id = 0;
    QString source;        // "capture" | "mini" | "live"
    QString srcLang;       // 可为空（自动检测）
    QString dstLangs;      // 逗号拼接，与 AppSettings 一致
    QString engine;        // 引擎 label，例 "本地 MT"
    QString imagePath;     // 缩略图路径（可为空）
    QRect region;          // 屏幕选区（物理像素，可为空）
    QString originalText;  // OCR / 输入原文
    QString translatedText;// 译文
    QDateTime createdAt;
};

// 翻译记录仓库（SQLite）。
//
// 失败语义：任何失败路径都只做两件事——把原因写进 lastError()（中文人话），
// 然后返回 false / 0 / -1 / 空列表。**不抛异常、不 abort**：
// 记录写不进去是次要功能故障，不该拖垮整个翻译窗口。
class TranslatorRecords final {
public:
    // dbPath 传空则退化为内存库（":memory:"），主要用于自测 / 冒烟。
    // 父目录不存在会自动 mkpath —— 首次启动时 AppData 下的目录往往还没建。
    explicit TranslatorRecords(const QString &dbPath);
    ~TranslatorRecords();

    // 连接名 / QSqlDatabase 都绑定在 this 上，复制会让两个对象抢同一条连接。
    TranslatorRecords(const TranslatorRecords &) = delete;
    TranslatorRecords &operator=(const TranslatorRecords &) = delete;

    // 首次调用时才真的开库（构造不碰磁盘，失败也拖不到构造里去报）。
    bool isOpen() const;
    QString lastError() const;

    // key 是记录的唯一标识：给实时翻译的父记录用
    // （实时一次会话一条父记录 + N 条子记录，子记录都带同一个 parentKey）。
    //
    // 成功返回新记录 id（> 0）；失败返回 0，原因见 lastError()。
    //
    // 推荐 parentKey 用 QStringLiteral("r%1").arg(父记录 id)：
    //   表里只有 parent 列、没有"父行自己的 key"列，用一次性 uuid 的话重启后
    //   就再也无法从某条父记录反查出它的子记录（见 README 里的遗留风险）。
    qint64 add(const TranslationRecord &rec, const QString &parentKey = QString());

    // 列表，默认 100 条，按时间倒序。
    // **只返回顶层行**（parent 为空）：实时翻译的子记录不混进主列表，
    // 展开某一行时用 children(parentKey) 单独取。
    // search 只匹配 original / translated，空串 = 不过滤。
    QVector<TranslationRecord> list(int limit = 100, const QString &search = QString()) const;

    // 某个 parentKey 下的子记录，按时间倒序（最新在上，与 B7 的子列表一致）。
    // parentKey 为空 = 没有子记录，返回空列表（这不算错误）。
    QVector<TranslationRecord> children(const QString &parentKey) const;

    // 删一条。id 不存在时返回 false 并说明原因。
    // 注意：删父记录**不会**连带删它的子记录（父行里没有 key，反查不到）；
    // 要整组删，先 children() 拿 id 逐条 remove()。
    bool remove(qint64 id);

    // 清空全部，返回删掉的行数；出错返回 -1。
    int clearAll();

    int count() const;

    // 构造时传进来的原始路径（空 = 内存库），给设置页显示用。
    QString dbPath() const;

private:
    // 打开 / 复用连接并建表；失败时 m_lastError 已有原因。
    bool ensureOpen() const;
    QSqlDatabase connection() const;

    QString m_dbPath;
    QString m_connectionName;
    mutable QString m_lastError;
    mutable bool m_open = false;
};

} // namespace studious::ui
