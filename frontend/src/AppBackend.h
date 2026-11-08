#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>

#include "LocalEngine.h"
#include "platform/Desktop.h"
#include "TranslatorRecords.h"

#include <studious/contract/Types.h>

class TranslateJob;
struct TranslateJobResult;
class QTimer;

namespace studious::cache {
class TranslationCache;
}

namespace studious::ui {

// QML 与后端之间唯一的桥。
//
// 设计约定：
//   · QML 不直接碰任何 studious::* 后端类型，只通过这里的属性 / 方法；
//   · 所有 setter 都会落回 AppSettings 并保存，QML 侧不需要管持久化；
//   · 耗时操作（翻译 / 拉模型 / 探测）全部异步或在工作线程，不阻塞 UI 线程。
class AppBackend : public QObject {
    Q_OBJECT

    // ---- 日志窗口 ----
    // 内容来自进程级 logsink（LogSink.h），这里只做「从任意线程的新日志 → 主线程一次 emit」
    // 和剪贴板 / 打开文件这类桥接。
    Q_PROPERTY(QString logText READ logText NOTIFY logTextChanged)
    Q_PROPERTY(QString logFilePath READ logFilePath CONSTANT)
    Q_PROPERTY(QStringList logLines READ logLines NOTIFY logTextChanged)

    // ---- 翻译页状态 ----
    Q_PROPERTY(QString sourceText READ sourceText WRITE setSourceText NOTIFY sourceTextChanged)
    Q_PROPERTY(QString translatedText READ translatedText NOTIFY translatedTextChanged)
    Q_PROPERTY(QString srcLang READ srcLang WRITE setSrcLang NOTIFY srcLangChanged)
    Q_PROPERTY(QStringList dstLangs READ dstLangs WRITE setDstLangs NOTIFY dstLangsChanged)
    Q_PROPERTY(QString engineId READ engineId WRITE setEngineId NOTIFY engineIdChanged)
    // 当前引擎能不能一次翻多个目标语言。本地 MT 是按语言对装的模型，
    // 多目标要么缺模型、要么得多绕几跳，界面据此把「多选」收成单选。
    Q_PROPERTY(bool dstMultiSupported READ dstMultiSupported NOTIFY engineIdChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusTextChanged)
    Q_PROPERTY(QString statusWarning READ statusWarning NOTIFY statusWarningChanged)

    // ---- 只读清单（QML 用于生成下拉 / chip / 分段）----
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    Q_PROPERTY(QVariantList engines READ engines CONSTANT)
    Q_PROPERTY(QVariantList engineStatuses READ engineStatuses NOTIFY engineStatusesChanged)

    // 自动引擎的优先级顺序（拖动排序用）。
    // 只含四个真实引擎：web / llm（云端）/ llm-local（本地）/ local-mt。
    // mock 是开发占位，不进列表。
    Q_PROPERTY(QStringList enginePriority READ enginePriority WRITE setEnginePriority
               NOTIFY enginePriorityChanged)

    // ---- 资产清单（来自 backend/asset/manifest.json + 磁盘扫描）----
    // 每项：{ id, category("llm"|"mt"), type, state, stateText, sizeBytes, sizeText,
    //         path, installed(bool), placeholder(bool), note }
    Q_PROPERTY(QVariantList assets READ assets NOTIFY assetsChanged)
    Q_PROPERTY(QString assetInstallRoot READ assetInstallRoot NOTIFY assetsChanged)

    // 正在下载的资产：id → 进度百分比 0~100。不在表里 = 没在下载。
    Q_PROPERTY(QVariantMap assetProgress READ assetProgress NOTIFY assetProgressChanged)

    // ---- 缓存统计 ----
    Q_PROPERTY(int cacheEntries READ cacheEntries NOTIFY cacheStatsChanged)
    Q_PROPERTY(int cacheHits READ cacheHits NOTIFY cacheStatsChanged)
    Q_PROPERTY(int cacheMisses READ cacheMisses NOTIFY cacheStatsChanged)

    // ---- 派生显示值 ----
    Q_PROPERTY(int paragraphCount READ paragraphCount NOTIFY sourceTextChanged)
    Q_PROPERTY(int charCount READ charCount NOTIFY sourceTextChanged)
    Q_PROPERTY(QString dstLabel READ dstLabel NOTIFY dstLangsChanged)

    // ---- 译文分块（供 QML 逐段渲染：tab 切换 + passthrough 徽标）----
    // 每项：{ index:int, source:QString, text:QString, passthrough:bool, empty:bool }
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY blocksChanged)
    // 当前译文栏正在看的那个目标语言（多目标时对应 tab）
    Q_PROPERTY(QString activeDst READ activeDst WRITE setActiveDst NOTIFY activeDstChanged)
    // 译文要不要带上原文（主界面已不用它；小窗口与实时翻译贴纸还要用）
    Q_PROPERTY(bool showSource READ showSource WRITE setShowSource NOTIFY showSourceChanged)

    // ---- 主题："system" / "light" / "dark"，落 QSettings ----
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)

    // ---- 后端能力的界面出口（去重后由后端供给，前端不再自己存一份）----
    // Web 来源注册表：来自后端 listWebProviders()，含 id / 显示名 / 说明 / 是否需要 key
    Q_PROPERTY(QVariantList webProviders READ webProviders CONSTANT)

    // 本地 LLM 运行期实况
    Q_PROPERTY(QStringList localModels READ localModels CONSTANT)
    Q_PROPERTY(bool localGpuAvailable READ localGpuAvailable CONSTANT)
    Q_PROPERTY(bool localCpuAvailable READ localCpuAvailable CONSTANT)
    Q_PROPERTY(int localPort READ localPort NOTIFY localStateChanged)
    Q_PROPERTY(qint64 localPid READ localPid NOTIFY localStateChanged)
    Q_PROPERTY(QString localBackend READ localBackend NOTIFY localStateChanged)
    Q_PROPERTY(qint64 localStartupMs READ localStartupMs NOTIFY localStateChanged)
    Q_PROPERTY(QString localModelPath READ localModelPath NOTIFY localStateChanged)
    Q_PROPERTY(QString localLogTail READ localLogTail NOTIFY localStateChanged)
    Q_PROPERTY(QString localLogPath READ localLogPath NOTIFY localStateChanged)

    // 云端 LLM：模型列表与思考探测（都是网络请求，异步回传）
    Q_PROPERTY(QVariantList cloudModels READ cloudModels NOTIFY cloudModelsChanged)
    Q_PROPERTY(bool cloudBusy READ cloudBusy NOTIFY cloudBusyChanged)
    Q_PROPERTY(QString cloudProbeResult READ cloudProbeResult NOTIFY cloudProbeResultChanged)

    // ---- 配置页用：引擎具体配置的读写 ----
    // 云端 LLM 的全部连接与采样参数（对应 config::LlmConfig）。
    // 键：baseUrl / apiKey / model / timeoutMs / maxTokens / temperature /
    //     systemPrompt / retryBudgetMs / reasoningEffort / batchSize / stripModelNotes
    Q_PROPERTY(QVariantMap llmConfig READ llmConfig WRITE setLlmConfig NOTIFY llmConfigChanged)
    // Web 来源：空 = 自动（google → mymemory）。后端只吃单个 id，见实现注释。
    Q_PROPERTY(QString webProviderId READ webProviderId WRITE setWebProviderId
               NOTIFY webProviderIdChanged)
    // 本地 LLM：模型文件名（空 = 用第一个可用模型）+ 是否优先 GPU。
    Q_PROPERTY(QString localModel READ localModel WRITE setLocalModel NOTIFY localModelChanged)
    Q_PROPERTY(bool localUseGpu READ localUseGpu WRITE setLocalUseGpu NOTIFY localUseGpuChanged)

    // 本地 LLM 的启动参数。三者都有「自动」语义，所以界面上的默认值就是
    // 0 / 0 / -1，而不是把 2048 之类写死到 QML 里 —— 默认值只活在 LocalEngine 一处。
    Q_PROPERTY(int localContextSize READ localContextSize WRITE setLocalContextSize
                   NOTIFY localLaunchChanged)
    Q_PROPERTY(int localThreads READ localThreads WRITE setLocalThreads NOTIFY localLaunchChanged)
    Q_PROPERTY(int localGpuLayers READ localGpuLayers WRITE setLocalGpuLayers
                   NOTIFY localLaunchChanged)
    Q_PROPERTY(QString localExtraArgs READ localExtraArgs WRITE setLocalExtraArgs
                   NOTIFY localLaunchChanged)

    // 本地服务生命周期策略：三个开关 + 一个分钟数，全部默认关。
    Q_PROPERTY(bool localStopOnSwitch READ localStopOnSwitch WRITE setLocalStopOnSwitch
                   NOTIFY localLaunchChanged)
    Q_PROPERTY(bool localPreheat READ localPreheat WRITE setLocalPreheat
                   NOTIFY localLaunchChanged)
    Q_PROPERTY(bool localIdleStop READ localIdleStop WRITE setLocalIdleStop
                   NOTIFY localLaunchChanged)
    Q_PROPERTY(int localIdleMinutes READ localIdleMinutes WRITE setLocalIdleMinutes
                   NOTIFY localLaunchChanged)

    // 本地 MT 已装的语言对：来自后端 installedPairs() + missingFiles()。
    // 每项：pair / path / missing（QStringList）/ usable（bool）
    Q_PROPERTY(QVariantList mtPairs READ mtPairs NOTIFY mtPairsChanged)

    // ---- 本地 MT 的常驻策略 ----
    // 开 = 启动就把已安装的语言对全部加载（第一次翻译不用等）；
    // 关 = 谁用到谁加载。没有语言库时界面把这两项置灰（见 mtPacksInstalled）。
    Q_PROPERTY(bool mtPreload READ mtPreload WRITE setMtPreload NOTIFY mtStateChanged)
    Q_PROPERTY(int mtIdleMinutes READ mtIdleMinutes WRITE setMtIdleMinutes NOTIFY mtStateChanged)
    // 当前常驻了几个语言对（状态行显示用）。
    Q_PROPERTY(int mtLoadedPairs READ mtLoadedPairs NOTIFY mtStateChanged)
    // 有没有可用的语言库（三项资产齐全）；没有就别让用户开开关。
    Q_PROPERTY(bool mtPacksInstalled READ mtPacksInstalled NOTIFY mtPairsChanged)

    // ---- 界面语言 ----
    // 取值："system" | "zh" | "en"。
    // 只落盘、不热生效 —— 语言在 main() 里加载 QML 之前就要定下来，
    // 所以改完必须重启（UI 里会因此弹一个「重启」按钮）。
    Q_PROPERTY(QString uiLanguage READ uiLanguage WRITE setUiLanguage NOTIFY uiLanguageChanged)

    // 系统语言解析结果（"zh" | "en"）。
    // 用来判断「跟随系统」和当前具体语言是不是同一个 —— 系统本来就是中文时，
    // 从「中文」切到「跟随系统」结果完全一样，不该提示重启。
    Q_PROPERTY(QString systemLanguage READ systemLanguage CONSTANT)

    // ---- 字号缩放百分比（80~150）----
    Q_PROPERTY(int fontScale READ fontScale WRITE setFontScale NOTIFY fontScaleChanged)
    // 译文正文字号（11~24）。只影响正文，左上角浮层/主界面的控件字号由 fontScale 统一缩放。
    Q_PROPERTY(int readingFontSize READ readingFontSize WRITE setReadingFontSize NOTIFY fontScaleChanged)

    // ---- 三个全局热键（文本形式，例如 "Ctrl+Alt+S"；空 = 不注册）----
    // 注册动作在 main.cpp（平台层归它管）。这里只负责校验、落盘、发信号，
    // 由 main.cpp 收到 hotkeysChanged 后重新注册，再把结果写回 hotkeyStatus。
    Q_PROPERTY(QString hotkeyCapture READ hotkeyCapture WRITE setHotkeyCapture
                   NOTIFY hotkeysChanged)
    Q_PROPERTY(QString hotkeyMini READ hotkeyMini WRITE setHotkeyMini NOTIFY hotkeysChanged)
    Q_PROPERTY(QString hotkeyLive READ hotkeyLive WRITE setHotkeyLive NOTIFY hotkeysChanged)
    // 划词翻译：读剪贴板 → 小窗 → 短文本直接翻。
    Q_PROPERTY(QString hotkeySelection READ hotkeySelection WRITE setHotkeySelection
                   NOTIFY hotkeysChanged)
    // 打开主页：把主窗口叫到最前（托盘里也能叫出来）。
    Q_PROPERTY(QString hotkeyHome READ hotkeyHome WRITE setHotkeyHome NOTIFY hotkeysChanged)
    // id → {ok: bool, message: string}；注册失败（被别的程序占用）时给界面看。
    Q_PROPERTY(QVariantMap hotkeyStatus READ hotkeyStatus NOTIFY hotkeyStatusChanged)

    // ---- 强调色（seed）："#RRGGBB"，空 = 默认紫 #6750A4，落 QSettings ----
    Q_PROPERTY(QString accentColor READ accentColor WRITE setAccentColor NOTIFY accentColorChanged)

    // ---- 设置页：翻译行为 / 桌面集成 / 网络 ----
    // 这几个开关全部"只存配置 + 立刻影响本进程行为"，不需要重启（单实例除外，
    // 它在启动时就把本地 socket 命名绑死了）。统一用 prefsChanged 通知，
    // 因为它们都在同一张设置页里、也不需要区分粒度。
    Q_PROPERTY(bool cacheEnabled READ cacheEnabled WRITE setCacheEnabled NOTIFY prefsChanged)
    Q_PROPERTY(bool sameLangHint READ sameLangHint WRITE setSameLangHint NOTIFY prefsChanged)
    Q_PROPERTY(bool suspiciousLangWarn READ suspiciousLangWarn WRITE setSuspiciousLangWarn
                   NOTIFY prefsChanged)
    Q_PROPERTY(bool autoDetectSource READ autoDetectSource WRITE setAutoDetectSource
                   NOTIFY prefsChanged)
    Q_PROPERTY(QString proxyUrl READ proxyUrl WRITE setProxyUrl NOTIFY prefsChanged)
    Q_PROPERTY(int httpTimeoutMs READ httpTimeoutMs WRITE setHttpTimeoutMs NOTIFY prefsChanged)
    Q_PROPERTY(bool autoStart READ autoStart WRITE setAutoStart NOTIFY prefsChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY prefsChanged)
    // 小窗翻译是否置顶（键 settings/mini/alwaysOnTop）。窗口的 flags 直接绑这个属性。
    Q_PROPERTY(bool miniAlwaysOnTop READ miniAlwaysOnTop WRITE setMiniAlwaysOnTop NOTIFY prefsChanged)
    Q_PROPERTY(bool closeToTray READ closeToTray WRITE setCloseToTray NOTIFY prefsChanged)
    Q_PROPERTY(bool singleInstance READ singleInstance WRITE setSingleInstance NOTIFY prefsChanged)
    // 托盘是否真的装上了（没 shell 的会话 / 非 Windows 为 false），界面据此置灰。
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY prefsChanged)

    // ---- 截屏翻译 / 浮层（A 页）----
    // 「选完直接翻译」：松手即送翻，跳过二次确认那一步。
    Q_PROPERTY(bool captureAutoRun READ captureAutoRun WRITE setCaptureAutoRun NOTIFY prefsChanged)
    Q_PROPERTY(bool captureAutoCopy READ captureAutoCopy WRITE setCaptureAutoCopy NOTIFY prefsChanged)
    Q_PROPERTY(bool captureAutoHide READ captureAutoHide WRITE setCaptureAutoHide NOTIFY prefsChanged)
    // 译文底色的不透明度（40~100，百分比）与字号（10~20 px）。
    Q_PROPERTY(int overlayOpacity READ overlayOpacity WRITE setOverlayOpacity NOTIFY prefsChanged)
    Q_PROPERTY(int overlayFontSize READ overlayFontSize WRITE setOverlayFontSize NOTIFY prefsChanged)
    // 译文上方要不要留一行虚线原文。
    Q_PROPERTY(bool overlayKeepOriginal READ overlayKeepOriginal WRITE setOverlayKeepOriginal NOTIFY prefsChanged)
    // 选区放大镜的倍率（2~8 倍），设置页以后直接改这个值。
    Q_PROPERTY(int overlayZoom READ overlayZoom WRITE setOverlayZoom NOTIFY prefsChanged)
    Q_PROPERTY(bool overlayMagnifier READ overlayMagnifier WRITE setOverlayMagnifier NOTIFY prefsChanged)
    Q_PROPERTY(QString magnifierCorner READ magnifierCorner WRITE setMagnifierCorner NOTIFY prefsChanged)
    Q_PROPERTY(int overlayDim READ overlayDim WRITE setOverlayDim NOTIFY prefsChanged)
    Q_PROPERTY(bool overlayArrowNudge READ overlayArrowNudge WRITE setOverlayArrowNudge NOTIFY prefsChanged)
    Q_PROPERTY(int overlayAutoHideDelay READ overlayAutoHideDelay WRITE setOverlayAutoHideDelay NOTIFY prefsChanged)
    // 实时翻译选区的背景底色强度（0~100）。0 = 不铺底。
    Q_PROPERTY(int overlayBackdrop READ overlayBackdrop WRITE setOverlayBackdrop NOTIFY prefsChanged)

    // ---- 实时翻译（C 页）----
    // 轮询间隔（400~1500 ms）。太小会把引擎压满，太大会看不出"实时"。
    Q_PROPERTY(int liveIntervalMs READ liveIntervalMs WRITE setLiveIntervalMs NOTIFY prefsChanged)
    // 画面没变就不重翻：省一次引擎调用，也让"已更新 N 次"这个数字有意义。
    Q_PROPERTY(bool liveOnlyOnChange READ liveOnlyOnChange WRITE setLiveOnlyOnChange NOTIFY prefsChanged)
    // 锁本地引擎：云端 / Web 单次都在 1 秒以上，做实时没有意义。

    // 翻译历史目前只有框架：开关 + 目录。写入与浏览界面在后面批次接上。
    Q_PROPERTY(bool historyEnabled READ historyEnabled WRITE setHistoryEnabled NOTIFY prefsChanged)
    Q_PROPERTY(QString historyDir READ historyDir NOTIFY prefsChanged)

public:
    explicit AppBackend(studious::cache::TranslationCache *cache, QObject *parent = nullptr);
    ~AppBackend() override;

    QString sourceText() const { return m_sourceText; }
    void setSourceText(const QString &text);

    QString translatedText() const { return m_translatedText; }

    QString srcLang() const { return m_srcLang; }
    void setSrcLang(const QString &code);

    QStringList dstLangs() const { return m_dstLangs; }
    bool dstMultiSupported() const { return m_engineId != QLatin1String("local-mt"); }
    void setDstLangs(const QStringList &codes);

    // 别处（小窗的 MiniSession）直接写了 settings/lang/*，改完调这个把共用值拉回来。
    Q_INVOKABLE void reloadLanguagesFromSettings();

    QString engineId() const { return m_engineId; }
    void setEngineId(const QString &id);

    bool running() const { return m_running; }
    QString statusText() const { return m_statusText; }
    bool statusIsError() const { return m_statusIsError; }
    // 「源语言可疑」警告是否显示由设置开关过滤 —— 原始警告照旧攒着，
    // 用户把开关打开就能立刻看到，不用重翻一遍。
    QString statusWarning() const { return m_suspiciousLangWarn ? m_statusWarning : QString(); }

    QVariantList languages() const;
    QVariantList engines() const;
    QVariantList engineStatuses() const { return m_engineStatuses; }

    QStringList enginePriority() const;
    void setEnginePriority(const QStringList &order);

    QVariantList assets() const { return m_assets; }
    QString assetInstallRoot() const { return m_assetInstallRoot; }
    QVariantMap assetProgress() const { return m_assetProgress; }

    int cacheEntries() const;
    int cacheHits() const;
    int cacheMisses() const;

    int paragraphCount() const;
    int charCount() const;
    QString dstLabel() const;

    QVariantList blocks() const;
    bool showSource() const { return m_showSource; }
    void setShowSource(bool on);
    QString activeDst() const { return m_activeDst; }
    void setActiveDst(const QString &code);

    QString themeMode() const { return m_themeMode; }
    void setThemeMode(const QString &mode);

    int fontScale() const { return m_fontScale; }
    void setFontScale(int percent);
    int readingFontSize() const { return m_readingFontSize; }
    void setReadingFontSize(int px);

    QString hotkeyCapture() const { return m_hotkeyCapture; }
    QString hotkeyMini() const { return m_hotkeyMini; }
    QString hotkeyLive() const { return m_hotkeyLive; }
    QString hotkeySelection() const { return m_hotkeySelection; }
    QString hotkeyHome() const { return m_hotkeyHome; }
    void setHotkeyCapture(const QString &combo);
    void setHotkeyMini(const QString &combo);
    void setHotkeyLive(const QString &combo);
    void setHotkeySelection(const QString &combo);
    void setHotkeyHome(const QString &combo);
    QVariantMap hotkeyStatus() const { return m_hotkeyStatus; }
    // main.cpp 注册完把结果写回来；id 取 capture / mini / live。
    Q_INVOKABLE void setHotkeyStatus(const QString &id, bool ok, const QString &message);
    // 录制出来的组合键先过一遍规范形（修饰键顺序固定、字母大写）。
    // 返回空串 = 不可用（没有修饰键，或只有修饰键本身）。
    Q_INVOKABLE QString normalizeHotkey(const QString &combo);

    QString uiLanguage() const { return m_uiLanguage; }
    void setUiLanguage(const QString &code);
    QString systemLanguage() const;

    QVariantMap llmConfig() const { return m_llmConfig; }
    void setLlmConfig(const QVariantMap &config);

    QVariantList webProviders() const;

    QStringList localModels() const;
    bool localGpuAvailable() const;
    bool localCpuAvailable() const;
    int localPort() const;
    qint64 localPid() const;
    QString localBackend() const;
    qint64 localStartupMs() const;
    QString localModelPath() const;
    QString localLogTail() const;
    QString localLogPath() const;

    QVariantList cloudModels() const { return m_cloudModels; }
    bool cloudBusy() const { return m_cloudBusy; }
    QString cloudProbeResult() const { return m_cloudProbeResult; }

    QString webProviderId() const { return m_webProviderId; }
    void setWebProviderId(const QString &id);

    QString localModel() const { return m_localModel; }
    void setLocalModel(const QString &fileName);

    bool localUseGpu() const { return m_localUseGpu; }
    void setLocalUseGpu(bool enabled);

    int localContextSize() const { return m_localContextSize; }
    void setLocalContextSize(int tokens);
    int localThreads() const { return m_localThreads; }
    void setLocalThreads(int threads);
    int localGpuLayers() const { return m_localGpuLayers; }
    void setLocalGpuLayers(int layers);
    QString localExtraArgs() const { return m_localExtraArgs; }
    void setLocalExtraArgs(const QString &args);

    bool localStopOnSwitch() const { return m_localStopOnSwitch; }
    void setLocalStopOnSwitch(bool on);
    bool localPreheat() const { return m_localPreheat; }
    void setLocalPreheat(bool on);
    bool localIdleStop() const { return m_localIdleStop; }
    void setLocalIdleStop(bool on);
    int localIdleMinutes() const { return m_localIdleMinutes; }
    void setLocalIdleMinutes(int minutes);

    // 本地 MT 已装语言对（含缺文件信息）。
    QVariantList mtPairs() const { return m_mtPairs; }
    void refreshMtPairs();

    bool mtPreload() const { return m_mtPreload; }
    void setMtPreload(bool on);
    int mtIdleMinutes() const { return m_mtIdleMinutes; }
    void setMtIdleMinutes(int minutes);
    int mtLoadedPairs() const;
    bool mtPacksInstalled() const;
    // 该语言对实际走的路线说明（"直接翻译" / "经英语中转" …）；不可用返回空串。
    // 引擎界面 id 的显示名（自动 / Web 在线 / 云端 LLM / 本地 LLM / 本地 MT）。
    // 名字只在 EngineIds.cpp 里定义一次，QML 各处下拉与状态条都调这里。
    Q_INVOKABLE QString engineName(const QString &engineId) const;

    Q_INVOKABLE QString mtRouteFor(const QString &src, const QString &dst) const;

    // 读：空串回落默认紫；写：非法颜色直接忽略，配色保持原样。
    QString accentColor() const;
    void setAccentColor(const QString &color);

    // ---- 设置页：翻译行为 ----
    bool cacheEnabled() const { return m_cacheEnabled; }
    void setCacheEnabled(bool on);
    bool sameLangHint() const { return m_sameLangHint; }
    void setSameLangHint(bool on);
    bool suspiciousLangWarn() const { return m_suspiciousLangWarn; }
    void setSuspiciousLangWarn(bool on);
    bool autoDetectSource() const { return m_autoDetectSource; }
    void setAutoDetectSource(bool on);

    // ---- 设置页：网络 ----
    QString proxyUrl() const { return m_proxyUrl; }
    void setProxyUrl(const QString &url);
    int httpTimeoutMs() const { return m_httpTimeoutMs; }
    void setHttpTimeoutMs(int ms);

    // ---- 设置页：桌面集成 ----
    bool autoStart() const { return m_autoStart; }
    void setAutoStart(bool on);

    bool captureAutoRun() const { return m_captureAutoRun; }
    void setCaptureAutoRun(bool on);
    bool captureAutoCopy() const { return m_captureAutoCopy; }
    void setCaptureAutoCopy(bool on);
    bool captureAutoHide() const { return m_captureAutoHide; }
    void setCaptureAutoHide(bool on);
    int overlayOpacity() const { return m_overlayOpacity; }
    void setOverlayOpacity(int percent);
    int overlayFontSize() const { return m_overlayFontSize; }
    void setOverlayFontSize(int px);
    bool overlayKeepOriginal() const { return m_overlayKeepOriginal; }
    void setOverlayKeepOriginal(bool on);
    int overlayZoom() const { return m_overlayZoom; }
    void setOverlayZoom(int times);
    bool overlayMagnifier() const { return m_overlayMagnifier; }
    void setOverlayMagnifier(bool on);
    QString magnifierCorner() const { return m_magnifierCorner; }
    void setMagnifierCorner(const QString &corner);
    int overlayDim() const { return m_overlayDim; }
    void setOverlayDim(int percent);
    bool overlayArrowNudge() const { return m_overlayArrowNudge; }
    void setOverlayArrowNudge(bool on);
    int overlayAutoHideDelay() const { return m_overlayAutoHideDelay; }
    void setOverlayAutoHideDelay(int seconds);
    int overlayBackdrop() const { return m_overlayBackdrop; }
    void setOverlayBackdrop(int percent);

    int liveIntervalMs() const { return m_liveIntervalMs; }
    void setLiveIntervalMs(int ms);
    bool liveOnlyOnChange() const { return m_liveOnlyOnChange; }
    void setLiveOnlyOnChange(bool on);

    bool historyEnabled() const { return m_historyEnabled; }
    void setHistoryEnabled(bool on);
    // 只读：设置里存的空串会解析成 <AppData>/studious/history；解析结果不写回设置，
    // 这样换机器、改 assetRoot 之后目录还能跟着走。
    QString historyDir() const;
    bool minimizeToTray() const { return m_minimizeToTray; }
    void setMinimizeToTray(bool on);
    bool miniAlwaysOnTop() const { return m_miniAlwaysOnTop; }
    void setMiniAlwaysOnTop(bool on);
    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool on);
    bool singleInstance() const { return m_singleInstance; }
    void setSingleInstance(bool on);
    bool trayAvailable();

    // 第二个实例被拒（已有进程在跑）时为 true，main() 据此直接退出。
    bool duplicateInstance() const { return m_duplicateInstance; }

public slots:
    // ---- 翻译动作 ----
    void translate();
    void cancel();
    void clearSource();
    void copyTranslated();
    void pasteFromClipboard();

    // ---- 状态刷新 ----
    void refreshEngineStatuses();
    void refreshCacheStats();
    void clearCache();
    void resetCacheStats();

    // 重新扫描清单 + 磁盘，刷新 assets 属性。
    void refreshAssets();

    // ---- 资产操作 ----
    // 下载在工作线程跑（Downloader 内部同步）；进度经 assetProgress 回传。
    Q_INVOKABLE void downloadAsset(const QString &id);
    Q_INVOKABLE void cancelAssetDownload(const QString &id);
    Q_INVOKABLE void uninstallAsset(const QString &id);

    // 复制任意文本到剪贴板（许可证正文、资产路径等）。
    Q_INVOKABLE void copyText(const QString &text);

    // 重启本程序（界面语言这类要重启才生效的设置，改完直接调它）。
    Q_INVOKABLE void restartApplication();

    // 重新读一次本地引擎实况（端口 / pid / 日志尾部都要重新取）。
    Q_INVOKABLE void refreshLocalState();

    // 截屏翻译自动存档：把一次框选的结果落到记录库（开启「保存翻译历史」时才写）。
    // originals 是 OCR 出来的逐行原文，blocks 是浮层上的逐块数据（原文 + 译文 + 框）。
    Q_INVOKABLE bool saveCaptureRecord(const QString &source,
                                       const QString &imagePath,
                                       const QRect &region,
                                       const QVariantList &originals,
                                       const QVariantList &blocks);

    // 三个来源各一个库（capture.db / mini.db / live.db），这里返回合并后按时间倒序的列表。
    // 每项：kind / id / source / srcLang / dstLangs / engine / imagePath / region /
    //       originalText / translatedText / createdAt（ISO 8601 字符串）。
    Q_INVOKABLE QVariantList historyRecords(int limit = 100);
    // 三个库的行数之和（界面上的「共 N 条」）。
    Q_INVOKABLE int recordCount();

    // ---- 实时翻译的会话分组 ----
    // 一次会话（开启实时翻译 → 关闭）算一组：中间每轮刷新只往缓冲里追加，
    // 关闭时合成**一条**记录写进 live.db。会话没开始就调 append 会被忽略。

    // 小窗翻译一条一存（跟截屏同一套字段，只是落进 mini.db）。
    Q_INVOKABLE bool saveMiniRecord(const QVariantList &originals,
                                    const QVariantList &blocks,
                                    const QString &engineLabel = QString());

    // 实时翻译一次会话一条（浮层关掉时由 LiveSession 的 sessionFinished 调）。
    // 和上面的 begin/append/finish 三件套是两条路：那条给"逐轮刷新"用，这条给
    // "会话结束时已经有完整结果"的场合留的入口。
    Q_INVOKABLE bool saveLiveRecord(const QVariantList &originals,
                                    const QVariantList &blocks,
                                    const QString &engineLabel = QString());

private:
    // 按来源取记录库（kind：capture / mini / live，其它值当 capture）。
    // 三个库各开一条连接、各有自己的文件 —— 这样清理或导出某一种不会牵动另一种。
    TranslatorRecords *recordsFor(const QString &kind);
    // 一次翻译结果 → 一条记录并写库；historyEnabled 关着的时候直接返回 false。
    bool persistRecord(const QString &kind,
                       const QString &imagePath,
                       const QRect &region,
                       const QVariantList &originals,
                       const QVariantList &blocks,
                       const QString &engineLabel = QString());

    // 重启空闲计时器。本地请求进来时调一次；localIdleStop 关着就什么都不做。
    void touchLocalIdle();
    // 本地 MT 的空闲释放计时器（0 分钟 = 不自动释放）。
    void touchMtIdle();
    // 停掉本地服务并按需给一句状态（reason 为空 = 安静地停）。
    // 跟 QML 的 stopLocalServer() 分开：那个是用户点「停止」，这个是我们自己回收。
    void shutdownLocalEngine(const QString &reason);

    // 把代理 / 超时写进环境变量。后端的 QtHttpClient 只在构造时读这些值，
    // 所以"改了立刻生效"的实现方式就是：改完环境变量，下一次新建客户端自然吃到。
    void applyNetworkEnvironment();
    // 托盘按需装上（最小化到托盘或关闭到托盘任一打开就需要它）。
    void syncTrayState();

public:
    // 去网关拉模型列表（GET {baseUrl}/models）。
    Q_INVOKABLE void fetchCloudModels();
    // 探测网关的「思考可关闭」能力，结果写进 cloudProbeResult。
    Q_INVOKABLE void probeCloudReasoning();
    // 语言检测，返回内部语言码（"en" / "zh" …），失败返回空串。
    Q_INVOKABLE QString detectLanguage(const QString &text);

    // 把 QPalette 同步成当前主题。
    // QML 侧是配色唯一来源，这里只负责把它转交给 Qt ——
    // 系统绘制的部分（文件对话框、滚动条、右键菜单、ToolTip）吃的是 QPalette，
    // 不跟着 Theme 走就会出现「浅色主题 + 深色对话框」这种割裂。
    // 入参键名：window / windowText / base / alternateBase / text / button /
    //           buttonText / highlight / highlightedText / toolTipBase /
    //           toolTipText / placeholderText
    Q_INVOKABLE void applyPalette(const QVariantMap &colors);

    // ---- 本地引擎 ----
    // Q_INVOKABLE 不能少：QML 的「启动 / 停止」按钮直接绑这两个名字，
    // 漏了就是"点了没反应"——QML 只在 stderr 里报一句 not a function。
    Q_INVOKABLE void startLocalServer();
    Q_INVOKABLE void stopLocalServer();

    // ---- 设置页：数据 ----
    // 用系统文件管理器打开一个路径（目录或文件所在目录）。空路径直接忽略。
    Q_INVOKABLE void openPath(const QString &path);
    // 打开（必要时创建）历史目录。跟 openPath 分开：这个目录归我们所有，
    // 用户第一次点就应该有东西弹出来，而不是"路径不存在"的报错。
    Q_INVOKABLE void openHistoryDir();
    // 删掉日志目录里 N 天前的 *.log，返回删掉的个数。
    Q_INVOKABLE int clearOldLogs(int days);
    // 清配置 + 清翻译缓存（保留已装资产），然后重启进程。
    Q_INVOKABLE void resetApplication();
    // 把整个安装根目录搬到 newRoot（同盘 rename，跨盘 copy+delete）。
    // 失败时保持原样并返回 false（原因写进 statusText）。
    Q_INVOKABLE bool migrateAssetRoot(const QString &newRoot);
    // 手动导入：让用户挑的文件先按清单 sha256 校验再装到 assets 里。
    // 在工作线程跑，结果经 errorRaised / statusText 回传。
    Q_INVOKABLE void importAssetFromFile(const QString &assetId, const QString &filePath);
    // 退出进程（托盘菜单「退出」与关闭行为都要用）。
    Q_INVOKABLE void quitApplication();

    // ---- 日志窗口 ----
    // 全文 / 最近 4000 行 / 文件路径。读的都是 logsink 的缓冲，不落盘、不做 IO。
    QString logText() const;
    QStringList logLines() const;
    QString logFilePath() const;
    // 清空内存缓冲（磁盘文件保留）。
    Q_INVOKABLE void clearLogs();
    // 全文写进剪贴板，成功后 statusText 报「已复制 N 行日志」。
    Q_INVOKABLE void copyLogs();
    // 用系统默认程序打开 studious.log / 打开它所在的目录（不存在就先建）。
    Q_INVOKABLE void openLogFile();
    Q_INVOKABLE void openLogDir();

signals:
    void sourceTextChanged();
    void translatedTextChanged();
    void srcLangChanged();
    void dstLangsChanged();
    void engineIdChanged();
    void runningChanged();
    void statusTextChanged();
    void statusWarningChanged();
    void engineStatusesChanged();
    void enginePriorityChanged();
    void cacheStatsChanged();
    void assetsChanged();
    void assetProgressChanged();
    // 工作线程 → 主线程的进度回传（不要直接改成员，跨线程不安全）
    void assetProgressTick(const QString &id, int percent);
    void blocksChanged();
    void showSourceChanged();
    void activeDstChanged();
    void themeModeChanged();
    void fontScaleChanged();
    void hotkeysChanged();
    void hotkeyStatusChanged();
    void uiLanguageChanged();
    void llmConfigChanged();
    void webProviderIdChanged();
    void localModelChanged();
    void localUseGpuChanged();
    void localLaunchChanged();
    void mtPairsChanged();
    void mtStateChanged();
    void localStateChanged();
    void cloudModelsChanged();
    void cloudBusyChanged();
    void cloudProbeResultChanged();
    void accentColorChanged();
    void prefsChanged();
    // 日志缓冲有更新（已按 200 ms 合并，见 AppBackend.cpp 构造函数）。
    void logTextChanged();

    // 单实例 ping / 托盘菜单要求把窗口叫出来；main() 负责接到 QML 窗口上。
    void restoreWindowRequested();

    // 供 QML 弹提示 / 打开对话框
    void errorRaised(const QString &title, const QString &detail);

private:
    void onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs);
    // startLocalServer() 只是「打个招呼 + 下一轮事件循环再真起」；
    // 阻塞式的 ensureStarted() 都在这里，状态文案才来得及上屏。
    void runLocalServerStart();
    void setStatus(const QString &text, bool isError);
    void setWarning(const QString &warning);
    void saveSettings();
    bool useLocalEngine() const;
    QString routerEngineId() const;

    // llama.cpp 运行时是否已装（用于把本地 LLM 的失败区分成
    // 「缺资产」= 运行时没下载 / 「未就绪」= 已下载但服务没起）。
    bool llamaRuntimeInstalled() const;

    studious::cache::TranslationCache *m_cache = nullptr;
    TranslateJob *m_job = nullptr;
    // 本地引擎实例见 QuickTranslate.cpp 的 sharedLocalEngine()：全进程只有一份，
    // 配置页、截图、实时、小窗共用同一个 llama-server。

    // 空闲自动停用。只在 localIdleStop 打开时走，其余时候是空对象。
    QTimer *m_localIdleTimer = nullptr;

    // 日志窗口的刷新合并器：见构造函数里的注释。
    // m_logNotifyPosted 在任意线程被读改（去重排队），所以是 atomic；
    // m_logDirty 只在主线程读写，普通 bool 就够。
    QTimer *m_logUiTimer = nullptr;
    std::atomic_bool m_logNotifyPosted{false};
    bool m_logDirty = false;

    QString m_sourceText;
    QString m_translatedText;
    QString m_srcLang;
    QStringList m_dstLangs;
    QString m_engineId;
    bool m_running = false;
    QString m_statusText;
    bool m_statusIsError = false;
    // 上一次已经弹过通知条的错误文本：setStatus 里用它去重，避免同一句话反复弹。
    QString m_lastNotifiedError;
    QString m_statusWarning;

    QStringList m_lastParagraphs;
    QString m_lastEngineLabel;
    bool m_lastCacheEnabled = false;
    // 发起这一单时的语言快照：结果回来时用户可能已经改了目标语言，
    // 用当前 m_dstLangs 拼装会把旧译文贴到新语言的栏位上。
    QStringList m_inflightDstLangs;
    QString m_inflightSrcLang;

    // 上一次翻译的原始结果，供 blocks() 按 tab 重新切片。
    QHash<QString, QStringList> m_byLang;
    bool m_lastPassthrough = false;
    bool m_showSource = true;
    QString m_activeDst;
    QString m_themeMode = QStringLiteral("system");
    int m_fontScale = 100;
    int m_readingFontSize = 13;
    QString m_hotkeyCapture;
    QString m_hotkeyMini;
    QString m_hotkeyLive;
    QString m_hotkeySelection;
    QString m_hotkeyHome;
    QVariantMap m_hotkeyStatus;
    QString m_uiLanguage = QStringLiteral("system");
    QVariantMap m_llmConfig;
    QString m_webProviderId;
    QString m_localModel;
    bool m_localUseGpu = true;
    int m_localContextSize = 2048;
    int m_localThreads = 0;
    int m_localGpuLayers = -1;
    QString m_localExtraArgs;
    bool m_localStopOnSwitch = false;
    bool m_localPreheat = false;
    // 已经排了一轮 startLocalServer()、还没进 runLocalServerStart()。
    // 自动拉起（截图 / 实时 / 小窗）会在很密的节奏里反复请求启动，
    // 没有这个标记就会排出一串 singleShot，每个都去 ensureStarted()。
    bool m_localStartQueued = false;
    bool m_localIdleStop = false;
    int m_localIdleMinutes = 10;
    QVariantList m_mtPairs;
    bool m_mtPreload = false;
    int m_mtIdleMinutes = 10;
    QTimer *m_mtIdleTimer = nullptr;
    QVariantList m_cloudModels;
    bool m_cloudBusy = false;
    QString m_cloudProbeResult;
    // 规范化的 "#RRGGBB"（大写）；空 = 用默认紫，由 accentColor() 兜底。
    QString m_accentColor;

    // ---- 设置页开关（对应 AppSettings 里的同名项）----
    bool m_cacheEnabled = true;
    bool m_sameLangHint = true;
    bool m_suspiciousLangWarn = true;
    bool m_autoDetectSource = true;
    QString m_proxyUrl;
    int m_httpTimeoutMs = 30000;
    bool m_autoStart = false;
    // 截屏翻译那一组（A 页）。默认值与 AppSettings 保持一致 —— 那边才是落盘口径。
    bool m_captureAutoRun = false;
    bool m_captureAutoCopy = false;
    bool m_captureAutoHide = true;
    int m_overlayOpacity = 96;
    int m_overlayFontSize = 13;
    bool m_overlayKeepOriginal = false;
    int m_overlayZoom = 3;
    bool m_overlayMagnifier = true;
    QString m_magnifierCorner = QStringLiteral("bottomRight");
    int m_overlayDim = 45;
    bool m_overlayArrowNudge = true;
    int m_overlayAutoHideDelay = 0;
    int m_overlayBackdrop = 70;
    int m_liveIntervalMs = 600;
    bool m_liveOnlyOnChange = true;
    bool m_historyEnabled = false;
    bool m_minimizeToTray = false;
    bool m_miniAlwaysOnTop = true;
    bool m_closeToTray = false;
    bool m_singleInstance = true;
    bool m_duplicateInstance = false;

    // 托盘 / 开机自启 / 单实例。放在 AppBackend 里而不是 main()：
    // 三者的开关都在设置页，改完立刻要生效，需要一个活着的 QObject 承载体。
    studious::platform::Desktop m_desktop;

    // 记录库：三种来源各一个库、各一条连接，懒打开（第一次保存/查询时才连）。
    // 分开存是为了「截屏」「小窗」「实时」能各自清理 / 各自导出，互不牵连。
    std::unique_ptr<TranslatorRecords> m_recordsCapture;
    std::unique_ptr<TranslatorRecords> m_recordsMini;
    std::unique_ptr<TranslatorRecords> m_recordsLive;

    // 实时翻译的会话缓冲：begin → append* → finish 合成一条记录。

    QVariantList m_engineStatuses;
    QStringList m_enginePriority;
    QVariantList m_assets;
    QString m_assetInstallRoot;
    QVariantMap m_assetProgress;
    // 每个下载任务的取消标志（工作线程读，主线程写）。
    QHash<QString, std::shared_ptr<std::atomic_bool>> m_assetCancel;
};

} // namespace studious::ui
