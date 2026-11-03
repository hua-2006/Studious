#include "MainWindow.h"
#include "TranslateJob.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <studious/contract/Types.h>

#include <cstdio>
#include <cstring>

namespace {

// 控制台一律写 UTF-8 字节（与 backend_selftest 一致）。
// Windows Terminal / PowerShell 7 默认 65001；老控制台请先 chcp 65001。
void say(const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void printUsage()
{
    say(QStringLiteral("用法:"));
    say(QStringLiteral("  studious.exe                                       # 启动 GUI"));
    say(QStringLiteral("  studious.exe --smoke \"文本\" --to zh --engine web    # 无界面冒烟"));
    say(QStringLiteral("  可选: --to <lang> (默认 zh) / --engine <mock|web|llm|local-mt> (默认自动)"));
}

// --smoke 冒烟：不建窗口，但走与 GUI 完全相同的路径
// （TranslateJob -> 工作线程 -> QtHttpClient + factory + TranslatorRouter）。
int runSmoke(const QStringList &args)
{
    QString text;
    QString to = QStringLiteral("zh");
    QString engine;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const bool hasNext = (i + 1) < args.size();
        if (arg == QLatin1String("--smoke")) {
            // --smoke 后面紧跟的第一个非 "--" 开头的参数就是待翻译文本。
            if (hasNext && !args.at(i + 1).startsWith(QLatin1String("--"))) {
                text = args.at(++i);
            }
        } else if (arg == QLatin1String("--to") && hasNext) {
            to = args.at(++i);
        } else if (arg == QLatin1String("--engine") && hasNext) {
            engine = args.at(++i);
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    if (text.trimmed().isEmpty()) {
        say(QStringLiteral("[FAIL] --smoke 缺少待翻译文本"));
        printUsage();
        return 1;
    }

    studious::core::TranslateRequest req;
    req.paragraphs = splitParagraphs(text);
    if (req.paragraphs.isEmpty()) {
        say(QStringLiteral("[FAIL] 切分后没有任何段落"));
        return 1;
    }
    req.srcLang = QStringLiteral("auto");
    req.dstLangs = QStringList{to};
    req.preferredEngine = engine;

    say(QStringLiteral("=== Studious 前端冒烟 ==="));
    say(QStringLiteral("engine=%1  to=%2  src=auto  paragraphs=%3")
            .arg(engine.isEmpty() ? QStringLiteral("auto") : engine, to)
            .arg(req.paragraphs.size()));

    TranslateJob job;
    QEventLoop loop;
    int exitCode = 1;

    QObject::connect(&job,
                     &TranslateJob::finished,
                     &loop,
                     [&](const studious::core::TranslateOutcome &outcome, qint64 elapsedMs) {
                         say(QStringLiteral("耗时: %1 ms").arg(elapsedMs));
                         if (!outcome.ok) {
                             say(QStringLiteral("[FAIL] ") + outcome.error);
                             exitCode = 1;
                         } else {
                             const QStringList lines = outcome.byLang.value(to);
                             say(QStringLiteral("译文:"));
                             for (const QString &line : lines) {
                                 say(line);
                             }
                             exitCode = 0;
                         }
                         loop.quit();
                     });

    job.start(req);

    // 包一层超时，避免个别网络环境把冒烟挂死。
    QTimer::singleShot(60000, &loop, [&]() {
        say(QStringLiteral("[FAIL] 冒烟超时（60s）"));
        exitCode = 1;
        loop.quit();
    });

    loop.exec();
    say(exitCode == 0 ? QStringLiteral("[OK] exit 0") : QStringLiteral("[OK] exit 1"));
    return exitCode;
}

bool hasSmokeFlag(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    // 先看命令行：冒烟模式只起 QCoreApplication，不碰 GUI 平台插件。
    if (hasSmokeFlag(argc, argv)) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        return runSmoke(QCoreApplication::arguments());
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("studious"));

    MainWindow window;
    window.show();
    return app.exec();
}
