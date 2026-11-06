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
#include <QIcon>
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
#include <QTranslator>

#include <studious/asset/AssetRegistry.h>
#include <studious/asset/Downloader.h>
#include <studious/asset/Manifest.h>
#include <studious/cache/TranslationCache.h>
#include <studious/contract/LanguageRegistry.h>
#include <studious/contract/Types.h>
#include <studious/i18n/L10n.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>
#include <studious/translators/LocalMtTranslator.h>

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
// QML 里 qsTr() 的翻译器。
//
// 源码原文就是中文，所以中文不需要翻译器（源字符串直接显示）；
// 只有 en 需要真正的 .qm。每次调用先卸掉上一个，避免重复安装叠加。
void installUiTranslator(const QString &code)
{
    static QTranslator *installed = nullptr;

    if (installed != nullptr) {
        QCoreApplication::removeTranslator(installed);
        delete installed;
        installed = nullptr;
    }

    auto *translator = new QTranslator(QCoreApplication::instance());
    if (code == QLatin1String("en")
        && translator->load(QStringLiteral(":/i18n/studious_en.qm"))) {
        QCoreApplication::installTranslator(translator);
        installed = translator;
    } else {
        delete translator;
    }
}

void applyUiLanguage(const QString &uiLanguage)
{
    using studious::i18n::Language;
    namespace ui18n = studious::ui::i18n;

    QString resolved;

    if (uiLanguage == QLatin1String("zh")) {
        studious::i18n::setLanguage(Language::Chinese);
        ui18n::setLanguage(ui18n::Lang::Chinese);
        resolved = QStringLiteral("zh");
    } else if (uiLanguage == QLatin1String("en")) {
        studious::i18n::setLanguage(Language::English);
        ui18n::setLanguage(ui18n::Lang::English);
        resolved = QStringLiteral("en");
    } else {
        studious::i18n::resetToSystemLanguage();
        const ui18n::Lang system = ui18n::detectSystemLanguage();
        ui18n::setLanguage(system);
        resolved = system == ui18n::Lang::Chinese ? QStringLiteral("zh")
                                                  : QStringLiteral("en");
    }

    installUiTranslator(resolved);
}

// 读配置 -> 应用界面语言。三条入口（GUI / --smoke / --assets-cli）都走这一份。
void applyUiLanguageFromSettings()
{
    applyUiLanguage(AppSettings::load().uiLanguage);
}

// ---------------------------------------------------------------- 引擎 id 归一

// 引擎 id 有两套体系，这里做一次桥接：
//   后端（Factory / Router）只认 mock / web / llm / local-mt；
//   界面与注册表（engine\id、engine\priority）用 web / llm-cloud / llm-local / local-mt。
// 以前 CLI 只认后端那套：--engine llm-cloud 直接报"不存在"；不带 --engine 又落到
// 硬编码的 mock 上，输出【zh】原文还 exit 0（假成功）。
struct EngineChoice {
    QString backendId;        // 空 = 解析失败，看 error
    QString uiId;             // 命中的界面 id（auto 时用来在 header 里说清"为什么是它"）
    bool forceLocal = false;  // llm-local：等价于 --local（拉起本机 llama-server）
    bool autoPicked = false;  // true = 由 auto / 未给 --engine 按设置的优先级推导
    QString error;            // 非空 = [FAIL] 的正文
};

// 单个界面 id -> 后端 id 的纯映射，不判断可用性。认不出返回 false。
bool mapUiEngineId(const QString &uiId, EngineChoice *out)
{
    const QString id = uiId.trimmed().toLower();
    if (id == QLatin1String("web")) {
        out->backendId = QStringLiteral("web");
    } else if (id == QLatin1String("llm") || id == QLatin1String("llm-cloud")) {
        out->backendId = QStringLiteral("llm");
    } else if (id == QLatin1String("llm-local")) {
        out->backendId = QStringLiteral("llm");
        out->forceLocal = true;
    } else if (id == QLatin1String("local-mt")) {
        out->backendId = QStringLiteral("local-mt");
    } else if (id == QLatin1String("mock")) {
        out->backendId = QStringLiteral("mock");
    } else {
        return false;
    }
    out->uiId = id;
    return true;
}

// 「当前可解析」= 这台机器上真的能用，不是"理论上支持"：
//   web       永远可用（联不联网由请求本身报错）
//   llm-cloud 地址 + 模型齐了就算（key 对不对只有真请求知道）
//   llm-local 运行时 + 至少一个模型装好了
//   local-mt  至少装了一个语言对；给了明确源语言时还要求这个语言对真能走
bool engineAvailable(const QString &uiId, const AppSettings &settings,
                     const QString &srcLang, const QString &dstLang)
{
    if (uiId == QLatin1String("web")) {
        return true;
    }
    if (uiId == QLatin1String("llm") || uiId == QLatin1String("llm-cloud")) {
        return settings.llm.isValid();
    }
    if (uiId == QLatin1String("llm-local")) {
        return !LocalEngine::availableModels().isEmpty()
               && (LocalEngine::isGpuAvailable() || LocalEngine::isCpuAvailable());
    }
    if (uiId == QLatin1String("local-mt")) {
        const studious::translate::LocalMtTranslator mt(LocalEngine::modelsDir());
        if (mt.installedPairs().isEmpty()) {
            return false;
        }
        const QString src = studious::core::LanguageRegistry::normalize(srcLang);
        const QString dst = studious::core::LanguageRegistry::normalize(dstLang);
        // 源语言是"自动检测"时 Router 必然拒绝（本地 MT 没法先探测再选模型），
        // 但引擎本身是装好的 —— 和界面一样按"装好没有"算可用。
        if (src.isEmpty() || src == QLatin1String("auto")) {
            return true;
        }
        return mt.resolve(src, dst).ready;
    }
    return false;
}

// raw = --engine 的原样值；auto / 空串 = 按 settings.enginePriority 挑第一个可用的界面 id。
EngineChoice resolveEngineId(const QString &raw, const AppSettings &settings,
                             const QString &srcLang, const QString &dstLang)
{
    const QString value = raw.trimmed().toLower();

    if (value.isEmpty() || value == QLatin1String("auto")) {
        // 设置里没存优先级时，用与 TranslateJob 兜底一致的本地优先顺序（注意没有 mock）。
        const QStringList order = settings.enginePriority.isEmpty()
                                      ? QStringList{QStringLiteral("local-mt"),
                                                    QStringLiteral("llm-cloud"),
                                                    QStringLiteral("web")}
                                      : settings.enginePriority;

        QStringList unusable;
        for (const QString &rawId : order) {
            const QString uiId = rawId.trimmed().toLower();
            if (uiId.isEmpty() || uiId == QLatin1String("mock")) {
                continue;   // mock 只能显式要，自动链里永不出现
            }
            EngineChoice mapped;
            if (!mapUiEngineId(uiId, &mapped)) {
                continue;   // 注册表里手改出来的野 id：忽略，别让整条链失效
            }
            if (!engineAvailable(mapped.uiId, settings, srcLang, dstLang)) {
                unusable << uiId;
                continue;
            }
            mapped.autoPicked = true;
            return mapped;
        }

        EngineChoice failed;
        failed.error =
            QStringLiteral("自动选择引擎失败：优先级（%1）里没有当前可用的引擎%2；"
                           "用 --engine 指定（web / llm-cloud / llm-local / local-mt / mock）")
                .arg(order.join(QStringLiteral("、")),
                     unusable.isEmpty()
                         ? QString()
                         : QStringLiteral("（已跳过：%1）").arg(unusable.join(QStringLiteral("、"))));
        return failed;
    }

    EngineChoice mapped;
    if (!mapUiEngineId(value, &mapped)) {
        mapped.error =
            QStringLiteral("未知引擎 \"%1\"；可用：web、llm-cloud、llm-local、local-mt、mock")
                .arg(raw.trimmed());
    }
    return mapped;
}

// 取 `--xxx` 后面的那个值。
// 两种"缺值"都算失败：后面什么都没有，或者紧跟的是另一个 `--xxx`。
// 以前写成 `arg == "--to" && hasNext` 的话，末尾悬空会被静默跳过、回落到默认值，
// 用户看到的现象就是"参数没生效"。
bool takeFlagValue(const QStringList &args, int *index, QString *value)
{
    const int next = *index + 1;
    if (next >= static_cast<int>(args.size())) {
        return false;
    }
    const QString candidate = args.at(next);
    if (candidate.startsWith(QLatin1String("--"))) {
        return false;
    }
    *index = next;
    *value = candidate;
    return true;
}

void printUsage()
{
    say(QStringLiteral("用法:"));
    say(QStringLiteral("  studious.exe                                       # 启动 GUI"));
    say(QStringLiteral("  studious.exe --smoke \"文本\" --to zh --engine web    # 无界面冒烟"));
    say(QStringLiteral("  可选: --to <lang> (默认取设置里的目标语言)"));
    say(QStringLiteral("        --engine <auto|web|llm-cloud|llm-local|local-mt|mock> (默认 auto)"));
    say(QStringLiteral("                 # auto = 按设置里的引擎优先级挑第一个可用的；mock 是假翻译，退出码 2"));
    say(QStringLiteral("        --local            # 用本机 llama.cpp（资产页下载的 runtime + GGUF）翻译"));
    say(QStringLiteral("        --local-model <文件名>  # 本地用哪个模型（不给 = 设置里的 / 第一个可用）"));
    say(QStringLiteral("        --local-gpu <on|off>   # 本地后端是否用 GPU（默认跟随设置的「GPU (Vulkan)」；没装 vulkan 运行时自动回落 CPU）"));
    say(QStringLiteral("        --switch-demo      # 同一进程内逐个换模型各翻一遍（验证热切换，建议配 --cache off）"));
    say(QStringLiteral("        --repeat <n>       # 同一请求连跑 n 次（验证本地引擎复用，配合 --cache off）"));
    say(QStringLiteral("        --cache <on|off> (默认取设置里的「启用缓存」，走进程级 SQLite 翻译缓存)"));
    say(QStringLiteral("        --provider <google|azure|mymemory|auto> (默认 auto，空列表=后端默认顺序)"));
    say(QStringLiteral("        --llm-base <url> --llm-key <key> --llm-model <model>"));
    say(QStringLiteral("        （三项都不给时用设置里的网关配置，缺项回落内置默认值）"));
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
    // 先读用户真实设置：缓存开关 / 目标语言 / LLM 连接参数 / 引擎优先级都从这里来。
    // 以前这里用 AppSettings::defaults()，于是注册表里 cache\enabled=false、llm\timeoutMs=69000、
    // engine\id=local-mt 全被无视，--smoke 打出来的是 cache=on / timeout=60000 / mock。
    // 显式 flag 仍然优先：下面解析到的值会覆盖这里（见 --cache / --to / --llm-* / --local）。
    const AppSettings settings = AppSettings::load();

    QString text;
    QString to = settings.dstLang.isEmpty() ? QStringLiteral("zh") : settings.dstLang;
    QString from;   // 空 = 用 settings.srcLang（默认 auto）；显式给出源语言能让小模型更可靠
    QString engine;   // --engine 的原样值（可能是界面 id / auto / 空）
    QString provider = QStringLiteral("auto");
    QString llmBase;
    QString llmKey;
    QString llmModel;
    // 本地引擎：只改"从哪拿译文"，其余流程与云端完全一致（同一个 TranslateJob / Router）。
    bool useLocalEngine = false;
    // 本地模型与后端：命令行显式给了就覆盖设置。
    // --local-gpu 没给时跟随设置里的「GPU (Vulkan)」；没装 vulkan 运行时仍会自动回落 CPU。
    QString localModelFile;   // 空 = 设置里的 / 第一个可用模型
    bool localGpu = settings.localUseGpu;
    bool switchDemo = false;   // 同一进程内挨个换模型翻译：验证热切换
    int repeat = 1;
    // 缓存开关默认取设置（--cache on|off 给了就覆盖）。
    bool cacheEnabled = settings.cacheEnabled;

    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const bool hasNext = (i + 1) < args.size();
        if (arg == QLatin1String("--smoke")) {
            // --smoke 后面紧跟的第一个非 "--" 开头的参数就是待翻译文本。
            if (hasNext && !args.at(i + 1).startsWith(QLatin1String("--"))) {
                text = args.at(++i);
            }
        } else if (arg == QLatin1String("--to") || arg == QLatin1String("--from")
                   || arg == QLatin1String("--engine")
                   || arg == QLatin1String("--local-model")) {
            // 这四个以前是 `&& hasNext`：末尾悬空会被静默跳过、回落默认值，看着像"设置没生效"。
            // 现在缺值直接报错退出，并说清缺的是什么。
            QString value;
            const QString what = (arg == QLatin1String("--engine"))
                                     ? QStringLiteral("引擎名")
                                     : (arg == QLatin1String("--local-model"))
                                           ? QStringLiteral("模型文件名")
                                           : QStringLiteral("语言值");
            if (!takeFlagValue(args, &i, &value)
                || (what == QStringLiteral("语言值") && value.trimmed().isEmpty())) {
                say(QStringLiteral("[FAIL] %1 缺少%2").arg(arg, what));
                printUsage();
                return 1;
            }
            if (arg == QLatin1String("--to")) {
                to = value.trimmed();
            } else if (arg == QLatin1String("--from")) {
                from = value.trimmed();
            } else if (arg == QLatin1String("--engine")) {
                engine = value.trimmed();
            } else {
                localModelFile = value.trimmed();
            }
        } else if (arg == QLatin1String("--local")) {
            useLocalEngine = true;
        } else if (arg == QLatin1String("--local-gpu")) {
            QString mode;
            if (!takeFlagValue(args, &i, &mode)) {
                say(QStringLiteral("[FAIL] --local-gpu 缺少值（on / off）"));
                printUsage();
                return 1;
            }
            mode = mode.trimmed().toLower();
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
        } else if (arg == QLatin1String("--repeat")) {
            QString value;
            if (!takeFlagValue(args, &i, &value)) {
                say(QStringLiteral("[FAIL] --repeat 缺少次数值（正整数）"));
                printUsage();
                return 1;
            }
            bool ok = false;
            const int parsed = value.trimmed().toInt(&ok);
            if (!ok || parsed <= 0) {
                say(QStringLiteral("[FAIL] --repeat 需要正整数，收到：") + value);
                return 1;
            }
            repeat = qBound(1, parsed, 20);   // 上限 20：冒烟是用来验行为的，不是压测
        } else if (arg == QLatin1String("--provider")) {
            if (!takeFlagValue(args, &i, &provider)) {
                say(QStringLiteral("[FAIL] --provider 缺少值（google|azure|mymemory|auto）"));
                printUsage();
                return 1;
            }
        } else if (arg == QLatin1String("--cache")) {
            QString mode;
            if (!takeFlagValue(args, &i, &mode)) {
                say(QStringLiteral("[FAIL] --cache 缺少值（on / off）"));
                printUsage();
                return 1;
            }
            mode = mode.trimmed().toLower();
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
        } else if (arg == QLatin1String("--llm-base")) {
            if (!takeFlagValue(args, &i, &llmBase)) {
                say(QStringLiteral("[FAIL] --llm-base 缺少 URL"));
                printUsage();
                return 1;
            }
        } else if (arg == QLatin1String("--llm-key")) {
            if (!takeFlagValue(args, &i, &llmKey)) {
                say(QStringLiteral("[FAIL] --llm-key 缺少 Key"));
                printUsage();
                return 1;
            }
        } else if (arg == QLatin1String("--llm-model")) {
            if (!takeFlagValue(args, &i, &llmModel)) {
                say(QStringLiteral("[FAIL] --llm-model 缺少模型名"));
                printUsage();
                return 1;
            }
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    // --switch-demo 演示的就是"本地引擎换模型"，没写 --local 也按本地理。
    if (switchDemo) {
        useLocalEngine = true;
    }
    // --local / --switch-demo 只可能配 llm 组件（本机 llama-server），auto 在这里没有意义：
    // 直接定成 llm，省得 auto 先挑出 web 再被下面的本地覆盖块改掉（header 会前后矛盾）。
    if (useLocalEngine
        && (engine.trimmed().isEmpty()
            || engine.trimmed().compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0)) {
        engine = QStringLiteral("llm");
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
    // 源语言：没给 --from 就用设置里的（默认 auto）。
    const QString srcLang = from.isEmpty()
                                ? (settings.srcLang.isEmpty() ? QStringLiteral("auto")
                                                              : settings.srcLang)
                                : from;

    // 引擎 id 归一：界面 id（llm-cloud / llm-local）和 auto 都在这里落到后端 id 上。
    // 解析失败（未知 id、auto 一个都挑不出来）直接 [FAIL] —— 以前这种情况会被 Router 的
    // 兜底链悄悄换成 mock，输出【zh】原文、还 exit 0。
    const EngineChoice engineChoice = resolveEngineId(engine, settings, srcLang, to);
    if (engineChoice.backendId.isEmpty()) {
        say(QStringLiteral("[FAIL] ") + engineChoice.error);
        printUsage();
        return 1;
    }
    // llm-local 等价于 --local：拉起本机 llama-server，再把 baseUrl 塞进 req.llm。
    if (engineChoice.forceLocal) {
        useLocalEngine = true;
    }
    // mock 是否真的在跑，等 --local 覆盖之后再判定（见下面的 [WARN]）：--local --engine mock
    // 会被强制改成 llm 真跑本地 llama-server，那时不该再打 mock 的警告。

    req.srcLang = srcLang;
    req.dstLangs = QStringList{to};
    // auto 模式**不**写死 preferredEngine：让 Router 顺着优先级链自己挑、并且运行时
    // 失败还能换下一个（写死等于关掉降级）。显式 --engine 才钉死单个引擎。
    // 注意 --local / llm-local 仍然要钉 llm：那是用户对"用本机 llama-server"的明确要求。
    if (!engineChoice.autoPicked || engineChoice.forceLocal) {
        req.engineId = engineChoice.backendId;
        engine = engineChoice.backendId;
    } else {
        // auto 还要把「设置里的界面优先级」翻成 Router 认的 id，否则 TranslateJob
        // 会用它自己的兜底链，跟界面上拖出来的顺序不一致。
        QStringList uiOrder = settings.enginePriority;
        if (uiOrder.isEmpty()) {
            uiOrder = QStringList{QStringLiteral("llm-local"), QStringLiteral("local-mt"),
                                  QStringLiteral("llm-cloud"), QStringLiteral("web")};
        }
        QStringList chain;
        for (const QString &uiId : uiOrder) {
            EngineChoice mapped;
            if (!mapUiEngineId(uiId, &mapped)) {
                continue;
            }
            // mock 不在生产链里；llm-local 与 llm-cloud 都落到后端的 "llm"，
            // 去重后只留一个（本地优先时先命中它，靠 --local 覆盖才真正走本地）。
            if (mapped.backendId == QLatin1String("mock")
                || chain.contains(mapped.backendId)) {
                continue;
            }
            chain << mapped.backendId;
        }
        req.priority = chain;
    }

    // --provider auto（或空）= 空列表 = 后端默认顺序 google -> mymemory。
    req.webProviders = (provider.isEmpty() || provider == QLatin1String("auto"))
                           ? QStringList{}
                           : QStringList{provider};

    // LLM 连接参数取自用户设置（地址 / key / 模型 / 超时 / 批量），--llm-* 显式给了就覆盖。
    req.llm = settings.llm;
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
            localModelFile.isEmpty() ? settings.localModelFileName : localModelFile;
        if (!preferred.isEmpty() && demoRotation.contains(preferred)) {
            demoRotation.removeAll(preferred);
            demoRotation.prepend(preferred);
        }
        if (demoRotation.size() < 2) {
            say(QStringLiteral("[FAIL] --switch-demo 需要至少 2 个已安装模型（当前 %1 个）")
                    .arg(demoRotation.size()));
            say(QStringLiteral("[DONE] exit 1"));
            return 1;
        }
        demoRotation = demoRotation.mid(0, 3);   // 最多 3 个：演示不是压测
    }

    // 普通模式用哪个模型：--local-model > 设置 > 空（空 = LocalEngine 自己取第一个可用）。
    // switchDemo 模式下由 demoRotation 逐轮决定，这里不用管。
    const QString localModelChoice = localModelFile.isEmpty() && !switchDemo
                                         ? settings.localModelFileName
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

    // mock 只在显式要的时候才会留到这里（候选链与 auto 都已剔除它），但必须让人一眼看出
    // 输出是假的。它在 --local 覆盖之后判定：--local --engine mock 会被强制改成 llm 真跑
    // 本地 llama-server，那就不该再报 mock。
    const bool mockActive = req.engineId == QLatin1String("mock");
    if (mockActive) {
        say(QStringLiteral("[WARN] 当前为 mock 引擎，输出是【语言码】原文的假翻译"));
    }

    say(QStringLiteral("=== Studious 前端冒烟 ==="));
    // auto 时把"按设置挑中的界面 id"一并打出来，否则只看到 engine=local-mt 会以为设置没读。
    const QString engineShown =
        engineChoice.autoPicked
            ? QStringLiteral("%1 (auto: %2)").arg(engine, engineChoice.uiId)
            : (engine.isEmpty() ? QStringLiteral("auto") : engine);
    say(QStringLiteral("engine=%1  to=%2  src=%3  provider=%4  paragraphs=%5")
            .arg(engineShown,
                 to,
                 req.srcLang,
                 req.webProviders.isEmpty() ? QStringLiteral("auto(google→mymemory)")
                                            : req.webProviders.join(QLatin1Char(',')))
            .arg(req.paragraphs.size()));
    if (useLocalEngine) {
        // 本地模式的真实 baseUrl 要等 ensureStarted 才知道（端口是启动时挑的）。
        // 以前这里打的是 req.llm.baseUrl —— 那时还是网关 3065（覆盖发生在启动之后），
        // 于是 header 显示 3065、真正请求打 18080，看着像"本地引擎没生效"。
        // 现在 header 只说"本地 llama-server"，真实地址交给启动后的 [local] baseUrl=... 那行。
        say(QStringLiteral("llm=local (llama-server)  timeout=%1 ms  temperature=%2")
                .arg(QString::number(req.llm.timeoutMs), QString::number(req.llm.temperature)));
    } else {
        say(QStringLiteral("llm=%1 @ %2  timeout=%3 ms  temperature=%4  think=%5")
                .arg(req.llm.model.isEmpty() ? QStringLiteral("<未配置>") : req.llm.model,
                     req.llm.baseUrl.isEmpty() ? QStringLiteral("<未配置>") : req.llm.baseUrl,
                     QString::number(req.llm.timeoutMs),
                     QString::number(req.llm.temperature),
                     req.llm.reasoningEffort.isEmpty() ? QStringLiteral("<默认>")
                                                       : req.llm.reasoningEffort));
    }
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
                say(QStringLiteral("[DONE] exit 1"));
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
            // model 段进缓存键的模型指纹 —— 写成固定的 "local" 会让 0.5B 与 1.5B
            // 共用一条缓存，换模型后直接返回上一个模型的译文。这里用真的 gguf 文件名。
            if (!localEngine.currentModel().isEmpty()) {
                req.llm.model = localEngine.currentModel();
            }
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

    // mock 输出的是【语言码】原文的假译文：流程再顺畅也不算成功。
    // 退出码 2 = "跑完了但是假的"，跟 1（真失败）区分开，脚本里能一眼分辨。
    if (exitCode == 0 && mockActive) {
        exitCode = 2;
    }
    say(QStringLiteral("[DONE] exit %1").arg(exitCode));
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

    // 与 --smoke 同一约定：三项没给就用设置里的网关配置（--llm-* 显式给了才覆盖）。
    // 以前这里是 defaults()，于是用户把网关改到别的端口后，--list-models 还在问 3065。
    studious::config::LlmConfig cfg = AppSettings::load().llm;
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

    say(QStringLiteral("[DONE] exit 0"));
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

    // 任务栏 / Alt-Tab / 窗口图标。exe 本身的图标由 resources/app.rc 编进 PE，
    // 这里再设一次是为了让运行期窗口也用同一张图（托盘以后接平台层时同样取它）。
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Studious/resources/brand.png")));

    // 界面语言必须在加载 QML 之前落地：之后所有 i18n::s 才对得上配置。
    // 后端文案跟着一起切（applyUiLanguage 里已同步 studious::i18n）——
    // 以前这里强行把后端固定成英文，结果中文界面里引擎状态那一行会冒出
    // "The engine state contradicts its resolve() result: …" 这种英文长句。
    applyUiLanguageFromSettings();

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

    // --ui-shot 是自动化工具：允许和已开着的界面并存，也不应该去 ping 已有实例抢焦点。
    // 所以它必须绕过下面的单实例判定 —— 以前它写判定之后，只要已经有一个 studious 在跑，
    // 截图命令就会静默 exit 0 却什么都不产出。
    const bool uiShotMode = hasFlag(argc, argv, "--ui-shot");

    // 第二个实例：构造函数里已经抢过单实例名并 ping 了在跑的那个进程，
    // 这里直接退出 —— 别加载 QML、别建窗口、别动资产目录。
    if (!uiShotMode && backend->duplicateInstance()) {
        return 0;
    }

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

    // 托盘双击 / 菜单「显示主窗口」/ 第二条实例 ping 过来 → 把窗口叫回前台。
    // 折叠成"先 show 再 raise 再 activate"三步：只 raise 一个隐藏窗口是没用的。
    if (auto *rootWindow = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst())) {
        QObject::connect(backend,
                         &studious::ui::AppBackend::restoreWindowRequested,
                         rootWindow,
                         [rootWindow] {
                             if (!rootWindow->isVisible()) {
                                 rootWindow->show();
                             }
                             if (rootWindow->windowState() == Qt::WindowMinimized) {
                                 rootWindow->showNormal();
                             }
                             rootWindow->raise();
                             rootWindow->requestActivate();
                         });
    }

    // ---- 开发用截图：studious.exe --ui-shot out.png [--ui-page N] [--ui-size WxH] ----
    // 拍完就退出，不进事件循环。给 UI 走查 / 与设计稿比对用。
    if (uiShotMode) {
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

        // --ui-section N：设置页的左栏分区（0 通用 … 6 关于）。
        // 没有它就只能靠临时改 QML 才能拍到某个分区，走查时太费劲。
        const int sectionIndex = valueAfter("--ui-section").toInt();
        if (sectionIndex > 0) {
            if (QObject *settingsPage = window->findChild<QObject *>(QStringLiteral("settingsPage"))) {
                settingsPage->setProperty("currentSection", sectionIndex);
            }
        }

        window->show();

        // 不能只 processEvents：那样不推进动画驱动，带 `Behavior on color` 的控件
        // 会永远停在动画第一帧（截图里就是浅色）。真跑一段事件循环等动画收敛。
        QEventLoop loop;
        QTimer::singleShot(700, &loop, &QEventLoop::quit);
        loop.exec();

        // STUDIOUS_UI_CALL=<Q_INVOKABLE>：走查"点下去之后"的样子用（通知条、弹窗、
        // 列表展开态）。只在这个截图分支里生效，正式启动路径完全不看这个变量。
        const QByteArray uiCall = qgetenv("STUDIOUS_UI_CALL");
        if (!uiCall.isEmpty()) {
            const QString uiArg = QString::fromUtf8(qgetenv("STUDIOUS_UI_ARG"));
            QEventLoop callLoop;
            QMetaObject::invokeMethod(backend,
                                      uiCall.constData(),
                                      Qt::DirectConnection,
                                      Q_ARG(QString, uiArg));
            QTimer::singleShot(1200, &callLoop, &QEventLoop::quit);
            callLoop.exec();
        }

        const QImage shot = window->grabWindow();
        const bool saved = !shot.isNull() && shot.save(outPath);
        // 两个尺寸都要打：--ui-size 给的是逻辑像素，grabWindow() 出的是物理像素。
        // 高 DPI 下二者不等（DPR 1.5 时 --ui-size 1280x800 存出来的是 1920x1200），
        // 以前只打后者，看起来就像 --ui-size 被忽略了。
        const qreal dpr = window->devicePixelRatio();
        const int pixelWidth =
            shot.isNull() ? static_cast<int>(width * dpr + 0.5) : shot.width();
        const int pixelHeight =
            shot.isNull() ? static_cast<int>(height * dpr + 0.5) : shot.height();
        std::printf("[ui-shot] %s -> %s (%dx%d @%.1fx, logical %dx%d)\n",
                    saved ? "saved" : "FAILED",
                    outPath.toUtf8().constData(),
                    pixelWidth,
                    pixelHeight,
                    static_cast<double>(dpr),
                    width,
                    height);
        return saved ? 0 : 1;
    }

    return app.exec();
}
