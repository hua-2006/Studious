#include "AppSettings.h"

#include <QColor>
#include <QSettings>

#include <algorithm>

namespace {

// QSettings 里存的是 QVariant，用户可能手改成越界值（注册表直接改），读回来必须夹紧，
// 否则 QSpinBox 之外的地方（比如 http 超时）会拿到荒谬数字。
int clampedInt(int value, int lo, int hi)
{
    return std::clamp(value, lo, hi);
}

double clampedDouble(double value, double lo, double hi)
{
    return std::clamp(value, lo, hi);
}

// QSettings 的 key 统一前缀，避免和将来别的模块撞名。
QString key(const char *suffix)
{
    return QLatin1String("settings/") + QLatin1String(suffix);
}

} // namespace

AppSettings AppSettings::defaults()
{
    AppSettings s;

    // 本机网关默认值（与 backend_selftest 的 --llm 模式保持一致）。
    s.llm.baseUrl = QStringLiteral("http://127.0.0.1:3065/v1");
    s.llm.apiKey = QStringLiteral("12345678");
    s.llm.model = QStringLiteral("deepseek-v4.1-flash");
    s.llm.timeoutMs = 60000;
    s.llm.maxTokens = 2048;
    s.llm.temperature = 0.2;
    s.llm.systemPrompt.clear();   // 空 = 后端内置 prompt
    s.llm.reasoningEffort = QStringLiteral("none");   // 默认关闭思考

    s.webProviderId.clear();   // 自动
    s.engineId.clear();        // 自动
    s.srcLang = QStringLiteral("auto");
    s.dstLang = QStringLiteral("zh");
    s.cacheEnabled = true;
    s.useLocalEngine = false;   // 默认走云端：本地引擎要用户自己去「资产」页下载
    s.localModelFileName.clear();   // 空 = 第一个可用模型
    s.localUseGpu = true;           // 装了 vulkan 运行时就用 GPU（实测 0.5B 单句快 4 倍多）
    s.localContextSize = 2048;      // llama-server -c
    s.localThreads = 0;             // 0 = 自动（不传 -t）
    s.localGpuLayers = -1;          // -1 = 自动（跟随 localUseGpu）
    s.localExtraArgs.clear();       // 空 = 不加额外参数
    s.localStopOnSwitch = false;    // 切走不停：省一次冷启动，代价是占内存
    s.localPreheat = false;         // 启动即预热：默认关，别让开机莫名其妙占几 GB
    s.localIdleStop = false;        // 空闲不停：默认关
    s.localIdleMinutes = 10;        // 打开「空闲自动停」时的默认阈值
    s.uiLanguage.clear();   // 空 = 跟随系统语言
    s.accentSeed.clear();   // 空 = 默认紫 #6750A4
    s.autoStart = false;            // 别替用户改系统启动项
    s.minimizeToTray = false;       // 默认老老实实待在任务栏
    s.closeBehavior = QStringLiteral("exit");
    s.singleInstance = true;        // 防多开是绝大多数人的预期
    s.sameLangHint = true;
    s.suspiciousLangWarn = true;
    s.autoDetectSource = true;
    s.proxyUrl.clear();             // 空 = 不覆盖环境变量
    s.httpTimeoutMs = 30000;        // 后端各调用点自己的超时多数是 5~15 s，这里只做上限抬升
    s.assetRoot.clear();            // 空 = %APPDATA%/studious/assets
    return s;
}

AppSettings AppSettings::load()
{
    const AppSettings def = defaults();

    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));

    const auto textOr = [&store](const QString &k, const QString &fallback) {
        const QString value = store.value(k).toString().trimmed();
        return value.isEmpty() ? fallback : value;
    };
    // 地址 / 模型这两项允许"留空 = 还没配"：只有注册表里根本没有这个键时才用默认值。
    // 否则用户把 Base URL 清掉，重启又被默认端点悄悄填回去，界面永远不说「未配置」，
    // 状态胶囊一直绿 —— 而那个默认端点（127.0.0.1:3065）在别人机器上根本没在跑。
    const auto textOrKeepEmpty = [&store](const QString &k, const QString &fallback) {
        if (!store.contains(k)) {
            return fallback;
        }
        return store.value(k).toString().trimmed();
    };

    AppSettings s = def;

    s.llm.baseUrl = textOrKeepEmpty(key("llm/baseUrl"), def.llm.baseUrl);
    s.llm.apiKey = textOr(key("llm/apiKey"), def.llm.apiKey);
    s.llm.model = textOrKeepEmpty(key("llm/model"), def.llm.model);
    // systemPrompt 不做空值回落：留空是有意义的状态。
    s.llm.systemPrompt = store.value(key("llm/systemPrompt")).toString();
    s.llm.timeoutMs = clampedInt(store.value(key("llm/timeoutMs"), def.llm.timeoutMs).toInt(),
                                 1000,
                                 180000);
    s.llm.maxTokens = clampedInt(store.value(key("llm/maxTokens"), def.llm.maxTokens).toInt(),
                                 1,
                                 32768);
    s.llm.temperature = clampedDouble(
        store.value(key("llm/temperature"), def.llm.temperature).toDouble(), 0.0, 1.5);
    // 思考等级同样不做空值回落：空串 = 不传参数（跟随服务端默认），是有意义的状态。
    s.llm.reasoningEffort =
        store.value(key("llm/reasoningEffort"), def.llm.reasoningEffort).toString().trimmed();

    // 这三个界面上可调，必须一起落盘 —— 少了它们，用户改完重启就回默认，
    // 会以为「设置没生效」。
    s.llm.retryBudgetMs = clampedInt(
        store.value(key("llm/retryBudgetMs"), def.llm.retryBudgetMs).toInt(), 1000, 300000);
    s.llm.batchSize =
        clampedInt(store.value(key("llm/batchSize"), def.llm.batchSize).toInt(), 1, 64);
    s.llm.stripModelNotes =
        store.value(key("llm/stripModelNotes"), def.llm.stripModelNotes).toBool();

    // 这两个空串本身是合法值（= 自动），textOr 回落到的也是空串，行为一致。
    s.webProviderId = textOr(key("web/providerId"), def.webProviderId);
    s.engineId = textOr(key("engine/id"), def.engineId);

    // 引擎优先级：逗号分隔串。空串 = 用后端默认顺序。
    s.enginePriority.clear();
    const QString priorityText = store.value(key("engine/priority")).toString();
    for (const QString &part : priorityText.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty() && !s.enginePriority.contains(trimmed)) {
            s.enginePriority << trimmed;
        }
    }

    s.srcLang = textOr(key("lang/src"), def.srcLang);
    s.dstLang = textOr(key("lang/dst"), def.dstLang);

    // 布尔值不需要空值回落：缺失时 QSettings 直接给默认值 true（= 与 defaults() 一致）。
    s.cacheEnabled = store.value(key("cache/enabled"), true).toBool();

    // 本地引擎开关：缺失/非法值一律当 false（默认走云端），
    // 否则注册表里一个手误的字符串就会被 QVariant::toBool 当成 true。
    s.useLocalEngine = store.value(key("llm/useLocalEngine"), def.useLocalEngine).toBool();

    // 本地引擎用哪个模型：空串是合法值（= 第一个可用模型），所以不做 textOr 回落。
    // 是否真的存在由 LocalEngine / 界面去校验（资产可能被删）。
    s.localModelFileName = store.value(key("llm/localModel")).toString().trimmed();
    s.localUseGpu = store.value(key("llm/localUseGpu"), def.localUseGpu).toBool();

    // 本地 llama-server 运行时参数。注册表可以被手改（甚至改成字符串 / 负数），
    // 越界值一律夹回边界而不是丢弃：丢掉只会让用户以为"改了没生效"，
    // 夹紧至少还是个能跑起来的可用值。
    s.localContextSize = clampedInt(
        store.value(key("llm/localContextSize"), def.localContextSize).toInt(), 512, 32768);
    // 0 是合法值（= 自动，不传 -t），所以下界就是 0，不能夹成 1。
    s.localThreads =
        clampedInt(store.value(key("llm/localThreads"), def.localThreads).toInt(), 0, 64);
    // -1 是合法值（= 自动，跟随 localUseGpu），下界就是 -1。
    // -1 在 QSettings 的 native 后端里会写成 REG_DWORD 0xFFFFFFFF，读回来是
    // unsigned int，直接 toInt() 溢出成 0 —— 而 0 的语义是"一层都不卸载（纯 CPU）"，
    // 于是「自动」静默变成「不用 GPU」。先按 64 位读、再按 32 位有符号还原。
    bool layersOk = false;
    const qint64 rawLayers =
        store.value(key("llm/localGpuLayers"), def.localGpuLayers).toLongLong(&layersOk);
    const int restoredLayers =
        layersOk ? static_cast<int>(static_cast<quint32>(rawLayers)) : def.localGpuLayers;
    s.localGpuLayers = clampedInt(restoredLayers, -1, 999);
    // 额外参数是自由文本：空串合法（= 不加），所以不走 textOr，只 trim。
    s.localExtraArgs = store.value(key("llm/localExtraArgs")).toString().trimmed();

    // 生命周期三开关。bool 用 toBool()：注册表里被写成字符串 "true"/"1" 也能认。
    s.localStopOnSwitch = store.value(key("llm/localStopOnSwitch"), def.localStopOnSwitch).toBool();
    s.localPreheat = store.value(key("llm/localPreheat"), def.localPreheat).toBool();
    s.localIdleStop = store.value(key("llm/localIdleStop"), def.localIdleStop).toBool();
    // 空闲阈值：1 分钟起（0 会让定时器立刻触发，等于"翻译完就停"，不是用户想要的）。
    s.localIdleMinutes = clampedInt(
        store.value(key("llm/localIdleMinutes"), def.localIdleMinutes).toInt(), 1, 240);

    // 界面语言：空串是有意义的值（= 跟随系统），所以不做 textOr 回落。
    // 注册表可能被手改成乱七八糟的值，不在白名单里就当"跟随系统"。
    const QString lang = store.value(key("ui/language")).toString().trimmed().toLower();
    s.uiLanguage = (lang == QLatin1String("zh") || lang == QLatin1String("en")) ? lang : QString();

    // 主题：只认三个白名单值，手改成别的就回落到跟随系统。
    const QString theme = store.value(key("ui/theme")).toString().trimmed().toLower();
    s.themeMode = (theme == QLatin1String("light") || theme == QLatin1String("dark"))
                      ? theme
                      : QStringLiteral("system");

    // 强调色：空串本身是合法值（= 默认紫），但注册表可能被手改成乱码，
    // 只有 QColor 解析不了的才按空处理，交给 Theme 回落默认色。
    const QString accent = store.value(key("ui/accent")).toString().trimmed();
    s.accentSeed = QColor(accent).isValid() ? accent : QString();

    // 字号缩放：夹在 80~150，缺失或越界都回落到 100。
    s.fontScale = clampedInt(store.value(key("ui/fontScale"), def.fontScale).toInt(), 80, 150);

    // 桌面集成：bool 一律 toBool()，注册表被写成 "true"/"1" 也能认。
    s.autoStart = store.value(key("desktop/autoStart"), def.autoStart).toBool();
    s.minimizeToTray = store.value(key("desktop/minimizeToTray"), def.minimizeToTray).toBool();
    s.singleInstance = store.value(key("desktop/singleInstance"), def.singleInstance).toBool();

    // 关闭行为只认两个白名单值，其它一律当"退出"。
    const QString closeBehavior =
        store.value(key("desktop/closeBehavior"), def.closeBehavior).toString().trimmed().toLower();
    s.closeBehavior = (closeBehavior == QLatin1String("tray")) ? closeBehavior
                                                              : QStringLiteral("exit");

    // 翻译行为三个展示开关。
    s.sameLangHint = store.value(key("translate/sameLangHint"), def.sameLangHint).toBool();
    s.suspiciousLangWarn =
        store.value(key("translate/suspiciousLangWarn"), def.suspiciousLangWarn).toBool();
    s.autoDetectSource =
        store.value(key("translate/autoDetectSource"), def.autoDetectSource).toBool();

    // 代理：空串是合法值（= 不覆盖），所以只 trim。
    s.proxyUrl = store.value(key("network/proxy")).toString().trimmed();

    // 全局超时下限：夹在 1 s ~ 10 min，越界夹回边界而不是丢掉。
    s.httpTimeoutMs =
        clampedInt(store.value(key("network/timeoutMs"), def.httpTimeoutMs).toInt(), 1000, 600000);

    // 资产根目录：空串是合法值（= 默认位置），只 trim。
    s.assetRoot = store.value(key("asset/root")).toString().trimmed();

    return s;
}

void AppSettings::save() const
{
    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));

    store.setValue(key("llm/baseUrl"), llm.baseUrl);
    store.setValue(key("llm/apiKey"), llm.apiKey);
    store.setValue(key("llm/model"), llm.model);
    store.setValue(key("llm/timeoutMs"), llm.timeoutMs);
    store.setValue(key("llm/maxTokens"), llm.maxTokens);
    store.setValue(key("llm/temperature"), llm.temperature);
    store.setValue(key("llm/systemPrompt"), llm.systemPrompt);
    store.setValue(key("llm/reasoningEffort"), llm.reasoningEffort);
    store.setValue(key("llm/retryBudgetMs"), llm.retryBudgetMs);
    store.setValue(key("llm/batchSize"), llm.batchSize);
    store.setValue(key("llm/stripModelNotes"), llm.stripModelNotes);

    store.setValue(key("web/providerId"), webProviderId);
    store.setValue(key("engine/id"), engineId);
    store.setValue(key("engine/priority"), enginePriority.join(QLatin1Char(',')));
    store.setValue(key("lang/src"), srcLang);
    store.setValue(key("lang/dst"), dstLang);

    store.setValue(key("cache/enabled"), cacheEnabled);
    store.setValue(key("llm/useLocalEngine"), useLocalEngine);
    store.setValue(key("llm/localModel"), localModelFileName);   // 空 = 第一个可用模型
    store.setValue(key("llm/localUseGpu"), localUseGpu);
    // 下面 4 个也是"结构体有字段但 save() 漏写就会重启回默认"的那类，别再漏。
    store.setValue(key("llm/localContextSize"), localContextSize);   // 512~32768
    store.setValue(key("llm/localThreads"), localThreads);           // 0 = 自动
    store.setValue(key("llm/localGpuLayers"), localGpuLayers);       // -1 = 自动
    store.setValue(key("llm/localExtraArgs"), localExtraArgs.trimmed());   // 空 = 不加参数
    store.setValue(key("llm/localStopOnSwitch"), localStopOnSwitch);       // 切引擎就停
    store.setValue(key("llm/localPreheat"), localPreheat);                 // 启动即预热
    store.setValue(key("llm/localIdleStop"), localIdleStop);               // 空闲自动停
    store.setValue(key("llm/localIdleMinutes"), localIdleMinutes);         // 1~240
    store.setValue(key("ui/language"), uiLanguage);   // 空 = 跟随系统
    store.setValue(key("ui/theme"), themeMode);       // system / light / dark
    store.setValue(key("ui/fontScale"), fontScale);   // 80~150，100 = 原始大小
    store.setValue(key("ui/accent"), accentSeed);     // 空 = 默认紫 #6750A4
    store.setValue(key("desktop/autoStart"), autoStart);
    store.setValue(key("desktop/minimizeToTray"), minimizeToTray);
    store.setValue(key("desktop/closeBehavior"), closeBehavior);   // exit / tray
    store.setValue(key("desktop/singleInstance"), singleInstance);
    store.setValue(key("translate/sameLangHint"), sameLangHint);
    store.setValue(key("translate/suspiciousLangWarn"), suspiciousLangWarn);
    store.setValue(key("translate/autoDetectSource"), autoDetectSource);
    store.setValue(key("network/proxy"), proxyUrl);               // 空 = 跟环境变量
    store.setValue(key("network/timeoutMs"), httpTimeoutMs);      // 1000~600000
    store.setValue(key("asset/root"), assetRoot);                 // 空 = 默认位置

    store.sync();
}
