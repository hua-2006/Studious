#include "AppBackend.h"
#include "AppSettings.h"
#include "AssetFormat.h"
#include "LocalEngine.h"
#include "TranslateJob.h"
#include "i18n/Strings.h"
#include "platform/IPlatform.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QObject>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QStringList>
#include <QStyleHints>
#include <QTimer>

#include <studious/asset/AssetRegistry.h>
#include <studious/asset/Downloader.h>
#include <studious/asset/Manifest.h>
#include <studious/cache/TranslationCache.h>
#include <studious/contract/LanguageRegistry.h>
#include <studious/contract/Types.h>
#include <studious/i18n/L10n.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>

#include <cstdio>
#include <cstring>

#include <atomic>

namespace {

// 装载一个字体文件，返回它注册的字体族名（失败返回空串）。
// 用族名而不是文件名，是因为 QML 的 font.family 认的是族名（如 "Source Han Sans SC"）。
QString loadFontFamily(const QString &path)
{
    const int id = QFontDatabase::addApplicationFont(path);
    if (id < 0) {
        std::fprintf(stderr, "[font] 装载失败: %s\n", path.toUtf8().constData());
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QString() : families.first();
}

// 装载界面字体 / 译文字体 / 等宽字体，返回 (ui, content, mono)。
struct FontSet {
    QString ui;
    QString content;
    QString mono;
};

FontSet loadBundledFonts()
{
    const QString root = QStringLiteral(STUDIOUS_FONT_DIR);
    const QString uiDir = root + QStringLiteral("/UI_OTF/SimplifiedChinese");
    const QString contentDir = root + QStringLiteral("/centent-v1.530");

    FontSet fonts;

    // 界面：思源黑体 SC。Regular + Medium + Bold 三个字重，
    // 让 QML 里的 Font.Normal / DemiBold / Bold 都能落到真字形上。
    fonts.ui = loadFontFamily(uiDir + QStringLiteral("/SourceHanSansSC-Regular.otf"));
    loadFontFamily(uiDir + QStringLiteral("/SourceHanSansSC-Medium.otf"));
    loadFontFamily(uiDir + QStringLiteral("/SourceHanSansSC-Bold.otf"));

    // 译文：霞鹜文楷。
    fonts.content = loadFontFamily(contentDir + QStringLiteral("/LXGWWenKai-Regular.ttf"));
    loadFontFamily(contentDir + QStringLiteral("/LXGWWenKai-Medium.ttf"));

    // 等宽：霞鹜文楷 Mono。
    fonts.mono = loadFontFamily(contentDir + QStringLiteral("/LXGWWenKaiMono-Regular.ttf"));

    if (fonts.ui.isEmpty()) {
        fonts.ui = QStringLiteral("Segoe UI Variable");
    }
    if (fonts.content.isEmpty()) {
        fonts.content = fonts.ui;
    }
    if (fonts.mono.isEmpty()) {
        fonts.mono = QStringLiteral("Cascadia Code");
    }

    std::printf("[font] ui=%s content=%s mono=%s\n",
                fonts.ui.toUtf8().constData(),
                fonts.content.toUtf8().constData(),
                fonts.mono.toUtf8().constData());
    return fonts;
}

// 注意：不要在这里关闭匿名 namespace —— 下面的缓存/工具函数还在同一个匿名 namespace 里，
// 它一直到文件末尾（main 之前）才闭合。

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

// 界面语言：配置里存了 zh / en 就显式覆盖后端语言，否则跟随系统。
//
// 调用时机很关键：必须在 QApplication / QCoreApplication **构造之后**
// （QSettings 需要一个已存在的 application 实例来定位持久化位置）、
// 且在创建任何窗口 / 解析任何本地化文案**之前**——i18n 覆盖是进程级的纯内存状态，
// 晚了就有一半文案是旧语言。
void applyUiLanguage(const QString &uiLanguage)
{
    using studious::i18n::Language;
    namespace ui18n = studious::ui::i18n;

    if (uiLanguage == QLatin1String("zh")) {
        studious::i18n::setLanguage(Language::Chinese);
        ui18n::setLanguage(ui18n::Lang::Chinese);
    } else if (uiLanguage == QLatin1String("en")) {
        studious::i18n::setLanguage(Language::English);
        ui18n::setLanguage(ui18n::Lang::English);
    } else {
        studious::i18n::resetToSystemLanguage();
        ui18n::setLanguage(ui18n::detectSystemLanguage());
    }
}

// 读配置 -> 应用界面语言。三条入口（GUI / --smoke / --assets-cli）都走这一份。
void applyUiLanguageFromSettings()
{
    applyUiLanguage(AppSettings::load().uiLanguage);
}

void printUsage()
{
    say(QStringLiteral("用法:"));
    say(QStringLiteral("  studious.exe                                       # 启动 GUI"));
    say(QStringLiteral("  studious.exe --smoke \"文本\" --to zh --engine web    # 无界面冒烟"));
    say(QStringLiteral("  可选: --to <lang> (默认 zh) / --engine <mock|web|llm|local-mt> (默认自动)"));
    say(QStringLiteral("        --local            # 用本机 llama.cpp（资产页下载的 runtime + GGUF）翻译"));
    say(QStringLiteral("        --local-model <文件名>  # 本地用哪个模型（不给 = 设置里的 / 第一个可用）"));
    say(QStringLiteral("        --local-gpu <on|off>   # 本地后端是否用 GPU（默认 on；没装 vulkan 运行时自动回落 CPU）"));
    say(QStringLiteral("        --switch-demo      # 同一进程内逐个换模型各翻一遍（验证热切换，建议配 --cache off）"));
    say(QStringLiteral("        --repeat <n>       # 同一请求连跑 n 次（验证本地引擎复用，配合 --cache off）"));
    say(QStringLiteral("        --cache <on|off> (默认 on，走进程级 SQLite 翻译缓存)"));
    say(QStringLiteral("        --provider <google|azure|mymemory|auto> (默认 auto，空列表=后端默认顺序)"));
    say(QStringLiteral("        --llm-base <url> --llm-key <key> --llm-model <model>"));
    say(QStringLiteral("        （三项都不给时用 AppSettings::defaults() 的本机网关配置）"));
    say(QStringLiteral("  studious.exe --list-models [--llm-base <url> --llm-key <key>]"));
    say(QStringLiteral("                                     # 打印网关模型目录 + 思考等级，不开 GUI"));
    say(QStringLiteral("  studious.exe --assets-cli <manifest.json>"));
    say(QStringLiteral("                                     # 打印资产清单（类型/大小/状态/占位），不开 GUI"));
    say(QStringLiteral("  studious.exe --asset-download <manifest.json> <assetId>"));
    say(QStringLiteral("                                     # 真实下载一个资产（含续传/校验/安装），不开 GUI"));
}

// --smoke 冒烟：不建窗口，但走与 GUI 完全相同的路径
// （TranslateJob -> 工作线程 -> QtHttpClient + factory + TranslatorRouter）。
int runSmoke(const QStringList &args)
{
    QString text;
    QString to = QStringLiteral("zh");
    QString from;   // 空 = auto；显式给出源语言能让小模型更可靠（避免照抄原文）
    QString engine;
    QString provider = QStringLiteral("auto");
    QString llmBase;
    QString llmKey;
    QString llmModel;
    // 本地引擎：只改"从哪拿译文"，其余流程与云端完全一致（同一个 TranslateJob / Router）。
    bool useLocalEngine = false;
    // 本地模型与后端：命令行显式给了就覆盖设置。
    // --local-gpu 默认 on（与界面的「GPU (Vulkan)」默认勾选一致）；没装 vulkan 运行时会自动回落 CPU。
    QString localModelFile;   // 空 = 设置里的 / 第一个可用模型
    bool localGpu = true;
    bool switchDemo = false;   // 同一进程内挨个换模型翻译：验证热切换
    int repeat = 1;
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
        } else if (arg == QLatin1String("--from") && hasNext) {
            from = args.at(++i);
        } else if (arg == QLatin1String("--engine") && hasNext) {
            engine = args.at(++i);
        } else if (arg == QLatin1String("--local")) {
            useLocalEngine = true;
        } else if (arg == QLatin1String("--local-model") && hasNext) {
            localModelFile = args.at(++i).trimmed();
        } else if (arg == QLatin1String("--local-gpu") && hasNext) {
            const QString mode = args.at(++i).trimmed().toLower();
            if (mode == QLatin1String("on") || mode == QLatin1String("1")
                || mode == QLatin1String("true")) {
                localGpu = true;
            } else if (mode == QLatin1String("off") || mode == QLatin1String("0")
                       || mode == QLatin1String("false")) {
                localGpu = false;
            } else {
                say(QStringLiteral("[FAIL] --local-gpu 只接受 on / off，收到：") + mode);
                return 1;
            }
        } else if (arg == QLatin1String("--switch-demo")) {
            switchDemo = true;
        } else if (arg == QLatin1String("--repeat") && hasNext) {
            bool ok = false;
            const int parsed = args.at(++i).trimmed().toInt(&ok);
            if (!ok || parsed <= 0) {
                say(QStringLiteral("[FAIL] --repeat 需要正整数，收到：") + args.at(i));
                return 1;
            }
            repeat = qBound(1, parsed, 20);   // 上限 20：冒烟是用来验行为的，不是压测
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

    // --switch-demo 演示的就是"本地引擎换模型"，没写 --local 也按本地理。
    if (switchDemo) {
        useLocalEngine = true;
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
    req.srcLang = from.isEmpty() ? QStringLiteral("auto") : from;
    req.dstLangs = QStringList{to};
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

    // 本地引擎：主线程拉起 llama-server（阻塞数百毫秒到数秒），再把 baseUrl 快照塞进 req.llm。
    // 顺序很关键——必须在 job.start() 之前完成，工作线程只认这份快照。
    LocalEngine localEngine;

    // --switch-demo：同一进程内挨个换模型各翻一遍（路径：第 1 轮模型 A -> 第 2 轮模型 B）。
    // 这是热切换的验收方式：每轮都打 pid / 启动耗时 / 后端，
    // 换模型那轮 lastStartupMs > 0 且 pid 变了，就是服务真的被 stop() + start() 了。
    QStringList demoRotation;
    if (useLocalEngine && switchDemo) {
        demoRotation = LocalEngine::availableModels();
        // 起点：--local-model > 设置 > 目录顺序第一个。
        const QString preferred =
            localModelFile.isEmpty() ? AppSettings::load().localModelFileName : localModelFile;
        if (!preferred.isEmpty() && demoRotation.contains(preferred)) {
            demoRotation.removeAll(preferred);
            demoRotation.prepend(preferred);
        }
        if (demoRotation.size() < 2) {
            say(QStringLiteral("[FAIL] --switch-demo 需要至少 2 个已安装模型（当前 %1 个）")
                    .arg(demoRotation.size()));
            say(QStringLiteral("[OK] exit 1"));
            return 1;
        }
        demoRotation = demoRotation.mid(0, 3);   // 最多 3 个：演示不是压测
    }

    // 普通模式用哪个模型：--local-model > 设置 > 空（空 = LocalEngine 自己取第一个可用）。
    // switchDemo 模式下由 demoRotation 逐轮决定，这里不用管。
    const QString localModelChoice = localModelFile.isEmpty() && !switchDemo
                                         ? AppSettings::load().localModelFileName
                                         : localModelFile;

    if (useLocalEngine) {
        // 本地模式覆盖云端三项：baseUrl 指向 127.0.0.1:<port>/v1，key 不需要
        // （llama-server 不校验），model 只是占位（服务端忽略这个字段）。
        // reasoningEffort 清空 = 请求里不再带 reasoning_effort / thinking。
        req.llm.apiKey.clear();
        req.llm.model = QStringLiteral("local");
        req.llm.reasoningEffort.clear();
        // 小模型专属收尾（与 GUI 的 applyLocalEngineConfig 保持一致）：
        // 删整行的括注 / 语言标签；禁用批量（0.5B 返回的是 JSON 对象，不是数组）。
        req.llm.stripModelNotes = true;
        req.llm.batchSize = 1;
        if (req.engineId != QLatin1String("llm")) {
            // 本地引擎只能配 llm 组件（其余组件早就 isReady()==false 了）。
            // 用户显式写了别的引擎就明说一句，别让 --engine mock --local 这种组合默默跑偏。
            if (!req.engineId.isEmpty()) {
                say(QStringLiteral("[local] 引擎强制为 llm（已忽略 --engine %1）")
                        .arg(req.engineId));
            }
            req.engineId = QStringLiteral("llm");
            engine = QStringLiteral("llm");
        }
    }

    // 缓存实例照给（关掉只是不用它，不影响任何行为），开关单独传。
    req.cache = sharedCache();
    req.cacheEnabled = cacheEnabled;

    say(QStringLiteral("=== Studious 前端冒烟 ==="));
    say(QStringLiteral("engine=%1  to=%2  src=%3  provider=%4  paragraphs=%5")
            .arg(engine.isEmpty() ? QStringLiteral("auto") : engine,
                 to,
                 req.srcLang,
                 req.webProviders.isEmpty() ? QStringLiteral("auto(google→mymemory)")
                                            : req.webProviders.join(QLatin1Char(',')))
            .arg(req.paragraphs.size()));
    // 本地模式下 baseUrl 要等 ensureStarted 才知道（端口是启动时挑的），别在这里写死"未配置"。
    const QString llmBaseShown =
        req.llm.baseUrl.isEmpty()
            ? (useLocalEngine ? QStringLiteral("<本地 llama-server，启动后分配端口>")
                              : QStringLiteral("<未配置>"))
            : req.llm.baseUrl;
    say(QStringLiteral("llm=%1 @ %2  timeout=%3 ms  temperature=%4  think=%5")
            .arg(req.llm.model.isEmpty() ? QStringLiteral("<未配置>") : req.llm.model,
                 llmBaseShown,
                 QString::number(req.llm.timeoutMs),
                 QString::number(req.llm.temperature),
                 req.llm.reasoningEffort.isEmpty() ? QStringLiteral("<默认>")
                                                   : req.llm.reasoningEffort));
    if (useLocalEngine) {
        say(QStringLiteral("local=%1  gpu=%2")
                .arg(localModelChoice.isEmpty() ? QStringLiteral("<第一个可用模型>")
                                                : localModelChoice,
                     localGpu ? QStringLiteral("on") : QStringLiteral("off")));
    }

    // 轮次：普通模式 = repeat 次同样的请求；--switch-demo = 每个模型一轮。
    const int totalRounds = switchDemo ? static_cast<int>(demoRotation.size()) : repeat;

    int exitCode = 1;
    for (int round = 1; round <= totalRounds; ++round) {
        const QString roundModel = switchDemo ? demoRotation.value(round - 1) : localModelChoice;

        if (switchDemo) {
            say(QStringLiteral("--- 第 %1/%2 轮：切到 %3 ---")
                    .arg(round)
                    .arg(totalRounds)
                    .arg(roundModel));
        } else if (repeat > 1) {
            say(QStringLiteral("--- 第 %1/%2 次 ---").arg(round).arg(repeat));
        }

        // 源语言与目标语言相同：Router 直接回填原文，用不到引擎。
        // 这时还去拉起 llama-server（1 秒启动 + 3 秒停止）纯属浪费，直接跳过。
        const QString srcNorm = studious::core::LanguageRegistry::normalize(req.srcLang);
        const QString dstNorm = studious::core::LanguageRegistry::normalize(to);
        const bool sameLanguage =
            !srcNorm.isEmpty() && srcNorm != QLatin1String("auto") && srcNorm == dstNorm;

        if (useLocalEngine && !sameLanguage) {
            LocalEngineConfig cfg;
            if (!roundModel.isEmpty()) {
                cfg.modelPath = QDir(LocalEngine::modelsDir()).filePath(roundModel);
            }
            cfg.preferGpu = localGpu;

            if (round == 1) {
                say(QStringLiteral("[local] 启动本地引擎…（GPU %1，模型 %2）")
                        .arg(localGpu ? QStringLiteral("on") : QStringLiteral("off"),
                             roundModel.isEmpty() ? QStringLiteral("<第一个可用>") : roundModel));
            }

            // 幂等 + 热切换都在 ensureStarted 内部：模型 / 后端变了就先停旧进程再起新的。
            QString startError;
            if (!localEngine.ensureStarted(cfg, &startError)) {
                say(QStringLiteral("[FAIL] ")
                    + (startError.isEmpty() ? QStringLiteral("本地引擎启动失败（未给出原因）")
                                            : startError));
                localEngine.shutdown();   // 双保险：失败路径也绝不留残留进程
                say(QStringLiteral("[OK] exit 1"));
                return 1;
            }

            const bool restarted = localEngine.lastStartupMs() > 0;
            say(QStringLiteral("[local] 后端=%1  模型=%2  pid=%3")
                    .arg(localEngine.currentBackend(),
                         localEngine.currentModel(),
                         QString::number(localEngine.processId())));
            if (restarted) {
                say(QStringLiteral("[local] 服务已重新拉起，耗时 %1 ms")
                        .arg(localEngine.lastStartupMs()));
            } else {
                say(QStringLiteral("[local] 复用已在运行的服务（未重新加载模型）"));
            }
            say(QStringLiteral("[local] baseUrl=%1（端口 %2）")
                    .arg(localEngine.baseUrl(), QString::number(localEngine.port())));

            // 每一轮都重新取快照：热切换后端口可能变，旧快照会指向已死的服务。
            req.llm.baseUrl = localEngine.baseUrl();
        }

        TranslateJob job;
        QEventLoop loop;
        int roundCode = 1;

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
                                 roundCode = 1;
                             } else {
                                 const QStringList lines = result.outcome.byLang.value(to);
                                 if (result.outcome.passthrough) {
    say(QStringLiteral("passthrough=true（同语言，未翻译）"));
}
say(QStringLiteral("译文:"));
                                 for (const QString &line : lines) {
                                     say(line);
                                 }
                                 if (!result.outcome.warning.isEmpty()) {
                                    say(QStringLiteral("[WARN] ") + result.outcome.warning);
                                }
                                roundCode = 0;
                             }
                             loop.quit();
                         });

        job.start(req);

        // 包一层超时，避免个别网络环境把冒烟挂死。LLM 可能比 web 更慢，给 120s。
        QTimer::singleShot(120000, &loop, [&]() {
            say(QStringLiteral("[FAIL] 冒烟超时（120s）"));
            roundCode = 1;
            loop.quit();
        });

        loop.exec();

        exitCode = roundCode;
        if (exitCode != 0) {
            break;   // 失败就停：后面的重复只会刷同样的错误
        }
    }

    if (useLocalEngine) {
        // 主动回收：--smoke 是一次性进程，跑完绝不能留一个 llama-server 在后台。
        QElapsedTimer stopTimer;
        stopTimer.start();
        localEngine.shutdown();
        say(QStringLiteral("[local] 已停止 llama-server（耗时 %1 ms）")
                .arg(stopTimer.elapsed()));
    }

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

// --assets-cli：纯命令行打印资产清单 + 每条安装状态，供手动验收（不开 GUI）。
//
// 解析选项与「资产」对话框完全一致：仓库里的 backend/asset/manifest.json 是示例清单，
// 未签名、sha256 是发布前才填的占位空串 —— 所以 requireSignature=false、allowMissingSha256=true。
int runAssetsCli(const QStringList &args)
{
    QString manifestPath;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const bool hasNext = (i + 1) < args.size();
        if (arg == QLatin1String("--assets-cli")) {
            if (hasNext && !args.at(i + 1).startsWith(QLatin1String("--"))) {
                manifestPath = args.at(++i);
            }
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    if (manifestPath.isEmpty()) {
        say(QStringLiteral("[FAIL] --assets-cli 需要一个 manifest.json 路径"));
        printUsage();
        return 1;
    }

    const QString absolutePath = QFileInfo(manifestPath).absoluteFilePath();

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        say(QStringLiteral("[FAIL] 打开清单失败：%1（%2）")
                .arg(file.errorString(), QDir::toNativeSeparators(absolutePath)));
        return 1;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    studious::asset::ManifestVerifyOptions opts;
    opts.requireSignature = false;    // 示例清单未签名
    opts.allowMissingSha256 = true;   // 示例清单的 sha256 全是待填占位
    opts.maxSchemaVersion = 1;

    const studious::asset::ManifestLoadResult parsed = studious::asset::parseManifest(bytes, opts);
    if (!parsed.ok) {
        // 后端 error 已本地化，原样打印。
        say(QStringLiteral("[FAIL] ") + parsed.error);
        return 1;
    }

    // 安装根目录与 GUI 完全一致（QStandardPaths::AppDataLocation + /assets）。
    const QString installRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("assets"));

    studious::asset::AssetRegistry registry(installRoot, parsed.manifest);
    registry.refresh();

    struct Row {
        QString id;
        QString type;
        QString size;
        QString state;
        QString placeholderNote;
    };

    QVector<Row> rows;
    int idWidth = 0;
    int typeWidth = 0;
    int sizeWidth = 0;
    int placeholders = 0;

    const QVector<studious::asset::AssetStatus> statuses = registry.listStatus();
    for (const studious::asset::AssetStatus &status : statuses) {
        const studious::asset::AssetEntry *entry = parsed.manifest.find(status.id);
        const bool isPlaceholder = entry != nullptr && entry->isPlaceholder();
        if (isPlaceholder) {
            ++placeholders;
        }

        Row row;
        row.id = status.id;
        row.type = entry != nullptr ? entry->type : QString();
        row.size = studious::frontend::humanBytes(entry != nullptr ? entry->size
                                                                   : status.installedBytes);
        row.state = studious::frontend::assetStateText(status.state);
        row.placeholderNote = isPlaceholder ? studious::i18n::s("占位", "placeholder") : QString();

        idWidth = qMax(idWidth, static_cast<int>(row.id.size()));
        typeWidth = qMax(typeWidth, static_cast<int>(row.type.size()));
        sizeWidth = qMax(sizeWidth, static_cast<int>(row.size.size()));
        rows << row;
    }

    say(studious::i18n::s("=== 资产清单 ===", "=== Asset manifest ==="));
    say(studious::i18n::s("清单: ", "Manifest: ") + QDir::toNativeSeparators(absolutePath));
    say(studious::i18n::sf("共 %1 条（占位 %2 条）",
                           "%1 entries (%2 placeholders)",
                           {QString::number(rows.size()), QString::number(placeholders)}));

    for (const Row &row : rows) {
        QString line = QStringLiteral("  %1  %2  %3  %4")
                           .arg(row.id.leftJustified(idWidth),
                                row.type.leftJustified(typeWidth),
                                row.size.leftJustified(sizeWidth),
                                row.state);
        if (!row.placeholderNote.isEmpty()) {
            line += QStringLiteral("  ") + row.placeholderNote;
        }
        say(line);
    }

    say(studious::i18n::sf("已安装总计: %1",
                           "Installed total: %1",
                           {studious::frontend::humanBytes(registry.totalInstalledBytes())}));
    return 0;
}

// --asset-download <manifest.json> <assetId>：无界面走一遍**真实**下载链路。
//
// 用途：验收「多源 → 续传 → sha256 校验 → 原子安装」，不需要开 GUI 手点。
// 与 GUI 用完全相同的清单解析选项与安装根目录，所以下载完直接去界面里就能看到「已安装」。
int runAssetDownload(const QStringList &args)
{
    QString manifestPath;
    QString assetId;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--asset-download")) {
            const bool hasFirst = (i + 1) < args.size() && !args.at(i + 1).startsWith(QLatin1String("--"));
            if (hasFirst) {
                manifestPath = args.at(++i);
            }
            const bool hasSecond = (i + 1) < args.size() && !args.at(i + 1).startsWith(QLatin1String("--"));
            if (hasSecond) {
                assetId = args.at(++i);
            }
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    if (manifestPath.isEmpty() || assetId.isEmpty()) {
        say(QStringLiteral("[FAIL] 用法：studious.exe --asset-download <manifest.json> <assetId>"));
        return 1;
    }

    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        say(QStringLiteral("[FAIL] 打开清单失败：%1").arg(file.errorString()));
        return 1;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    studious::asset::ManifestVerifyOptions opts;
    opts.requireSignature = false;
    opts.allowMissingSha256 = true;
    opts.maxSchemaVersion = 1;

    const studious::asset::ManifestLoadResult parsed = studious::asset::parseManifest(bytes, opts);
    if (!parsed.ok) {
        say(QStringLiteral("[FAIL] ") + parsed.error);
        return 1;
    }

    const QString installRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("assets"));

    studious::asset::AssetRegistry registry(installRoot, parsed.manifest);
    studious::net::QtHttpClient http;
    studious::asset::Downloader downloader(http, registry);

    say(QStringLiteral("=== 真实资产下载 ==="));
    say(QStringLiteral("清单     = ") + QDir::toNativeSeparators(QFileInfo(manifestPath).absoluteFilePath()));
    say(QStringLiteral("资产     = ") + assetId);
    say(QStringLiteral("安装根   = ") + QDir::toNativeSeparators(installRoot));
    say(QStringLiteral("当前状态 = ") + studious::frontend::assetStateText(registry.status(assetId).state));

    std::atomic_bool cancel{false};
    int lastPrintedPercent = -1;
    QElapsedTimer timer;
    timer.start();

    const studious::asset::DownloadResult result =
        downloader.download(assetId,
                            [&](const studious::asset::DownloadProgress &p) {
                                const int percent =
                                    p.total > 0 ? static_cast<int>(p.received * 100 / p.total) : -1;
                                // 每 10% 打一行，避免刷屏又把进度看清楚。
                                if (percent >= 0 && percent / 10 != lastPrintedPercent / 10) {
                                    lastPrintedPercent = percent;
                                    say(QStringLiteral("  %1%  %2 / %3  源 %4/%5")
                                            .arg(percent)
                                            .arg(studious::frontend::humanBytes(p.received),
                                                 studious::frontend::humanBytes(p.total))
                                            .arg(p.sourceIndex + 1)
                                            .arg(p.sourceCount));
                                }
                            },
                            &cancel);

    const qint64 elapsed = timer.elapsed();
    say(QString());
    if (!result.ok) {
        // 后端已本地化，直接显示。
        say(QStringLiteral("[FAIL] ") + result.error);
        return 1;
    }

    registry.refresh();
    const studious::asset::AssetStatus status = registry.status(assetId);
    say(QStringLiteral("[OK] 下载并安装成功：%1").arg(QDir::toNativeSeparators(result.installedPath)));
    say(QStringLiteral("     耗时 %1 ms，状态 = %2，占用 %3")
            .arg(elapsed)
            .arg(studious::frontend::assetStateText(status.state),
                 studious::frontend::humanBytes(status.installedBytes)));
    return 0;
}

bool hasFlag(int argc, char *argv[], const char *flag){
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

// 版本信息来自仓库根的 version.json（不写死在代码里）。
// 找不到或解析失败就用保守的兜底值，界面照样能起。
struct AppInfo {
    QString name = QStringLiteral("Studious");
    QString version = QStringLiteral("0.0.0");
    QString channel = QStringLiteral("stable");
    QString channelLabel = QStringLiteral("开发版");
};

AppInfo loadAppInfo()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("version.json")),
        QDir(appDir).filePath(QStringLiteral("../version.json")),
        QDir(appDir).filePath(QStringLiteral("../../version.json")),
    };

    for (const QString &path : candidates) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray bytes = file.readAll();
        file.close();

        const QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (!doc.isObject()) {
            std::fprintf(stderr, "[appinfo] %s 不是合法 JSON\n", path.toUtf8().constData());
            continue;
        }
        const QJsonObject obj = doc.object();

        AppInfo info;
        info.name = obj.value(QStringLiteral("name")).toString(info.name);
        info.version = obj.value(QStringLiteral("version")).toString(info.version);
        info.channel = obj.value(QStringLiteral("channel")).toString(info.channel);
        info.channelLabel = obj.value(QStringLiteral("channelLabel")).toString(info.channelLabel);

        std::printf("[appinfo] %s 版本 %s（%s）\n",
                    info.name.toUtf8().constData(),
                    info.version.toUtf8().constData(),
                    path.toUtf8().constData());
        return info;
    }

    std::fprintf(stderr, "[appinfo] 没找到 version.json，用兜底值\n");
    return AppInfo{};
}

} // namespace

int main(int argc, char *argv[])
{
    // 先看命令行：--list-models / --smoke / --assets-cli 都只起 QCoreApplication，
    // 不碰 GUI 平台插件（无桌面 / 无 DISPLAY 也能跑）。
    if (hasFlag(argc, argv, "--list-models")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        return runListModels(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--smoke")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        return runSmoke(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--assets-cli")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        return runAssetsCli(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--asset-download")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        return runAssetDownload(QCoreApplication::arguments());
    }

    // DPI 感知必须在 QGuiApplication 构造之前设置（Windows 上只生效一次）。
    studious::platform::IPlatform::initProcessDpi();

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("studious"));

    // 界面语言必须在加载 QML 之前落地：之后所有 i18n::s 才对得上配置。
    applyUiLanguageFromSettings();

    // 产品约定：界面中英双语可切，但**后端返回的错误文案固定英文**。
    if (studious::ui::i18n::backendMessagesInEnglish()) {
        studious::i18n::setLanguage(studious::i18n::Language::English);
    }

    // Controls 样式必须是 Basic：Windows 原生样式不认 background / contentItem，
    // 会静默忽略页面里所有定制（探针实测：把背景改成纯红也不生效）。
    // QQuickStyle::setStyle() 单独用在这台机器上没接管成功，环境变量必须更早设置，
    // 所以两个都上 —— 环境变量在 QGuiApplication 之前生效，setStyle 兜底。
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Basic 样式下 TextField / ComboBox 的底色取自 QPalette::Base，
    // 这里不给值的话会落到系统默认（本机是黑色），看起来就是「黑底白字」。
    // 这组值与 qml/Theme.qml 里的一一对应，改色两边一起改。
    {
        QPalette pal;
        pal.setColor(QPalette::Window, QColor(QStringLiteral("#F9F3FC")));
        pal.setColor(QPalette::WindowText, QColor(QStringLiteral("#2C273F")));
        pal.setColor(QPalette::Base, QColor(QStringLiteral("#F0EAF6")));
        pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#E9E2F1")));
        pal.setColor(QPalette::Text, QColor(QStringLiteral("#2C273F")));
        pal.setColor(QPalette::Button, QColor(QStringLiteral("#F0EAF6")));
        pal.setColor(QPalette::ButtonText, QColor(QStringLiteral("#2C273F")));
        pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#DAD5E9")));
        pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#4C3B79")));
        pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#E4DEEB")));
        pal.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#2C273F")));
        pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#736F87")));
        app.setPalette(pal);
    }

    QQmlApplicationEngine engine;
    auto *backend = new studious::ui::AppBackend(sharedCache(), &app);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), backend);

    // 字体：必须在 QML 加载前注册好，否则首帧会用系统默认字体渲染再跳变。
    const FontSet fonts = loadBundledFonts();
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontUi"), fonts.ui);
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontContent"), fonts.content);
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontMono"), fonts.mono);

    // 关于页的信息：名称/版本/渠道读仓库根的 version.json；提交号构建期取；Qt 版本与架构运行期取。
    const AppInfo appInfo = loadAppInfo();
    engine.rootContext()->setContextProperty(QStringLiteral("appName"), appInfo.name);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), appInfo.version);
    engine.rootContext()->setContextProperty(QStringLiteral("appChannel"), appInfo.channel);
    engine.rootContext()->setContextProperty(QStringLiteral("appChannelLabel"),
                                             appInfo.channelLabel);
    engine.rootContext()->setContextProperty(QStringLiteral("appCommit"),
                                             QStringLiteral(STUDIOUS_COMMIT));
    engine.rootContext()->setContextProperty(QStringLiteral("qtVersion"),
                                             QString::fromLatin1(qVersion()));
    engine.rootContext()->setContextProperty(
        QStringLiteral("buildArch"),
#if defined(Q_PROCESSOR_ARM_64) || defined(__aarch64__)
        QStringLiteral("arm64")
#else
        QStringLiteral("x64")
#endif
    );
    // QML 侧拿不稳 QStyleHints，这里注入一次「系统是否深色」给 Theme 单例用。
    engine.rootContext()->setContextProperty(
        QStringLiteral("systemPrefersDark"),
        QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    engine.loadFromModule(QStringLiteral("Studious"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "[fatal] QML 加载失败：检查 Studious 模块是否注册成功\n");
        return 1;
    }

    // ---- 开发用截图：studious.exe --ui-shot out.png [--ui-page N] [--ui-size WxH] ----
    // 拍完就退出，不进事件循环。给 UI 走查 / 与设计稿比对用。
    if (hasFlag(argc, argv, "--ui-shot")) {
        const QStringList all = QCoreApplication::arguments();
        const auto valueAfter = [&all](const char *flag) {
            const int index = all.indexOf(QString::fromLatin1(flag));
            return (index >= 0 && index + 1 < all.size()) ? all.at(index + 1) : QString();
        };

        const QString outPath = valueAfter("--ui-shot");
        const int pageIndex = valueAfter("--ui-page").toInt();

        int width = 1440;
        int height = 900;
        const QStringList sizeParts = valueAfter("--ui-size").split(QLatin1Char('x'));
        if (sizeParts.size() == 2) {
            width = sizeParts.at(0).toInt();
            height = sizeParts.at(1).toInt();
        }

        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        if (window == nullptr) {
            std::fprintf(stderr, "[ui-shot] 根对象不是 QQuickWindow\n");
            return 1;
        }

        window->resize(width, height);
        window->setProperty("currentPage", pageIndex);
        window->show();

        // 不能只 processEvents：那样不推进动画驱动，带 `Behavior on color` 的控件
        // 会永远停在动画第一帧（截图里就是浅色）。真跑一段事件循环等动画收敛。
        QEventLoop loop;
        QTimer::singleShot(700, &loop, &QEventLoop::quit);
        loop.exec();

        const QImage shot = window->grabWindow();
        const bool saved = !shot.isNull() && shot.save(outPath);
        std::printf("[ui-shot] %s -> %s (%dx%d)\n",
                    saved ? "saved" : "FAILED",
                    outPath.toUtf8().constData(),
                    shot.width(),
                    shot.height());
        return saved ? 0 : 1;
    }

    return app.exec();
}
