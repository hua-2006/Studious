#include "AppBackend.h"
#include "AppSettings.h"
#include "AssetFormat.h"
#include "AssetPaths.h"
#include "CacheHandle.h"
#include "EngineIds.h"
#include "Languages.h"
#include "LocalEngine.h"
#include "LiveSession.h"
#include "LogSink.h"
#include "MiniSession.h"
#include "Paragraphs.h"
#include "ScreenFlow.h"
#include "platform/ScreenCaptureWin.h"
#include "TranslateJob.h"
#include "i18n/Strings.h"
#include "platform/IPlatform.h"
#include "platform/PlatformFactory.h"

// 引擎 id 的权威实现（归一 / 优先级 / 显示名）在 EngineIds，命令行不再自留一份顺序表。
namespace engine = studious::ui::engine;

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
#include <QQuickItem>
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
#include <studious/contract/LanguageRegistry.h>
#include <studious/contract/Types.h>
#include <studious/i18n/L10n.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>
#include <studious/translators/LocalMtTranslator.h>

#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

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
    // 字体编在 qrc 里（见 CMake 的 fonts 资源）：发行包里没有 font/ 目录，
    // 从磁盘读会在用户机器上静默失败，然后整界面掉回系统字体。
    FontSet fonts;

    // 界面：思源黑体 SC（改名成「软件默认-思源黑体」，见 resources/fonts/README.md）。
    // Regular + Medium + Bold 三个字重，让 QML 里的 Font.Normal / DemiBold / Bold
    // 都能落到真字形上。
    fonts.ui = loadFontFamily(QStringLiteral(":/fonts/StudiousSans-Regular.otf"));
    loadFontFamily(QStringLiteral(":/fonts/StudiousSans-Medium.otf"));
    loadFontFamily(QStringLiteral(":/fonts/StudiousSans-Bold.otf"));

    // 译文：霞鹜文楷。
    fonts.content = loadFontFamily(QStringLiteral(":/fonts/StudiousWenKai-Regular.ttf"));

    // 等宽：霞鹜文楷 Mono。
    fonts.mono = loadFontFamily(QStringLiteral(":/fonts/StudiousWenKaiMono-Regular.ttf"));

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

// 注意：不要在这里关闭匿名 namespace —— 下面的工具函数还在同一个匿名 namespace 里，
// 它一直到文件末尾（main 之前）才闭合。

// 进程级唯一的翻译缓存改由 CacheHandle.h 提供（studious::ui::sharedCache()）：
// 以前这里有一份、QuickTranslate.cpp 里又有一份 processCache()，两处各养一个实例。

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
        const studious::translate::LocalMtTranslator mt(studious::ui::paths::modelsMtDir());
        if (mt.installedPairs().isEmpty()) {
            return false;
        }
        const QString src = studious::core::LanguageRegistry::normalize(srcLang);
        const QString dst = studious::core::LanguageRegistry::normalize(dstLang);
        // 源语言是"自动检测"时 Router 必然拒绝（本地 MT 没法先探测再选模型），
        // 但引擎本身是装好的 —— 和界面一样按"装好没有"算可用。
        // 「自动」的判定只有一份：langs::isAuto（空串 / auto，不分大小写）。
        if (studious::ui::langs::isAuto(src)) {
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
        // 顺序只有 EngineIds 一份；命令行把手动链里的 llm-local 摘掉 ——
        // --engine auto 是脚本/冒烟用的，不该顺手拉起一个 1~3 GB 的本地模型。
        QStringList order = settings.enginePriority.isEmpty() ? engine::defaultPriority()
                                                              : settings.enginePriority;
        if (settings.enginePriority.isEmpty()) {
            order.removeAll(QStringLiteral("llm-local"));
        }

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
    say(QStringLiteral("  studious.exe --asset-verify <assetId>"));
    say(QStringLiteral("                                     # 按启动前规则校验一条已安装资产（导入的不校验哈希）"));
    say(QStringLiteral("  studious.exe --capture-probe <x,y,w,h,out.png>   （诊断：抓一块物理像素区域）"));
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
    req.paragraphs = studious::ui::paragraphs::split(text);
    if (req.paragraphs.isEmpty()) {
        say(QStringLiteral("[FAIL] 切分后没有任何段落"));
        return 1;
    }
    // 源语言：没给 --from 就用设置里的（默认 auto）。
    // 两边都过 langs::normalizeSource，「自动」只有 "auto" 一种写法。
    const QString srcLang = from.isEmpty()
                                ? studious::ui::langs::normalizeSource(settings.srcLang)
                                : studious::ui::langs::normalizeSource(from);

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
    // 目标语言：丢掉「自动」（目标语言不能是自动），也别和源语言撞上 ——
    // 源=目标时后端只会原样回填，CLI 看起来像"翻了个寂寞"。
    req.dstLangs = studious::ui::langs::normalizeTargetsAgainstSource(
        studious::ui::langs::normalizeTargets(QStringList{to}, engineChoice.uiId), srcLang);
    if (req.dstLangs.first() != to) {
        say(QStringLiteral("[WARN] 目标语言已从 %1 调整为 %2（%3）")
                .arg(to,
                     req.dstLangs.first(),
                     studious::ui::langs::isAuto(to)
                         ? QStringLiteral("目标语言不能是自动检测")
                         : QStringLiteral("不能和源语言相同")));
    }
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
    req.cache = &studious::ui::sharedCache();
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
                 req.dstLangs.value(0),
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
        const QString srcNorm = studious::ui::langs::normalizeSource(req.srcLang);
        // 目标语言以请求里的为准（上面已经做过"去自动 + 去重源语言"的归一），
        // 这里再用原始 `to` 判定会把 zh→zh 当成同语言、直接回填原文。
        const QString dstNorm = studious::core::LanguageRegistry::normalize(req.dstLangs.value(0));
        const bool sameLanguage = !studious::ui::langs::isAuto(srcNorm) && srcNorm == dstNorm;

        if (useLocalEngine && !sameLanguage) {
            LocalEngineConfig cfg;
            if (!roundModel.isEmpty()) {
                cfg.modelPath = QDir(LocalEngine::llmModelsDir()).filePath(roundModel);
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
                                 // byLang 的键是"实际要翻到的语言"，也就是归一后的目标语言；
        // 用原始的 `to` 取会在 zh→zh 被改成 en 时拿到空表（译文看着像没翻）。
        const QString wantDst = req.dstLangs.value(0);
        const QStringList lines = result.outcome.byLang.value(wantDst);
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

    // 安装根目录与 GUI 完全一致（paths::assetsRoot()：默认 <dataRoot>/assets）。
    const QString installRoot = studious::ui::paths::assetsRoot();

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

    const QString installRoot = studious::ui::paths::assetsRoot();

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
    // 版名（release codename）：version.json 里的 codename，可空。
    QString codename;
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
        info.codename = obj.value(QStringLiteral("codename")).toString(info.codename);
        info.channel = obj.value(QStringLiteral("channel")).toString(info.channel);
        info.channelLabel = obj.value(QStringLiteral("channelLabel")).toString(info.channelLabel);

        std::printf("[appinfo] %s 版本 %s%s%s（%s）\n",
                    info.name.toUtf8().constData(),
                    info.version.toUtf8().constData(),
                    info.codename.isEmpty() ? "" : " ",
                    info.codename.toUtf8().constData(),
                    path.toUtf8().constData());
        return info;
    }

    std::fprintf(stderr, "[appinfo] 没找到 version.json，用兜底值\n");
    return AppInfo{};
}

    // --asset-verify <assetId>：按「启动前校验」的同一套逻辑检查一条已安装资产。
//
// 用途：验收「下载安装的资产必须逐字节对上清单哈希、用户导入的只确认文件还在」。
// origin 记录在 <安装根>/state/asset-origins.json。
int runAssetVerify(const QStringList &args)
{
    QString assetId;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--asset-verify")) {
            const bool hasValue = (i + 1) < args.size() && !args.at(i + 1).startsWith(QLatin1String("--"));
            if (hasValue) {
                assetId = args.at(++i);
            }
        } else if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
    }

    if (assetId.isEmpty()) {
        say(QStringLiteral("[FAIL] 用法：studious.exe --asset-verify <assetId>"));
        return 1;
    }

    studious::asset::Manifest manifest;
    if (!studious::ui::paths::loadManifest(&manifest)) {
        say(QStringLiteral("[FAIL] 找不到或无法解析资产清单：") +
            QDir::toNativeSeparators(studious::ui::paths::manifestPath()));
        return 1;
    }

    studious::asset::AssetRegistry registry(studious::ui::paths::assetsRoot(), manifest);
    const studious::asset::AssetOrigin origin = registry.originOf(assetId);
    QString originName = QStringLiteral("unknown");
    if (origin == studious::asset::AssetOrigin::Import) {
        originName = QStringLiteral("import");
    } else if (origin == studious::asset::AssetOrigin::Download) {
        originName = QStringLiteral("download");
    }

    QString error;
    const bool ok = registry.verifyInstalled(assetId, &error);
    say(QStringLiteral("[verify] %1 origin=%2 -> %3")
            .arg(assetId, originName, ok ? QStringLiteral("ok") : QStringLiteral("FAIL")));
    if (!ok) {
        say(error);
    }
    return ok ? 0 : 1;
}

// --capture-probe <x,y,w,h,out.png>：诊断用，抓一块物理像素区域并打印亮度。
//
// 用途：确认「游戏窗口抓出来是黑的」到底是 GDI 读不到硬件平面，还是我们的调用错了。
// 配合环境变量 STUDIOUS_FORCE_DXGI=1 对比两条路（见 ScreenCaptureWin.cpp）。
int runCaptureProbe(const QStringList &args)
{
    QString spec;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--capture-probe")) {
            if ((i + 1) < args.size() && !args.at(i + 1).startsWith(QLatin1String("--")))
                spec = args.at(++i);
        }
    }
    const QStringList parts = spec.split(QLatin1Char(','));
    if (parts.size() != 5) {
        say(QStringLiteral("[FAIL] 用法：studious.exe --capture-probe <x,y,w,h,out.png>"));
        return 1;
    }
    bool ok = true;
    const int x = parts.at(0).toInt(&ok);
    const int y = parts.at(1).toInt(&ok);
    const int w = parts.at(2).toInt(&ok);
    const int h = parts.at(3).toInt(&ok);
    if (!ok || w <= 0 || h <= 0) {
        say(QStringLiteral("[FAIL] 几何参数不对：%1").arg(spec));
        return 1;
    }

    const studious::platform::CaptureResult result =
        studious::platform::winCaptureRect(QRect(x, y, w, h));
    if (result.status != studious::platform::PlatformStatus::Ok) {
        say(QStringLiteral("[probe] FAIL %1").arg(result.error));
        return 1;
    }

    // 抽样算亮度（9 px 网格），和 ImageMagick 的 mean 一个量级，够判断黑图。
    qint64 sum = 0;
    int count = 0;
    int bright = 0;
    for (int py = 0; py < result.image.height(); py += 9) {
        for (int px = 0; px < result.image.width(); px += 9) {
            const QRgb c = result.image.pixel(px, py);
            const int lum = (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000;
            sum += lum;
            ++count;
            if (lum > 24)
                ++bright;
        }
    }
    const double mean = count > 0 ? static_cast<double>(sum) / count : 0.0;
    const double brightPct = count > 0 ? 100.0 * bright / count : 0.0;

    const QString outPath = parts.at(4);
    if (!result.image.save(outPath)) {
        say(QStringLiteral("[probe] 保存失败：%1").arg(outPath));
        return 1;
    }
    say(QStringLiteral("[probe] %1x%2 mean=%3 bright=%4%% -> %5")
            .arg(result.image.width())
            .arg(result.image.height())
            .arg(mean, 0, 'f', 1)
            .arg(brightPct, 0, 'f', 1)
            .arg(outPath));
    return 0;
}

// 每个进程入口都要装的日志汇（CLI 也落盘，不然 --smoke 出问题时没现场）。
// 装完再挂 aboutToQuit：正常退出时收尾 flush 一次；缓冲区本身是逐行 flush 的，
// 所以即便走的是 CLI 那种「不跑事件循环直接 return」的路径，日志也不会丢。
void installLogSink(QCoreApplication &app)
{
    studious::ui::logsink::install();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] {
        studious::ui::logsink::flushToDisk();
    });
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // 这个进程是 GUI 子系统（双击不弹控制台黑框），默认没有 stdout/stderr。
    // 带参数从终端里启动时把父进程的控制台接回来，命令行模式的输出才看得见；
    // 已经有输出句柄（被管道 / 文件重定向，比如 `studious.exe --smoke | more`）就别动它，
    // 否则输出会从管道跑到控制台上、脚本抓不到。接不上父控制台就算了，logsink 的日志文件照旧。
    const HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (argc > 1 && (stdOut == nullptr || stdOut == INVALID_HANDLE_VALUE)
        && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }
#endif

    // 先看命令行：--list-models / --smoke / --assets-cli 都只起 QCoreApplication，
    // 不碰 GUI 平台插件（无桌面 / 无 DISPLAY 也能跑）。
    if (hasFlag(argc, argv, "--list-models")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runListModels(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--smoke")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runSmoke(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--assets-cli")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runAssetsCli(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--asset-verify")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runAssetVerify(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--capture-probe")) {
        // 抓屏要 QGuiApplication 里的 QImage 插件（PNG 编码），QCoreApplication 存不出图。
        QGuiApplication app(argc, argv);
        QGuiApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runCaptureProbe(QCoreApplication::arguments());
    }

    if (hasFlag(argc, argv, "--asset-download")) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("studious"));
        applyUiLanguageFromSettings();
        installLogSink(app);
        return runAssetDownload(QCoreApplication::arguments());
    }

    // DPI 感知必须在 QGuiApplication 构造之前设置（Windows 上只生效一次）。
    studious::platform::IPlatform::initProcessDpi();

    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("studious"));

    // 日志汇要尽早装上：后面 QML 加载期的一堆 warning（绑定失败、缺组件）
    // 正是日志窗口最需要看的东西。这行之后的 qWarning 才会进缓冲。
    installLogSink(app);

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
    // 字体必须在 AppBackend 之前注册：构造时会按族名核对「选中的字体是否真的装了」，
    // 顺序反了就会把自带的两个族名报成 (MISSING)。
    const FontSet fonts = loadBundledFonts();
    auto *backend = new studious::ui::AppBackend(&studious::ui::sharedCache(), &app);

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

    // ---- 截屏翻译 / 小窗 / 实时的平台底座 ----
    // 平台实例全进程一份：热键消息窗口只能有一个，抓屏也不必每次重建 GDI 句柄。
    auto platform = studious::platform::createPlatform();
    auto *screenFlow = new studious::ui::ScreenFlow(platform.get(), &app);
    engine.rootContext()->setContextProperty(QStringLiteral("screenFlow"), screenFlow);

    // 实时翻译：和截屏共用平台实例（抓屏、热键、窗口标记都在那一层），
    // 但自己一条轮询链 —— 它的节拍是毫秒，不能被截屏那条链的慢动作拖住。
    auto *liveSession = new studious::ui::LiveSession(platform.get(), &app);
    engine.rootContext()->setContextProperty(QStringLiteral("liveSession"), liveSession);

    // 小窗翻译：一条独立的同步链（用户敲一次、翻一次），不需要平台能力。
    auto *miniSession = new studious::ui::MiniSession(&app);
    engine.rootContext()->setContextProperty(QStringLiteral("miniSession"), miniSession);

    // 热键：文本写在设置里（"Ctrl+Alt+S"），这里翻译成 HotkeySpec 注册。
    // 注册失败（被别的程序占用）不改设置、不阻断启动，只把结果写回 backend.hotkeyStatus，
    // 由快捷键页显示「已被占用」。
    const studious::platform::CapabilityReport platformCaps = platform->capabilities();
    if (platformCaps.hotkey.available) {
        // 注意：backend 与 platform 的生命周期都到 main 结束，这个 lambda 只会在
        // 用户改设置时被触发，捕获裸指针是安全的（connect 的 context 用 backend）。
        const auto applyHotkeys = [backend, platform = platform.get()]() {
            const QStringList ids{QStringLiteral("capture"), QStringLiteral("mini"),
                                  QStringLiteral("live"), QStringLiteral("selection"),
                                  QStringLiteral("home")};
            for (const QString &id : ids) {
                platform->unregisterHotkey(id);
            }

            // 三个键位撞在一起时，系统只会告诉你"被占用"，那个提示对用户毫无信息量。
            // 自己先查一遍，把"和谁的撞了"说清楚。
            QHash<QString, QString> takenCombos;
            const auto registerOne = [backend, platform, &takenCombos](const QString &id,
                                                                       const QString &text) {
                if (text.trimmed().isEmpty()) {
                    // 空 = 用户清掉了这条：既不算失败，也不留上一次的「已占用」。
                    backend->setHotkeyStatus(id, true, QString());
                    return;
                }
                const QString key = text.toUpper();
                if (takenCombos.contains(key)) {
                    const auto label = [](const QString &v) {
                        if (v == QLatin1String("capture"))
                            return QStringLiteral("截图翻译");
                        if (v == QLatin1String("mini"))
                            return QStringLiteral("小窗翻译");
                        if (v == QLatin1String("selection"))
                            return QStringLiteral("划词翻译");
                        if (v == QLatin1String("home"))
                            return QStringLiteral("打开主页");
                        return QStringLiteral("实时翻译");
                    };
                    backend->setHotkeyStatus(
                        id, false,
                        QStringLiteral("与其他快捷键重复：%1 已经给了「%2」。")
                            .arg(text, label(takenCombos.value(key))));
                    return;
                }
                takenCombos.insert(key, id);

                studious::platform::HotkeySpec spec;
                spec.id = id;
                spec.description = text;   // key == 0 时平台层把 description 当组合键文本解析
                QString error;
                const bool ok = platform->registerHotkey(spec, &error);
                if (!ok) {
                    qWarning("[hotkey] %s: %s", qUtf8Printable(text), qUtf8Printable(error));
                }
                backend->setHotkeyStatus(id, ok, ok ? QString() : error);
            };

            registerOne(QStringLiteral("capture"), backend->hotkeyCapture());
            registerOne(QStringLiteral("mini"), backend->hotkeyMini());
            registerOne(QStringLiteral("live"), backend->hotkeyLive());
            registerOne(QStringLiteral("selection"), backend->hotkeySelection());
            registerOne(QStringLiteral("home"), backend->hotkeyHome());
        };

        applyHotkeys();
        QObject::connect(backend, &studious::ui::AppBackend::hotkeysChanged, backend,
                         applyHotkeys);

        platform->setHotkeyHandler([screenFlow](const QString &id) {
            // 三个热键的落地：capture 直接开框选；小窗与实时由 QML 侧接信号。
            if (id == QLatin1String("capture"))
                screenFlow->begin();
            else
                emit screenFlow->launchRequested(id);
        });
    }
    engine.rootContext()->setContextProperty(QStringLiteral("platformName"),
                                             platformCaps.backendName);
    engine.rootContext()->setContextProperty(QStringLiteral("platformCanCapture"),
                                             platformCaps.capture.available);
    engine.rootContext()->setContextProperty(QStringLiteral("platformCanHotkey"),
                                             platformCaps.hotkey.available);

    // 字体在第 1474 行附近（AppBackend 之前）就注册好了：见那里的注释。
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontUi"), fonts.ui);
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontContent"), fonts.content);
    engine.rootContext()->setContextProperty(QStringLiteral("bundledFontMono"), fonts.mono);

    // 关于页的信息：名称/版本/渠道读仓库根的 version.json；提交号构建期取；Qt 版本与架构运行期取。
    const AppInfo appInfo = loadAppInfo();
    engine.rootContext()->setContextProperty(QStringLiteral("appName"), appInfo.name);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), appInfo.version);
    // 版名（release codename）：关于页显示成「版本 0.9.9 Erstling」。
    engine.rootContext()->setContextProperty(QStringLiteral("appCodename"), appInfo.codename);
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
    // 开场动画（Main.qml 里的 splash）：正常启动要播，但两条路必须跳过 ——
    //   --ui-shot：截图工具要拍的是界面本身，开场盖上去就什么也拍不到；
    //   STUDIOUS_NO_SPLASH=1：走查 / 录屏时不想每次都等那一下。
    engine.rootContext()->setContextProperty(
        QStringLiteral("startupSplash"),
        !uiShotMode && !qEnvironmentVariableIsSet("STUDIOUS_NO_SPLASH"));

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

    // 开发用：STUDIOUS_DEV_CAPTURE=1 时启动后自动进截屏流程（方便手动走查拖框）。
    // 放在 screenshot 分支之外：这两件事互斥，截图分支会自己调 begin()。
    if (!uiShotMode && qEnvironmentVariableIsSet("STUDIOUS_DEV_CAPTURE"))
        QTimer::singleShot(1200, screenFlow, [screenFlow] {
            if (!screenFlow->begin())
                qWarning("[dev-capture] begin() refused, phase=%s",
                         qUtf8Printable(screenFlow->property("phase").toString()));
        });

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

        // 不给 --ui-size 就按窗口自己的默认尺寸拍 —— 以前写死 1440×900，
        // 于是截图永远看不出 Main.qml 里的真实默认大小。
        int width = 0;
        int height = 0;
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

        if (width > 0 && height > 0) {
            window->resize(width, height);
        }
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

        // 截图取证专用：抓屏之前先把主窗口挪出可视区。真实热键路径里主窗口本来就压在
        // 别的程序后面，而 --ui-shot 会把它端到最前，抓到的整屏就变成"主界面盖住
        // 目标窗口"，OCR 出来全是自己的 UI 文字。
        //
        // 注意这里**不能 hide()**：CaptureOverlay 是声明在 Main.qml 里的 Window，
        // 会被 Qt 当成主窗口的 transient 子窗口 —— 父窗口一 hide，浮层跟着不可见，
        // 于是 grabWindow() 只能回退去抓主窗口（踩过：截图尺寸 1680x1080 而不是 2561x1601）。
        // 挪到屏幕外面既能避开抓屏，又能让浮层继续活着。
        if (qgetenv("STUDIOUS_UI_TARGET") == QByteArray("screenFlow")) {
            if (qEnvironmentVariableIsSet("STUDIOUS_UI_HIDE_MAIN")) {
                if (auto *main = qobject_cast<QQuickWindow *>(window))
                    main->setPosition(-20000, -20000);
            }
            QEventLoop settle;
            QTimer::singleShot(400, &settle, &QEventLoop::quit);
            settle.exec();
        }

        // 选区可以"单独在最后给"，也可以在调用序列中间用 `rect` 这个词占位。
        // 验证"翻译中途取消"必须用后者：begin -> rect(开跑) -> cancelTranslation。
        bool rectApplied = false;
        auto applyUiRect = [&]() {
            if (rectApplied)
                return;
            rectApplied = true;
            const QByteArray uiRect = qgetenv("STUDIOUS_UI_RECT");
            if (uiRect.isEmpty())
                return;
            const QList<QByteArray> parts = uiRect.split(',');
            if (parts.size() != 4)
                return;
            const QRect rect(parts[0].toInt(), parts[1].toInt(),
                             parts[2].toInt(), parts[3].toInt());
            QEventLoop rectLoop;
            // 实时翻译的取证：同一个矩形换成"在那个选区上开一局实时"。
            // 两条链的入口都叫"给我一个框"，所以共用一个环境变量、按 target 分流。
            const bool liveTarget = qgetenv("STUDIOUS_UI_TARGET") == QByteArray("liveSession");
            if (liveTarget) {
                QMetaObject::invokeMethod(liveSession, "start",
                                          Qt::DirectConnection, Q_ARG(QRect, rect));
            } else {
                QMetaObject::invokeMethod(screenFlow, "confirmSelection",
                                          Qt::DirectConnection, Q_ARG(QRect, rect));
            }
            const int rectWaitMs = [] {
                bool ok = false;
                const int ms = qEnvironmentVariableIntValue("STUDIOUS_UI_RECT_WAIT", &ok);
                return ok && ms > 0 ? ms : 4000;
            }();
            QTimer::singleShot(rectWaitMs, &rectLoop, &QEventLoop::quit);
            rectLoop.exec();
            if (liveTarget) {
                qWarning("[ui-shot] live start done: phase=%s blocks=%d updates=%d status=%s",
                         qUtf8Printable(liveSession->property("phase").toString()),
                         liveSession->property("blocks").toList().size(),
                         liveSession->property("updateCount").toInt(),
                         qUtf8Printable(liveSession->property("statusText").toString()));
                return;
            }
            qWarning("[ui-shot] confirmSelection done: phase=%s words=%d blocks=%d status=%s",
                     qUtf8Printable(screenFlow->property("phase").toString()),
                     screenFlow->property("blocks").toList().size(),
                     screenFlow->property("blocks").toList().size(),
                     qUtf8Printable(screenFlow->property("statusText").toString()));
        };

        // STUDIOUS_UI_SET="prop=value;prop2=value2"：只在这个截图分支里生效，
        // 用 QObject::setProperty 改 Q_PROPERTY（很多设置项的 WRITE 访问器不是 Q_INVOKABLE，
        // invokeMethod 调不到），用来走查"改了设置之后"的界面与落盘形态。
        const QByteArray uiSet = qgetenv("STUDIOUS_UI_SET");
        if (!uiSet.isEmpty()) {
            const QStringList pairs = QString::fromUtf8(uiSet).split(QLatin1Char(';'),
                                                                    Qt::SkipEmptyParts);
            for (const QString &pair : pairs) {
                const int eq = pair.indexOf(QLatin1Char('='));
                if (eq <= 0) {
                    continue;
                }
                const QByteArray name = pair.left(eq).trimmed().toUtf8();
                const QString value = pair.mid(eq + 1).trimmed();
                const bool ok = backend->setProperty(name.constData(), value);
                qWarning("[ui-shot] set %s = %s -> %d",
                         name.constData(),
                         qUtf8Printable(value),
                         ok ? 1 : 0);
            }
        }
        // STUDIOUS_UI_CALL=<Q_INVOKABLE>：走查"点下去之后"的样子用（通知条、弹窗、
        // 列表展开态）。只在这个截图分支里生效，正式启动路径完全不看这个变量。
        const QByteArray uiCall = qgetenv("STUDIOUS_UI_CALL");
        if (!uiCall.isEmpty()) {
            const QString uiArg = QString::fromUtf8(qgetenv("STUDIOUS_UI_ARG"));
            QEventLoop callLoop;
            // 有的 Q_INVOKABLE 是零参数的（startLocalServer / stopLocalServer），
            // 硬塞一个 QString 会匹配不到方法、静默什么都不做。
            // STUDIOUS_UI_TARGET=screenFlow 时把调用打到截屏流程上（截图取证要它）。
            QObject *uiTarget = backend;
            if (qgetenv("STUDIOUS_UI_TARGET") == QByteArray("screenFlow")) {
                uiTarget = screenFlow;
            } else if (qgetenv("STUDIOUS_UI_TARGET") == QByteArray("liveSession")) {
                uiTarget = liveSession;
            } else if (qgetenv("STUDIOUS_UI_TARGET") == QByteArray("miniSession")) {
                uiTarget = miniSession;
            } else if (const QByteArray named = qgetenv("STUDIOUS_UI_TARGET"); !named.isEmpty()) {
                // 其它名字当 objectName 处理：取证时要能调某个浮层窗口上的 QML 函数
                //（例如 liveControlBar.toggleSettings()），不必再为每个窗口加一条分支。
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    if (w->objectName() == QString::fromUtf8(named)) {
                        uiTarget = w;
                        break;
                    }
                }
            }
            // 一次要按顺序触发好几个动作（先 begin() 再 debugSampleBlocks()）时用 `|` 分隔。
            // 每一项可以带 `@毫秒` 后缀，表示"这一步做完后等多久"（默认 400）。
            // 验证"翻译中途取消"就得靠它：begin@4000 等 OCR+翻译跑起来，
            // 再 cancelTranslation@800 看它是不是真的停了。
            const QList<QByteArray> calls = uiCall.split('|');
            for (const QByteArray &raw : calls) {
                QByteArray one = raw;
                int waitMs = 400;
                const qsizetype at = one.lastIndexOf('@');
                if (at > 0) {
                    waitMs = one.mid(at + 1).toInt();
                    one = one.left(at);
                }
                if (one.isEmpty())
                    continue;
                if (one == "rect") {
                    applyUiRect();
                    continue;
                }
                // "objectName:method" 形式：一次序列里要调不同对象时用
                //（例如先 liveSession 开局，再 liveControlBar.toggleSettings()）。
                QObject *oneTarget = uiTarget;
                QByteArray oneName = one;
                const qsizetype colon = one.indexOf(':');
                if (colon > 0) {
                    const QString wanted = QString::fromUtf8(one.left(colon));
                    for (QWindow *w : QGuiApplication::topLevelWindows()) {
                        if (w->objectName() == wanted) {
                            oneTarget = w;
                            break;
                        }
                    }
                    // 页面是普通 Item、不是窗口：窗口里没搜到就进 contentItem 里找
                    //（例如 overlayPage 这种页级 objectName，用来切页内分段控件）。
                    if (oneTarget == uiTarget && window) {
                        if (auto *root = window->contentItem()) {
                            if (auto *item = root->findChild<QQuickItem *>(wanted))
                                oneTarget = item;
                        }
                    }
                    oneName = one.mid(colon + 1);
                }
                const QMetaObject *mo = oneTarget->metaObject();
                // QML 里声明成 function f(x) 的方法签名是 f(QVariant)，
                // 只按 f() / f(QString) 找会两边都落空（invokeMethod 静默返回 false）。
                const int sigNone = mo->indexOfMethod(QByteArray(oneName + "()").constData());
                const int sigVariant =
                    mo->indexOfMethod(QByteArray(oneName + "(QVariant)").constData());
                if (sigNone >= 0) {
                    QMetaObject::invokeMethod(oneTarget, oneName.constData(), Qt::DirectConnection);
                    qInfo("[ui-shot] called %s()", oneName.constData());
                } else if (sigVariant >= 0) {
                    const bool ok = QMetaObject::invokeMethod(oneTarget,
                                                              oneName.constData(),
                                                              Qt::DirectConnection,
                                                              Q_ARG(QVariant, QVariant(uiArg)));
                    qInfo("[ui-shot] called %s(\"%s\") -> %d", oneName.constData(),
                          qUtf8Printable(uiArg), ok ? 1 : 0);
                } else {
                    const bool ok = QMetaObject::invokeMethod(oneTarget,
                                                              oneName.constData(),
                                                              Qt::DirectConnection,
                                                              Q_ARG(QString, uiArg));
                    qInfo("[ui-shot] called %s(\"%s\") -> %d", oneName.constData(),
                          qUtf8Printable(uiArg), ok ? 1 : 0);
                }
                QTimer::singleShot(waitMs, &callLoop, &QEventLoop::quit);
                callLoop.exec();
            }
            QTimer::singleShot(qEnvironmentVariableIntValue("STUDIOUS_UI_WAIT_MS") > 0
                                   ? qEnvironmentVariableIntValue("STUDIOUS_UI_WAIT_MS")
                                   : 1200,
                               &callLoop,
                               &QEventLoop::quit);
            callLoop.exec();
        }

        // STUDIOUS_UI_RECT="x,y,w,h"：直接给选区，省掉真拖鼠标（只用于截图取证）。
        // 如果上面的调用序列里已经用 `rect` 占位跑过一次，这里不会重复执行。
        applyUiRect();

        // STUDIOUS_UI_CURSOR="x,y"：给浮层摆一个假光标位置，用来拍放大镜
        // （真光标在自动截图里动不了）。排在 SELECT 之前：放大镜只在框选阶段可见。
        const QByteArray uiCursor = qgetenv("STUDIOUS_UI_CURSOR");
        if (!uiCursor.isEmpty()) {
            const QList<QByteArray> parts = uiCursor.split(',');
            if (parts.size() == 2) {
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    if (w->objectName() != QStringLiteral("captureOverlay"))
                        continue;
                    w->setProperty("cursorPos",
                                   QPointF(parts[0].toDouble(), parts[1].toDouble()));
                    // 放大镜只在拖框时出现，截图要假一份「正在拖」的状态。
                    if (qEnvironmentVariableIsSet("STUDIOUS_UI_DRAGGING"))
                        w->setProperty("dragging", true);
                    break;
                }
                QEventLoop cursorLoop;
                QTimer::singleShot(400, &cursorLoop, &QEventLoop::quit);
                cursorLoop.exec();
            }
        }

        // STUDIOUS_UI_SELECT="x,y,w,h"：把浮层摆到「已框选、等确认」那一步，
        // 拍 A2 用的。直接改 QML 根对象的属性（Window 就是根对象）。
        const QByteArray uiSelect = qgetenv("STUDIOUS_UI_SELECT");
        if (!uiSelect.isEmpty()) {
            const QList<QByteArray> parts = uiSelect.split(',');
            if (parts.size() == 4) {
                for (QWindow *w : QGuiApplication::topLevelWindows()) {
                    if (w->objectName() != QStringLiteral("captureOverlay"))
                        continue;
                    w->setProperty("localRect",
                                   QRectF(parts[0].toDouble(), parts[1].toDouble(),
                                          parts[2].toDouble(), parts[3].toDouble()));
                    w->setProperty("confirmed", true);
                    break;
                }
                QEventLoop selectLoop;
                QTimer::singleShot(600, &selectLoop, &QEventLoop::quit);
                selectLoop.exec();
            }
        }

        // STUDIOUS_UI_COMMIT="x,y,w,h"：直接调 QML 的 commitSelection（模拟"松手"），
        // 用来区分「松手链路没触发」还是「工具栏可见条件不满足」。
        const QByteArray uiCommit = qgetenv("STUDIOUS_UI_COMMIT");
        if (!uiCommit.isEmpty()) {
            const QList<QByteArray> parts = uiCommit.split(',');
            for (QWindow *w : QGuiApplication::topLevelWindows()) {
                if (w->objectName() != QStringLiteral("captureOverlay"))
                    continue;
                if (parts.size() != 4)
                    break;
                const QRect rect(parts[0].toInt(), parts[1].toInt(),
                                 parts[2].toInt(), parts[3].toInt());
                QEventLoop commitLoop;
                QMetaObject::invokeMethod(w, "commitSelection",
                                          Qt::DirectConnection,
                                          Q_ARG(QVariant, QVariant::fromValue(rect)));
                QTimer::singleShot(800, &commitLoop, &QEventLoop::quit);
                commitLoop.exec();
            }
        }

        // STUDIOUS_UI_RESIZE="e,120,0" / STUDIOUS_UI_MOVE="80,40"：直接驱动浮层的
        // 二次调整与整块搬家，用来验「拖手柄 / 拖框内」这套几何算法（真实拖拽在自动截图里做不了）。
        const auto driveOverlay = [](const char *envName, const char *qmlMethod, bool needsEdge) {
            const QByteArray raw = qgetenv(envName);
            if (raw.isEmpty())
                return;
            const QList<QByteArray> parts = raw.split(',');
            for (QWindow *w : QGuiApplication::topLevelWindows()) {
                if (w->objectName() != QStringLiteral("captureOverlay"))
                    continue;
                if (needsEdge) {
                    if (parts.size() != 3)
                        return;
                    w->setProperty("resizeEdge", QString::fromUtf8(parts[0]));
                    w->setProperty("resizeRect", w->property("localRect"));
                    QMetaObject::invokeMethod(w, qmlMethod, Qt::DirectConnection,
                                              Q_ARG(QVariant, QVariant(parts[1].toDouble())),
                                              Q_ARG(QVariant, QVariant(parts[2].toDouble())));
                } else {
                    if (parts.size() != 2)
                        return;
                    w->setProperty("moveRect", w->property("localRect"));
                    QMetaObject::invokeMethod(w, qmlMethod, Qt::DirectConnection,
                                              Q_ARG(QVariant, QVariant(parts[0].toDouble())),
                                              Q_ARG(QVariant, QVariant(parts[1].toDouble())));
                }
                qWarning("[ui-shot] %s -> localRect=%s", envName,
                         qUtf8Printable(w->property("localRect").toString()));
                break;
            }
        };
        driveOverlay("STUDIOUS_UI_RESIZE", "applyResize", true);
        driveOverlay("STUDIOUS_UI_MOVE", "applyMove", false);

        // STUDIOUS_UI_EMIT="objectName:signal:arg"：直接触发某个 QML 信号，
        // 用来验"点了控件之后到底有没有反应"这条链路（只在这个截图分支生效）。
        // 排在 SELECT/COMMIT 之后：先让工具栏按最终布局摆好，再去点它。
        const QByteArray uiEmit = qgetenv("STUDIOUS_UI_EMIT");
        if (!uiEmit.isEmpty()) {
            const QList<QByteArray> parts = uiEmit.split(':');
            if (parts.size() == 3) {
                // QQuickWindow 的 QObject 子树包含 QML 根对象与全部子项；contentItem
                // 那棵子树只到可视根，页面级的 objectName 未必在里面（踩过）。
                // 另外目标可能在浮层这个独立顶层窗口里，主窗口里找不到要接着找别的窗口。
                const QString name = QString::fromUtf8(parts[0]);
                QObject *target = window->findChild<QObject *>(name);
                if (!target) {
                    for (QWindow *w : QGuiApplication::topLevelWindows()) {
                        target = w->findChild<QObject *>(name);
                        if (target)
                            break;
                    }
                }
                if (target) {
                    QEventLoop emitLoop;
                    // 信号可能不带参数（clicked）也可能带一个 QString（launchRequested），
                    // 先按带参试，失败再按零参。
                    const QMetaObject *tmo = target->metaObject();
                    const QByteArray sigWithArg = parts[1] + "(QString)";
                    const QByteArray sigBare = parts[1] + "()";
                    bool emitted = false;
                    if (tmo->indexOfSignal(sigWithArg.constData()) >= 0) {
                        emitted = QMetaObject::invokeMethod(
                            target, parts[1].constData(), Qt::DirectConnection,
                            Q_ARG(QString, QString::fromUtf8(parts[2])));
                    } else if (tmo->indexOfSignal(sigBare.constData()) >= 0) {
                        emitted = QMetaObject::invokeMethod(target, parts[1].constData(),
                                                            Qt::DirectConnection);
                    }
                    if (!emitted)
                        qWarning("[ui-shot] emit '%s' on '%s' failed",
                                 parts[1].constData(), parts[0].constData());
                    QTimer::singleShot(1500, &emitLoop, &QEventLoop::quit);
                    emitLoop.exec();
                    qWarning("[ui-shot] after emit: phase=%s error='%s'",
                             qUtf8Printable(screenFlow->property("phase").toString()),
                             qUtf8Printable(screenFlow->property("errorText").toString()));
                } else {
                    qWarning("[ui-shot] emit target '%s' not found", parts[0].constData());
                }
            }
        }

        // 浮层/设置小窗都是独立顶层窗口：STUDIOUS_UI_SHOT 给了名字就抓那个窗口，
        // 主窗口那份不参与（"main" 或不设 = 抓主窗口）。
        QQuickWindow *shotWindow = window;
        const QByteArray shotTarget = qgetenv("STUDIOUS_UI_SHOT");
        if (!shotTarget.isEmpty() && shotTarget != QByteArray("main")) {
            const QString wanted = QString::fromUtf8(shotTarget == QByteArray("overlay")
                                                         ? QByteArray("captureOverlay")
                                                         : shotTarget);
            const auto tops = QGuiApplication::topLevelWindows();
            for (QWindow *w : tops) {
                qWarning("[ui-shot] window '%s' %dx%d at (%d,%d) visible=%d",
                         qUtf8Printable(w->objectName()), w->width(), w->height(),
                         w->x(), w->y(), int(w->isVisible()));
                if (w->objectName() == wanted && w->isVisible()) {
                    shotWindow = qobject_cast<QQuickWindow *>(w);
                    break;
                }
            }
        }

        const QImage shot = shotWindow ? shotWindow->grabWindow() : QImage();
        const bool saved = !shot.isNull() && shot.save(outPath);
        // 两个尺寸都要打：--ui-size 给的是逻辑像素，grabWindow() 出的是物理像素。
        // 高 DPI 下二者不等（DPR 1.5 时 --ui-size 1280x800 存出来的是 1920x1200），
        // 以前只打后者，看起来就像 --ui-size 被忽略了。
        // 抓的是浮层时，逻辑尺寸要按浮层算 —— 否则日志说 1120x720、图却是整屏。
        const qreal dpr = shotWindow ? shotWindow->devicePixelRatio() : 1.0;
        const bool shootingOverlay = shotWindow != window;
        const int logicalWidth =
            shootingOverlay ? (shotWindow ? shotWindow->width() : 0)
                            : (width > 0 ? width : window->width());
        const int logicalHeight =
            shootingOverlay ? (shotWindow ? shotWindow->height() : 0)
                            : (height > 0 ? height : window->height());
        const int pixelWidth =
            shot.isNull() ? static_cast<int>(logicalWidth * dpr + 0.5) : shot.width();
        const int pixelHeight =
            shot.isNull() ? static_cast<int>(logicalHeight * dpr + 0.5) : shot.height();
        std::printf("[ui-shot] %s -> %s (%dx%d @%.1fx, logical %dx%d)\n",
                    saved ? "saved" : "FAILED",
                    outPath.toUtf8().constData(),
                    pixelWidth,
                    pixelHeight,
                    static_cast<double>(dpr),
                    logicalWidth,
                    logicalHeight);
        return saved ? 0 : 1;
    }

    return app.exec();
}
