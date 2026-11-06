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

#include <studious/contract/Types.h>

class TranslateJob;
struct TranslateJobResult;

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

    // ---- 翻译页状态 ----
    Q_PROPERTY(QString sourceText READ sourceText WRITE setSourceText NOTIFY sourceTextChanged)
    Q_PROPERTY(QString translatedText READ translatedText NOTIFY translatedTextChanged)
    Q_PROPERTY(QString srcLang READ srcLang WRITE setSrcLang NOTIFY srcLangChanged)
    Q_PROPERTY(QStringList dstLangs READ dstLangs WRITE setDstLangs NOTIFY dstLangsChanged)
    Q_PROPERTY(QString engineId READ engineId WRITE setEngineId NOTIFY engineIdChanged)
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
    Q_PROPERTY(bool showSource READ showSource WRITE setShowSource NOTIFY showSourceChanged)

    // ---- 译文分块（供 QML 逐段渲染：tab 切换 + passthrough 徽标）----
    // 每项：{ index:int, source:QString, text:QString, passthrough:bool, empty:bool }
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY blocksChanged)
    // 当前译文栏正在看的那个目标语言（多目标时对应 tab）
    Q_PROPERTY(QString activeDst READ activeDst WRITE setActiveDst NOTIFY activeDstChanged)

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

    // 本地 MT 已装的语言对：来自后端 installedPairs() + missingFiles()。
    // 每项：pair / path / missing（QStringList）/ usable（bool）
    Q_PROPERTY(QVariantList mtPairs READ mtPairs NOTIFY mtPairsChanged)

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

    // ---- 强调色（seed）："#RRGGBB"，空 = 默认紫 #6750A4，落 QSettings ----
    Q_PROPERTY(QString accentColor READ accentColor WRITE setAccentColor NOTIFY accentColorChanged)

public:
    explicit AppBackend(studious::cache::TranslationCache *cache, QObject *parent = nullptr);
    ~AppBackend() override;

    QString sourceText() const { return m_sourceText; }
    void setSourceText(const QString &text);

    QString translatedText() const { return m_translatedText; }

    QString srcLang() const { return m_srcLang; }
    void setSrcLang(const QString &code);

    QStringList dstLangs() const { return m_dstLangs; }
    void setDstLangs(const QStringList &codes);

    QString engineId() const { return m_engineId; }
    void setEngineId(const QString &id);

    bool running() const { return m_running; }
    QString statusText() const { return m_statusText; }
    bool statusIsError() const { return m_statusIsError; }
    QString statusWarning() const { return m_statusWarning; }

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
    bool showSource() const { return m_showSource; }
    void setShowSource(bool on);

    QVariantList blocks() const;
    QString activeDst() const { return m_activeDst; }
    void setActiveDst(const QString &code);

    QString themeMode() const { return m_themeMode; }
    void setThemeMode(const QString &mode);

    int fontScale() const { return m_fontScale; }
    void setFontScale(int percent);

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

    // 本地 MT 已装语言对（含缺文件信息）。
    QVariantList mtPairs() const { return m_mtPairs; }
    void refreshMtPairs();
    // 该语言对实际走的路线说明（"直接翻译" / "经英语中转" …）；不可用返回空串。
    Q_INVOKABLE QString mtRouteFor(const QString &src, const QString &dst) const;

    // 读：空串回落默认紫；写：非法颜色直接忽略，配色保持原样。
    QString accentColor() const;
    void setAccentColor(const QString &color);

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
    void startLocalServer();
    void stopLocalServer();

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
    void showSourceChanged();
    void blocksChanged();
    void activeDstChanged();
    void themeModeChanged();
    void fontScaleChanged();
    void uiLanguageChanged();
    void llmConfigChanged();
    void webProviderIdChanged();
    void localModelChanged();
    void localUseGpuChanged();
    void localLaunchChanged();
    void mtPairsChanged();
    void localStateChanged();
    void cloudModelsChanged();
    void cloudBusyChanged();
    void cloudProbeResultChanged();
    void accentColorChanged();

    // 供 QML 弹提示 / 打开对话框
    void errorRaised(const QString &title, const QString &detail);

private:
    void onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs);
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
    LocalEngine m_localEngine;   // 懒启动：第一次用本地引擎时才拉 llama-server

    QString m_sourceText;
    QString m_translatedText;
    QString m_srcLang;
    QStringList m_dstLangs;
    QString m_engineId;
    bool m_running = false;
    QString m_statusText;
    bool m_statusIsError = false;
    QString m_statusWarning;

    QStringList m_lastParagraphs;
    QString m_lastEngineLabel;
    bool m_lastCacheEnabled = false;
    bool m_showSource = true;

    // 上一次翻译的原始结果，供 blocks() 按 tab 重新切片。
    QHash<QString, QStringList> m_byLang;
    bool m_lastPassthrough = false;
    QString m_activeDst;
    QString m_themeMode = QStringLiteral("system");
    int m_fontScale = 100;
    QString m_uiLanguage = QStringLiteral("system");
    QVariantMap m_llmConfig;
    QString m_webProviderId;
    QString m_localModel;
    bool m_localUseGpu = true;
    int m_localContextSize = 2048;
    int m_localThreads = 0;
    int m_localGpuLayers = -1;
    QString m_localExtraArgs;
    QVariantList m_mtPairs;
    QVariantList m_cloudModels;
    bool m_cloudBusy = false;
    QString m_cloudProbeResult;
    // 规范化的 "#RRGGBB"（大写）；空 = 用默认紫，由 accentColor() 兜底。
    QString m_accentColor;

    QVariantList m_engineStatuses;
    QStringList m_enginePriority;
    QVariantList m_assets;
    QString m_assetInstallRoot;
    QVariantMap m_assetProgress;
    // 每个下载任务的取消标志（工作线程读，主线程写）。
    QHash<QString, std::shared_ptr<std::atomic_bool>> m_assetCancel;
};

} // namespace studious::ui
