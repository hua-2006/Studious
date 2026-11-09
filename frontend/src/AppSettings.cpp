#include "AppSettings.h"

#include "Languages.h"
#include "platform/SecretStore.h"

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

    // 不带任何默认端点 / Key / 模型名：包里以前写死了开发机的本机网关
    // （http://127.0.0.1:3065/v1 + deepseek-v4.1-flash），发出去就是一条别人永远连不上的地址，
    // 而且把测试配置直接印在了发行版里。留空 = 「未配置」，界面会明说，用户自己填。
    s.llm.baseUrl.clear();
    s.llm.apiKey.clear();
    s.llm.model.clear();
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
    s.fontUi = QStringLiteral("软件默认-思源黑体");        // 界面默认就是项目自带的思源黑体
    s.fontContent = QStringLiteral("软件默认-霞鹜文楷");   // 译文默认就是项目自带的霞鹜文楷
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
    s.assetRoot.clear();            // 空 = <应用目录>/data/assets
    // ---- 截屏 / 小窗 / 实时（三个入口共用这一套） ----
    s.captureAutoRun = false;       // 默认还要点一次「翻译」，防止误触就烧 token
    s.captureAutoCopy = false;      // 别偷偷改剪贴板
    s.captureAutoHide = true;       // 翻完就收起浮层，视线不被挡住
    s.overlayOpacity = 96;          // 96% 还能透一点原来的行距，100% 更像贴纸
    s.overlayFontSize = 13;
    s.overlayKeepOriginal = false;
    s.overlayZoom = 3;                 // 3 倍左右刚好：看得清字，又不至于只剩几个像素
    s.overlayMagnifier = true;      // 拖框时给个放大镜，不然 8px 的字根本对不准
    s.magnifierCorner = QStringLiteral("bottomRight");   // 光标右下角，离内容最远
    s.overlayDim = 45;              // 45% 压暗：能看清框外轮廓，又明显"暗下来"
    s.overlayBackdrop = 70;         // 70%：刚好压住原文，又不至于像一块纯色板
    s.overlayArrowNudge = true;     // 鼠标到 1px 很难，方向键补位
    s.overlayAutoHideDelay = 0;     // 0 = 复制完立刻收
    s.miniAlwaysOnTop = true;
    s.liveIntervalMs = 600;         // 400 太快，本地 MT 跟不上
    s.liveOnlyOnChange = true;      // 内容没变就不重翻，省一次引擎调用
    // 热键默认空（用户自己录），见 AppSettings.h 的说明。
    s.hotkeyCapture.clear();
    s.hotkeyMini.clear();
    s.hotkeyLive.clear();
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
    // 落盘的是密文（Windows 走 DPAPI）；老版本写进去的明文由 unprotect 原样返回，
    // 下次保存时自动变成密文。
    s.llm.apiKey = studious::platform::secret::unprotect(textOr(key("llm/apiKey"), def.llm.apiKey));
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

    // 源语言：「自动」只有一种写法（"auto"），空串一律在入口处折成 "auto" ——
    // 内存里若留一个空串，判断「是否自动」的地方就得各自记着两种写法。
    s.srcLang = studious::ui::langs::normalizeSource(textOr(key("lang/src"), def.srcLang));
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

    // 本地 MT：常驻开关 + 空闲释放阈值。0 是合法值（= 不自动释放）。
    s.mtPreload = store.value(key("mt/preload"), def.mtPreload).toBool();
    s.mtIdleMinutes = clampedInt(store.value(key("mt/idleMinutes"), def.mtIdleMinutes).toInt(), 0,
                                 180);

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
    s.readingFontSize =
        clampedInt(store.value(key("ui/readingFontSize"), def.readingFontSize).toInt(), 11, 24);

    // 字体：空串 = 项目自带字体，__system__ = 跟随系统，其余按字体族名原样用。
    s.fontUi = store.value(key("ui/fontUi"), def.fontUi).toString().trimmed();
    s.fontContent = store.value(key("ui/fontContent"), def.fontContent).toString().trimmed();
    // 老版本把"空串 = 跟随项目字体"写进了注册表，现在默认值就是自带的两个族名，
    // 空串统一折回默认，免得升级后界面上显示「(auto)」而实际用的是默认。
    if (s.fontUi.isEmpty()) {
        s.fontUi = def.fontUi;
    }
    if (s.fontContent.isEmpty()) {
        s.fontContent = def.fontContent;
    }

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

    // ---- 截屏 / 小窗 / 实时 ----
    s.captureAutoRun = store.value(key("capture/autoRunOnSelect"), def.captureAutoRun).toBool();
    s.captureAutoCopy = store.value(key("capture/autoCopy"), def.captureAutoCopy).toBool();
    s.captureAutoHide = store.value(key("capture/autoHide"), def.captureAutoHide).toBool();
    s.overlayOpacity = clampedInt(store.value(key("overlay/opacity"), def.overlayOpacity).toInt(), 40, 100);
    s.overlayFontSize = clampedInt(store.value(key("overlay/fontSize"), def.overlayFontSize).toInt(), 10, 20);
    s.overlayKeepOriginal = store.value(key("overlay/keepOriginal"), def.overlayKeepOriginal).toBool();
    s.overlayZoom = clampedInt(store.value(key("overlay/zoom"), def.overlayZoom).toInt(), 2, 8);
    s.overlayMagnifier = store.value(key("overlay/magnifier"), def.overlayMagnifier).toBool();
    // 四角枚举只认四个值，写错的（手改注册表、旧版本遗留）一律回默认，免得 QML 里算坐标时全靠 if 兜底。
    const QString corner = store.value(key("overlay/magnifierCorner"), def.magnifierCorner).toString().trimmed();
    static const QStringList kCorners{QStringLiteral("bottomRight"), QStringLiteral("bottomLeft"),
                                      QStringLiteral("topRight"), QStringLiteral("topLeft")};
    s.magnifierCorner = kCorners.contains(corner) ? corner : def.magnifierCorner;
    s.overlayDim = clampedInt(store.value(key("overlay/dim"), def.overlayDim).toInt(), 20, 80);
    s.overlayArrowNudge = store.value(key("overlay/arrowNudge"), def.overlayArrowNudge).toBool();
    s.overlayAutoHideDelay = clampedInt(store.value(key("overlay/autoHideDelay"), def.overlayAutoHideDelay).toInt(), 0, 10);
    s.overlayBackdrop = clampedInt(store.value(key("overlay/backdrop"), def.overlayBackdrop).toInt(), 0, 100);
    // 历史：开关只有 true/false；目录为空 = 用默认位置（AppBackend 解析）。
    s.historyEnabled = store.value(key("history/enabled"), def.historyEnabled).toBool();
    s.historyDir = store.value(key("history/dir"), def.historyDir).toString().trimmed();
    s.miniAlwaysOnTop = store.value(key("mini/alwaysOnTop"), def.miniAlwaysOnTop).toBool();
    s.liveIntervalMs = clampedInt(store.value(key("live/intervalMs"), def.liveIntervalMs).toInt(), 400, 1500);
    s.liveOnlyOnChange = store.value(key("live/onlyOnChange"), def.liveOnlyOnChange).toBool();

    // 热键文本只做去空白：合法性由平台层注册时判定，写在这里会把用户的写法「规范」掉。
    // key() 收的是 const char*，所以这里直接写三个字面量、不要再套一层 QString。
    const auto hotkey = [&store](const char *k, const QString &fallback) {
        const QString v = store.value(key(k), fallback).toString().trimmed();
        return v.isEmpty() ? fallback : v;
    };
    s.hotkeyCapture = hotkey("hotkey/capture", def.hotkeyCapture);
    s.hotkeyMini = hotkey("hotkey/mini", def.hotkeyMini);
    s.hotkeyLive = hotkey("hotkey/live", def.hotkeyLive);
    s.hotkeySelection = hotkey("hotkey/selection", def.hotkeySelection);
    s.hotkeyHome = hotkey("hotkey/home", def.hotkeyHome);

    // 更新检查：两个时间戳都允许空（空 = 从没跳/从没查过），所以不走空值回落。
    s.updateSkippedDate = store.value(key("update/skippedDate"), def.updateSkippedDate).toString().trimmed();
    s.updateLastCheckAt = store.value(key("update/lastCheckAt"), def.updateLastCheckAt).toString().trimmed();

    return s;
}

void AppSettings::save() const
{
    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));

    store.setValue(key("llm/baseUrl"), llm.baseUrl);
    store.setValue(key("llm/apiKey"), studious::platform::secret::protect(llm.apiKey));
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
    store.setValue(key("lang/src"), studious::ui::langs::normalizeSource(srcLang));
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
    store.setValue(key("mt/preload"), mtPreload);
    store.setValue(key("mt/idleMinutes"), mtIdleMinutes);                  // 0 = 不自动释放
    store.setValue(key("ui/language"), uiLanguage);   // 空 = 跟随系统
    store.setValue(key("ui/theme"), themeMode);       // system / light / dark
    store.setValue(key("ui/fontScale"), fontScale);   // 80~150，100 = 原始大小
    store.setValue(key("ui/readingFontSize"), readingFontSize);   // 11~24，译文正文字号
    store.setValue(key("ui/fontUi"), fontUi);           // 空 = 自带字体，__system__ = 系统默认
    store.setValue(key("ui/fontContent"), fontContent);
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

    store.setValue(key("capture/autoRunOnSelect"), captureAutoRun);
    store.setValue(key("capture/autoCopy"), captureAutoCopy);
    store.setValue(key("capture/autoHide"), captureAutoHide);
    store.setValue(key("overlay/opacity"), overlayOpacity);       // 40~100
    store.setValue(key("overlay/fontSize"), overlayFontSize);     // 10~20
    store.setValue(key("overlay/keepOriginal"), overlayKeepOriginal);
    store.setValue(key("overlay/zoom"), overlayZoom);                // 2~8
    store.setValue(key("overlay/magnifier"), overlayMagnifier);
    store.setValue(key("overlay/magnifierCorner"), magnifierCorner);  // bottomRight / bottomLeft / topRight / topLeft
    store.setValue(key("overlay/dim"), overlayDim);                   // 20~80
    store.setValue(key("overlay/arrowNudge"), overlayArrowNudge);
    store.setValue(key("overlay/autoHideDelay"), overlayAutoHideDelay);  // 0~10 秒
    store.setValue(key("overlay/backdrop"), overlayBackdrop);  // 0~100
    store.setValue(key("history/enabled"), historyEnabled);
    store.setValue(key("history/dir"), historyDir);                   // 空 = 默认位置
    store.setValue(key("mini/alwaysOnTop"), miniAlwaysOnTop);
    store.setValue(key("live/intervalMs"), liveIntervalMs);       // 400~1500
    store.setValue(key("live/onlyOnChange"), liveOnlyOnChange);
    store.setValue(key("hotkey/capture"), hotkeyCapture);
    store.setValue(key("hotkey/mini"), hotkeyMini);
    store.setValue(key("hotkey/live"), hotkeyLive);
    store.setValue(key("hotkey/selection"), hotkeySelection);
    store.setValue(key("hotkey/home"), hotkeyHome);

    store.sync();
}
