#include "LogSink.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>

#include <atomic>
#include <utility>

#include "AssetPaths.h"

namespace studious::ui::logsink {
namespace {

// 缓冲上限：再多也只留最近 4000 行 —— 日志窗口是"看最近发生了什么"，不是归档。
constexpr int kMaxLines = 4000;
// 单文件上限。超了把 studious.log 挪成 studious.1.log（只留一份旧档）。
constexpr qint64 kRotateBytes = 8LL * 1024 * 1024;
// 单条消息上限：某些异常会把整个 HTML / json 打进 qWarning，别让 4000 行吃爆内存。
constexpr int kMaxLineChars = 8000;

constexpr char kLogFileName[] = "studious.log";
constexpr char kRotatedFileName[] = "studious.1.log";

struct State {
    QMutex mutex;
    QStringList lines;
    std::function<void()> notify;

    QFile file;
    bool fileOpen = false;
    bool openFailed = false;   // 打开失败后不再每行重试，免得刷屏
    qint64 written = 0;

    QString filePath;          // 懒解析（install 时定下来）
    QString rotatedPath;
};

State &state()
{
    // 故意漏出去、不做静态析构：
    // 进程退出阶段（Qt 的线程池/静态对象正在拆）仍然会有 qWarning 进来 —— 如果 State 已经
    // 被析构，那条日志会写进一个已关闭的 QFile，打出 "QIODevice::write: device not open"，
    // 运气不好还会踩到已析构的 std::function（实测 --ui-shot 偶发退出码 0xC0000005）。
    // 一个 QFile + 一个 QStringList 的泄漏换掉整个类别的退出期崩溃，值。
    static State *s = new State;
    return *s;
}

// 与 paths::assetsRoot() 同口径：默认资产根是 <AppData>/studious/assets，
// 日志放它的父目录下的 logs/，即 <AppData>/studious/logs。
// assetRoot 被用户改到别处时，资产和日志一起搬走（历史目录 historyDir() 也是这个思路）。
void resolvePathsLocked()
{
    State &s = state();
    if (!s.filePath.isEmpty()) {
        return;
    }
    const QDir dataRoot = QFileInfo(studious::ui::paths::assetsRoot()).absoluteDir();
    const QDir logDir(dataRoot.filePath(QStringLiteral("logs")));
    QDir().mkpath(logDir.absolutePath());
    s.filePath = logDir.filePath(QLatin1String(kLogFileName));
    s.rotatedPath = logDir.filePath(QLatin1String(kRotatedFileName));
}

char levelChar(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return 'D';
    case QtInfoMsg:
        return 'I';
    case QtWarningMsg:
        return 'W';
    case QtCriticalMsg:
    case QtFatalMsg:
        return 'E';
    }
    return 'I';
}

void rotateLocked()
{
    State &s = state();
    s.file.close();
    s.fileOpen = false;
    QFile::remove(s.rotatedPath);
    QFile::rename(s.filePath, s.rotatedPath);
    s.written = 0;
}

// 调用方持锁。
void writeLocked(const QString &line)
{
    State &s = state();
    resolvePathsLocked();
    if (s.filePath.isEmpty() || s.openFailed) {
        return;
    }
    if (!s.fileOpen) {
        s.file.setFileName(s.filePath);
        if (!s.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            // 这里绝对不能打日志：我们自己就是 handler，会递归。
            s.openFailed = true;
            return;
        }
        s.fileOpen = true;
        s.written = s.file.size();
    }
    if (s.written > kRotateBytes) {
        rotateLocked();
        s.file.setFileName(s.filePath);
        if (!s.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            s.openFailed = true;
            return;
        }
        s.fileOpen = true;
    }

    const QByteArray bytes = line.toUtf8() + '\n';
    // 行缓冲：每行写完就 flush —— 崩溃时最多丢正在写的那一行。
    if (s.file.write(bytes) > 0) {
        s.written += bytes.size();
    }
    s.file.flush();
}

void appendMessage(QtMsgType type, QString msg)
{
    while (msg.endsWith(QLatin1Char('\n')) || msg.endsWith(QLatin1Char('\r'))) {
        msg.chop(1);
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString prefix = stamp + QLatin1String(" [") + QLatin1Char(levelChar(type)) + QLatin1String("] ");

    // 多行消息拆成多条，保住「一条 = 一行」的不变量（日志窗口按行渲染）。
    QStringList pieces = msg.split(QLatin1Char('\n'));
    if (pieces.size() > 40) {
        // 极端情况（整段堆栈 / 整个响应体）：只留前 40 行，避免一次写入卡住 UI 线程。
        const int dropped = pieces.size() - 40;
        pieces = pieces.mid(0, 40);
        pieces.append(QStringLiteral("… (%1 more lines truncated)").arg(dropped));
    }

    std::function<void()> notify;
    {
        QMutexLocker locker(&state().mutex);
        State &s = state();
        for (QString piece : std::as_const(pieces)) {
            if (piece.endsWith(QLatin1Char('\r'))) {
                piece.chop(1);
            }
            if (piece.size() > kMaxLineChars) {
                piece.truncate(kMaxLineChars);
                piece.append(QStringLiteral(" …"));
            }
            const QString line = prefix + piece;
            s.lines.append(line);
            if (s.lines.size() > kMaxLines) {
                s.lines.remove(0, s.lines.size() - kMaxLines);
            }
            writeLocked(line);
        }
        notify = s.notify;
    }

    // 锁外回调：通知方（AppBackend）可能要排队到主线程，拿着锁等它不安全。
    if (notify) {
        notify();
    }
}

std::atomic<QtMessageHandler> g_previous{nullptr};
std::atomic_bool g_installed{false};
// 防递归：handler 自己（含落盘失败路径）打的日志只转发，不再进缓冲。
thread_local bool t_inHandler = false;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QtMessageHandler previous = g_previous.load(std::memory_order_relaxed);

    if (!t_inHandler) {
        t_inHandler = true;
        appendMessage(type, msg);
        t_inHandler = false;
    }

    // 转发给原来的 handler（默认 handler 写 stderr）。吞掉它，--smoke 的终端输出就没了。
    if (previous != nullptr) {
        previous(type, context, msg);
    }
}

}   // namespace

void install()
{
    if (g_installed.exchange(true)) {
        return;
    }

    {
        // 先建目录：第一行日志（下面那行 [logsink]）要能落盘。
        QMutexLocker locker(&state().mutex);
        resolvePathsLocked();
    }

    g_previous.store(qInstallMessageHandler(&messageHandler), std::memory_order_relaxed);

    // 有意留这一行：验收时看文件里有没有它就是「日志汇到底装没装」。
    qInfo("[logsink] installed, file=%s", qUtf8Printable(filePath()));
}

QString filePath()
{
    QMutexLocker locker(&state().mutex);
    resolvePathsLocked();
    return state().filePath;
}

QString text()
{
    QMutexLocker locker(&state().mutex);
    return state().lines.join(QLatin1Char('\n'));
}

QStringList tail(int maxLines)
{
    if (maxLines <= 0) {
        return {};
    }
    QMutexLocker locker(&state().mutex);
    const QStringList &lines = state().lines;
    if (lines.size() <= maxLines) {
        return lines;
    }
    return lines.mid(lines.size() - maxLines);
}

void clear()
{
    QMutexLocker locker(&state().mutex);
    state().lines.clear();
}

void setNotify(std::function<void()> notify)
{
    QMutexLocker locker(&state().mutex);
    state().notify = std::move(notify);
}

void flushToDisk()
{
    QMutexLocker locker(&state().mutex);
    if (state().fileOpen) {
        state().file.flush();
    }
}

}   // namespace studious::ui::logsink
