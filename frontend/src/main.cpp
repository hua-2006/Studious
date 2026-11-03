#include "AppSettings.h"
#include "MainWindow.h"
#include "TranslateJob.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <studious/cache/TranslationCache.h>
#include <studious/contract/Types.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>

#include <cstdio>
#include <cstring>

namespace {

// 进程级唯一的翻译缓存（GUI / --smoke 共用同一个 db 文件）。
//
// 两个刻意的设计决定：
//   1. 用函数内静态而不是命名空间级静态：QStandardPaths::writableLocation(AppDataLocation)
//      依赖 QCoreApplication 的 applicationName，命名空间级静态会在 QApplication 构造
//      之前就执行，拿到的路径是错的（甚至为空）。
//   2. new 出来故意不 delete：TranslationCache 的析构函数要碰 QSqlDatabase 的驱动管理器，
//      而进程退出时谁先析构是未定义的——让它在进程结束时随操作系统回收，最省心。
studious::cache::TranslationCache *sharedCache()
{
    static studious::cache::TranslationCache *instance = nullptr;
    if (instance == nullptr) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString cachePath = dir + QStringLiteral("/cache.db");
        instance = new studious::cache::TranslationCache(cachePath);
    }
    return instance;
}

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
    say(QStringLiteral("        --cache <on|off> (默认 on，走进程级 SQLite 翻译缓存)"));
    say(QStringLiteral("        --provider <google|azure|mymemory|auto> (默认 auto，空列表=后端默认顺序)"));
    say(QStringLiteral("        --llm-base <url> --llm-key <key> --llm-model <model>"));
    say(QStringLiteral("        （三项都不给时用 AppSettings::defaults() 的本机网关配置）"));
    say(QStringLiteral("  studious.exe --list-models [--llm-base <url> --llm-key <key>]"));
    say(QStringLiteral("                                     # 打印网关模型目录 + 思考等级，不开 GUI"));
}

// --smoke 冒烟：不建窗口，但走与 GUI 完全相同的路径
// （TranslateJob -> 工作线程 -> QtHttpClient + factory + TranslatorRouter）。
int runSmoke(const QStringList &args)
{
    QString text;
    QString to = QStringLiteral("zh");
    QString engine;
    QString provider = QStringLiteral("auto");
    QString llmBase;
    QString llmKey;
    QString llmModel;
    // 缓存开关默认与 GUI 一致（AppSettings::defaults() 里是 true）。
    bool cacheEnabled = AppSettings::defaults().cacheEnabled;

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
        } else if (arg == QLatin1String("--provider") && hasNext) {
            provider = args.at(++i);
        } else if (arg == QLatin1String("--cache") && hasNext) {
            const QString mode = args.at(++i).trimmed().toLower();
            if (mode == QLatin1String("on") || mode == QLatin1String("1")
                || mode == QLatin1String("true")) {
                cacheEnabled = true;
            } else if (mode == QLatin1String("off") || mode == QLatin1String("0")
                       || mode == QLatin1String("false")) {
                cacheEnabled = false;
            } else {
                say(QStringLiteral("[FAIL] --cache 只接受 on / off，收到：") + mode);
                return 1;
            }
        } else if (arg == QLatin1String("--llm-base") && hasNext) {
            llmBase = args.at(++i);
        } else if (arg == QLatin1String("--llm-key") && hasNext) {
            llmKey = args.at(++i);
        } else if (arg == QLatin1String("--llm-model") && hasNext) {
            llmModel = args.at(++i);
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

    TranslateJobRequest req;
    req.paragraphs = splitParagraphs(text);
    if (req.paragraphs.isEmpty()) {
        say(QStringLiteral("[FAIL] 切分后没有任何段落"));
        return 1;
    }
    req.srcLang = QStringLiteral("auto");
    req.dstLang = to;
    req.engineId = engine;

    // --provider auto（或空）= 空列表 = 后端默认顺序 google -> mymemory。
    req.webProviders = (provider.isEmpty() || provider == QLatin1String("auto"))
                           ? QStringList{}
                           : QStringList{provider};

    // LLM 三项未给出时回落到内置默认网关（保证 --engine llm 开箱可用）。
    req.llm = AppSettings::defaults().llm;
    if (!llmBase.isEmpty()) {
        req.llm.baseUrl = llmBase;
    }
    if (!llmKey.isEmpty()) {
        req.llm.apiKey = llmKey;
    }
    if (!llmModel.isEmpty()) {
        req.llm.model = llmModel;
    }

    // 缓存实例照给（关掉只是不用它，不影响任何行为），开关单独传。
    req.cache = sharedCache();
    req.cacheEnabled = cacheEnabled;

    say(QStringLiteral("=== Studious 前端冒烟 ==="));
    say(QStringLiteral("engine=%1  to=%2  src=auto  provider=%3  paragraphs=%4")
            .arg(engine.isEmpty() ? QStringLiteral("auto") : engine,
                 to,
                 req.webProviders.isEmpty() ? QStringLiteral("auto(google→mymemory)")
                                            : req.webProviders.join(QLatin1Char(',')))
            .arg(req.paragraphs.size()));
    say(QStringLiteral("llm=%1 @ %2  timeout=%3 ms  temperature=%4  think=%5")
            .arg(req.llm.model.isEmpty() ? QStringLiteral("<未配置>") : req.llm.model,
                 req.llm.baseUrl.isEmpty() ? QStringLiteral("<未配置>") : req.llm.baseUrl,
                 QString::number(req.llm.timeoutMs),
                 QString::number(req.llm.temperature),
                 req.llm.reasoningEffort.isEmpty() ? QStringLiteral("<默认>")
                                                   : req.llm.reasoningEffort));

    TranslateJob job;
    QEventLoop loop;
    int exitCode = 1;

    QObject::connect(&job,
                     &TranslateJob::finished,
                     &loop,
                     [&](const TranslateJobResult &result, qint64 elapsedMs) {
                         say(QStringLiteral("耗时: %1 ms").arg(elapsedMs));
                         // 缓存一行：开 = 报命中情况（命中数/总段数），关 = 明确写 off。
                         if (cacheEnabled) {
                             const int total = result.cacheHits + result.cacheMisses;
                             say(QStringLiteral("cache=on  命中=%1/%2")
                                     .arg(result.cacheHits)
                                     .arg(total));
                         } else {
                             say(QStringLiteral("cache=off"));
                         }
                         if (!result.outcome.ok) {
                             say(QStringLiteral("[FAIL] ") + result.outcome.error);
                             exitCode = 1;
                         } else {
                             const QStringList lines = result.outcome.byLang.value(to);
                             say(QStringLiteral("译文:"));
                             for (const QString &line : lines) {
                                 say(line);
                             }
                             exitCode = 0;
                         }
                         loop.quit();
                     });

    job.start(req);

    // 包一层超时，避免个别网络环境把冒烟挂死。LLM 可能比 web 更慢，给 120s。
    QTimer::singleShot(120000, &loop, [&]() {
        say(QStringLiteral("[FAIL] 冒烟超时（120s）"));
        exitCode = 1;
        loop.quit();
    });

    loop.exec();
    say(exitCode == 0 ? QStringLiteral("[OK] exit 0") : QStringLiteral("[OK] exit 1"));
    return exitCode;
}

// --list-models：命令行看清网关到底暴露了哪些模型、每个模型能选哪些思考等级。
// 不开 GUI：只起 QCoreApplication，QtHttpClient::send() 内部自带 QEventLoop。
int runListModels(const QStringList &args)
{
    QString llmBase;
    QString llmKey;
    QString llmModel;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const bool hasNext = (i + 1) < args.size();
        if (arg == QLatin1String("--llm-base") && hasNext) {
            llmBase = args.at(++i);
        } else if (arg == QLatin1String("--llm-key") && hasNext) {
            llmKey = args.at(++i);
        } else if (arg == QLatin1String("--llm-model") && hasNext) {
            llmModel = args.at(++i);
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    // 与 --smoke 同一约定：三项没给就用 AppSettings::defaults() 的本机网关配置。
    studious::config::LlmConfig cfg = AppSettings::defaults().llm;
    if (!llmBase.isEmpty()) {
        cfg.baseUrl = llmBase;
    }
    if (!llmKey.isEmpty()) {
        cfg.apiKey = llmKey;
    }
    if (!llmModel.isEmpty()) {
        cfg.model = llmModel;
    }

    say(QStringLiteral("=== 模型目录 ==="));
    say(QStringLiteral("baseUrl = %1").arg(cfg.baseUrl));

    studious::net::QtHttpClient http;   // 本线程构造；send() 里自带事件循环，无需 QtConcurrent
    const studious::llm::ModelListResult result = studious::llm::listModels(cfg, http);
    if (!result.ok) {
        say(QStringLiteral("[FAIL] ")
            + (result.error.isEmpty() ? QStringLiteral("获取模型失败（后端未给出原因）")
                                      : result.error));
        return 1;
    }

    say(QStringLiteral("共 %1 个模型").arg(result.models.size()));
    for (const studious::llm::LlmModelInfo &info : result.models) {
        const QString limits = (info.maxInputTokens > 0 || info.maxOutputTokens > 0)
                                   ? QStringLiteral("%1/%2")
                                         .arg(info.maxInputTokens)
                                         .arg(info.maxOutputTokens)
                                   : QStringLiteral("未告知");
        // id 补齐到 24 列，后面的「推理」「上限」才对得齐。
        const QString id = info.id.leftJustified(24, QLatin1Char(' '));
        say(QStringLiteral("  %1推理=%2  上限=%3")
                .arg(id,
                     info.supportsReasoning ? QStringLiteral("是") : QStringLiteral("否"),
                     limits));
    }

    // 思考等级演示：优先用 --llm-model 指定且真实存在的模型，否则用列表第一项。
    const studious::llm::LlmModelInfo *sample =
        result.models.isEmpty() ? nullptr : &result.models.first();
    for (const studious::llm::LlmModelInfo &info : result.models) {
        if (info.id.compare(cfg.model.trimmed(), Qt::CaseInsensitive) == 0) {
            sample = &info;
            break;
        }
    }
    if (sample != nullptr) {
        say(QStringLiteral("--- 思考等级（%1）---").arg(sample->id));
        const QVector<studious::llm::ReasoningOption> options =
            studious::llm::reasoningOptionsFor(*sample);
        for (const studious::llm::ReasoningOption &option : options) {
            // 空 value = 请求里不发送 reasoning_effort（跟随服务端默认），打出来才看得清。
            const QString value = option.value.isEmpty() ? QStringLiteral("<空>") : option.value;
            say(QStringLiteral("  [%1] %2").arg(value, option.label));
        }
    }

    say(QStringLiteral("[OK] exit 0"));
    return 0;
}

bool hasFlag(int argc, char *argv[], const char *flag)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    // 先看命令行：--list-models / --smoke 都只起 QCoreApplication，不碰 GUI 平台插件。
    if (hasFlag(argc, argv, "--list-models")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        return runListModels(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--smoke")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        return runSmoke(QCoreApplication::arguments());
    }

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("studious"));

    MainWindow window(sharedCache());
    window.show();
    return app.exec();
}
