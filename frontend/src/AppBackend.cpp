#include "AppBackend.h"

#include "AppSettings.h"
#include "AssetFormat.h"
#include "LocalEngine.h"
#include "TranslateJob.h"
#include "i18n/Strings.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPalette>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>
#include <QtConcurrent>

#include <studious/Factory.h>
#include <studious/asset/AssetRegistry.h>
#include <studious/asset/Downloader.h>
#include <studious/asset/Manifest.h>
#include <studious/cache/TranslationCache.h>
#include <studious/contract/LanguageRegistry.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>

#include <algorithm>
#include <utility>

namespace studious::ui {
namespace {

// 目标语言在 QSettings 里以逗号分隔串保存（AppSettings::dstLang 仍是单个 QString）。
QStringList splitLangs(const QString &joined)
{
    QStringList out;
    const QStringList parts = joined.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty() && !out.contains(trimmed)) {
            out << trimmed;
        }
    }
    return out;
}

QString joinLangs(const QStringList &langs)
{
    return langs.join(QLatin1Char(','));
}

QVariantMap entry(const QString &code, const QString &label)
{
    QVariantMap map;
    map.insert(QStringLiteral("code"), code);
    map.insert(QStringLiteral("label"), label);
    return map;
}

// 资产归类。依据是清单里的 family 和 id 前缀，不靠猜：
//   · opus-mt 家族、mt.opus.* 语言对、onnxruntime 运行时 → 本地 MT
//   · 其余（qwen 模型、llama.cpp 运行时）→ LLM
QString assetCategory(const QString &id, const QString &family)
{
    if (family == QLatin1String("opus-mt")
        || id.startsWith(QLatin1String("mt.opus."))
        || id.contains(QLatin1String("onnxruntime"))) {
        return QStringLiteral("mt");
    }
    return QStringLiteral("llm");
}

// 清单路径：三个候选按顺序找（见 refreshAssets 里的说明）。
QString findManifestPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(QStringLiteral("manifest.json")),
        QDir(appDir).filePath(QStringLiteral("../backend/asset/manifest.json")),
        QDir(appDir).filePath(QStringLiteral("../../backend/asset/manifest.json")),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return QString();
}

QString assetInstallRootPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("assets"));
}

// 解析仓库里这份示例清单（未签名、sha256 占位，所以放宽两项校验）。
bool parseBundledManifest(studious::asset::Manifest *out)
{
    const QString path = findManifestPath();
    if (path.isEmpty()) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    studious::asset::ManifestVerifyOptions opts;
    opts.requireSignature = false;
    opts.allowMissingSha256 = true;
    opts.maxSchemaVersion = 1;

    const studious::asset::ManifestLoadResult parsed =
        studious::asset::parseManifest(bytes, opts);
    if (!parsed.ok) {
        return false;
    }

    *out = parsed.manifest;
    return true;
}

} // namespace

AppBackend::AppBackend(studious::cache::TranslationCache *cache, QObject *parent)
    : QObject(parent)
    , m_cache(cache)
    , m_job(new TranslateJob(this))
{
    AppSettings settings = AppSettings::load();

    m_srcLang = settings.srcLang.isEmpty() ? QStringLiteral("auto") : settings.srcLang;
    m_dstLangs = splitLangs(settings.dstLang);
    if (m_dstLangs.isEmpty()) {
        m_dstLangs = {QStringLiteral("zh")};
    }

    m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
    m_themeMode = settings.themeMode;
    m_fontScale = settings.fontScale;
    // 设置里空串 = 跟随系统，界面用 "system" 表达。
    m_uiLanguage = settings.uiLanguage.isEmpty() ? QStringLiteral("system")
                                                 : settings.uiLanguage;
    // load() 已经过滤过非法值，这里拿到的要么是 "#RRGGBB"，要么是空（= 默认紫）。
    m_accentColor = settings.accentSeed;

    // 设置里存的是 Router 引擎 id，QML 用带语义的 id，这里做一次映射。
    if (settings.engineId == QLatin1String("web")) {
        m_engineId = QStringLiteral("web");
    } else if (settings.engineId == QLatin1String("local-mt")) {
        m_engineId = QStringLiteral("local-mt");
    } else if (settings.engineId == QLatin1String("llm")) {
        m_engineId = settings.useLocalEngine ? QStringLiteral("llm-local")
                                             : QStringLiteral("llm-cloud");
    }

    m_statusText = i18n::s().common.ready;

    // 首次启动的示例原文：三段，段间用空行分隔（splitParagraphs 按空行切段）。
    // 用户输入不会覆盖它 —— 这里只做初始值。
    m_sourceText = QStringLiteral("Hello, world!\n"
                                  "\n"
                                  "This is a simple text translation test.\n"
                                  "\n"
                                  "The quick brown fox jumps over the lazy dog.");

    connect(m_job, &TranslateJob::finished, this, &AppBackend::onTranslationFinished);

    refreshAssets();

    // 首屏就要有状态：构造函数里先探一次，别等用户去动语言选择器。
    refreshEngineStatuses();
    refreshCacheStats();

    // 工作线程发来的下载进度，排队回主线程再改成员（成员只归主线程所有）。
    connect(this,
            &AppBackend::assetProgressTick,
            this,
            [this](const QString &id, int percent) {
                QVariantMap updated = m_assetProgress;
                updated.insert(id, percent);
                m_assetProgress = updated;
                emit assetProgressChanged();
            },
            Qt::QueuedConnection);
}

AppBackend::~AppBackend() = default;

// ---------------------------------------------------------------- 属性 setter

void AppBackend::setSourceText(const QString &text)
{
    if (m_sourceText == text) {
        return;
    }
    m_sourceText = text;
    emit sourceTextChanged();
}

void AppBackend::setSrcLang(const QString &code)
{
    const QString normalized = code.isEmpty() ? QStringLiteral("auto") : code;
    if (m_srcLang == normalized) {
        return;
    }
    m_srcLang = normalized;
    saveSettings();
    emit srcLangChanged();
    refreshEngineStatuses();
}

void AppBackend::setDstLangs(const QStringList &codes)
{
    QStringList next = codes;
    if (next.isEmpty()) {
        next = {QStringLiteral("zh")};
    }
    if (m_dstLangs == next) {
        return;
    }
    m_dstLangs = next;
    // 当前 tab 若已被取消勾选，回落到第一个目标语言。
    if (!m_dstLangs.contains(m_activeDst)) {
        m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
        emit activeDstChanged();
    }
    saveSettings();
    emit dstLangsChanged();
    emit blocksChanged();
    refreshEngineStatuses();
}

void AppBackend::setEngineId(const QString &id)
{
    if (m_engineId == id) {
        return;
    }
    m_engineId = id;
    saveSettings();
    emit engineIdChanged();
    refreshEngineStatuses();
}

// ------------------------------------------------------------------ 只读清单

QVariantList AppBackend::languages() const
{
    QVariantList list;
    list.append(entry(QStringLiteral("auto"), i18n::s().translate.autoDetect));
    for (const QString &code : studious::core::LanguageRegistry::allInternal()) {
        if (code == QLatin1String("auto")) {
            continue;
        }
        list.append(entry(code, studious::core::LanguageRegistry::displayName(code)));
    }
    return list;
}

QVariantList AppBackend::engines() const
{
    QVariantList list;
    list.append(entry(QString(), i18n::s().translate.engineAuto));
    list.append(entry(QStringLiteral("web"), i18n::s().translate.engineWeb));
    list.append(entry(QStringLiteral("llm-cloud"), i18n::s().translate.engineLlmCloud));
    list.append(entry(QStringLiteral("llm-local"), i18n::s().translate.engineLlmLocal));
    list.append(entry(QStringLiteral("local-mt"), i18n::s().translate.engineMt));
    return list;
}

int AppBackend::cacheEntries() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->count()) : 0;
}

int AppBackend::cacheHits() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->stats().hits) : 0;
}

int AppBackend::cacheMisses() const
{
    return m_cache != nullptr ? static_cast<int>(m_cache->stats().misses) : 0;
}

int AppBackend::paragraphCount() const
{
    return splitParagraphs(m_sourceText).size();
}

int AppBackend::charCount() const
{
    return m_sourceText.length();
}

QString AppBackend::dstLabel() const
{
    QStringList names;
    names.reserve(m_dstLangs.size());
    for (const QString &code : m_dstLangs) {
        names << studious::core::LanguageRegistry::displayName(code);
    }
    return names.join(QStringLiteral(" + "));
}

void AppBackend::setShowSource(bool on)
{
    if (m_showSource == on) {
        return;
    }
    m_showSource = on;
    emit showSourceChanged();
}

QVariantList AppBackend::blocks() const
{
    QVariantList list;
    const QStringList translated = m_byLang.value(m_activeDst);
    const int count = m_lastParagraphs.isEmpty() ? translated.size() : m_lastParagraphs.size();
    list.reserve(count);

    for (int i = 0; i < count; ++i) {
        const QString text = translated.value(i);
        QVariantMap block;
        block.insert(QStringLiteral("index"), i);
        block.insert(QStringLiteral("source"), m_lastParagraphs.value(i));
        block.insert(QStringLiteral("text"), text);
        // 后端只给整次请求一个 passthrough 标志（源 == 目标时原样返回），
        // 所以逐段徽标也按整次请求的状态来。
        block.insert(QStringLiteral("passthrough"), m_lastPassthrough);
        block.insert(QStringLiteral("empty"), text.trimmed().isEmpty());
        list.append(block);
    }
    return list;
}

void AppBackend::setActiveDst(const QString &code)
{
    if (m_activeDst == code || !m_dstLangs.contains(code)) {
        return;
    }
    m_activeDst = code;
    emit activeDstChanged();
    emit blocksChanged();
}

void AppBackend::setThemeMode(const QString &mode)
{
    const QString next = (mode == QLatin1String("light") || mode == QLatin1String("dark"))
                             ? mode
                             : QStringLiteral("system");
    if (m_themeMode == next) {
        return;
    }
    m_themeMode = next;
    AppSettings settings = AppSettings::load();
    settings.themeMode = next;
    settings.save();
    emit themeModeChanged();
}

void AppBackend::setFontScale(int percent)
{    // 夹在 80~150，注册表被手改成离谱值时也不会把界面撑爆。
    const int next = std::clamp(percent, 80, 150);
    if (m_fontScale == next) {
        return;
    }
    m_fontScale = next;
    AppSettings settings = AppSettings::load();
    settings.fontScale = next;
    settings.save();
    emit fontScaleChanged();
}

QString AppBackend::accentColor() const
{
    // 空 = 用默认紫。写入口已规范化，这里只做兜底。
    return m_accentColor.isEmpty() ? QStringLiteral("#6750A4") : m_accentColor;
}

void AppBackend::setAccentColor(const QString &color)
{
    const QColor parsed(color.trimmed());
    // 非法值不改配色：否则注册表里一个手误就让整套主题哑掉。
    // QML 侧的输入框标红是它自己的事，后端永远保留最后一次有效值。
    if (!parsed.isValid()) {
        return;
    }
    // 统一成大写 "#RRGGBB"，QML 里比较选中态就不用管大小写。
    const QString normalized = parsed.name(QColor::HexRgb).toUpper();
    if (m_accentColor == normalized) {
        return;
    }
    m_accentColor = normalized;

    AppSettings settings = AppSettings::load();
    settings.accentSeed = m_accentColor;
    settings.save();

    emit accentColorChanged();
}

// -------------------------------------------------------------------- 翻译

bool AppBackend::useLocalEngine() const
{
    return m_engineId == QLatin1String("llm-local");
}

QString AppBackend::routerEngineId() const
{
    if (m_engineId == QLatin1String("web")) {
        return QStringLiteral("web");
    }
    if (m_engineId == QLatin1String("local-mt")) {
        return QStringLiteral("local-mt");
    }
    if (m_engineId == QLatin1String("llm-cloud") || m_engineId == QLatin1String("llm-local")) {
        return QStringLiteral("llm");
    }
    return QString();   // 自动
}

void AppBackend::translate()
{
    if (m_job->isRunning()) {
        return;
    }

    const QStringList paragraphs = splitParagraphs(m_sourceText);
    if (paragraphs.isEmpty()) {
        setStatus(i18n::s().translate.emptyOriginal, true);
        return;
    }

    AppSettings settings = AppSettings::load();

    TranslateJobRequest req;
    req.paragraphs = paragraphs;
    req.srcLang = m_srcLang;
    req.dstLangs = m_dstLangs;
    req.engineId = routerEngineId();
    req.priority = settings.enginePriority;
    req.llm = settings.llm;
    req.cache = m_cache;
    req.cacheEnabled = settings.cacheEnabled;

    m_lastParagraphs = paragraphs;
    m_lastEngineLabel = m_engineId.isEmpty() ? i18n::s().translate.engineAuto : m_engineId;
    m_lastCacheEnabled = settings.cacheEnabled;

    if (useLocalEngine()) {
        // 本地引擎：先把 llama-server 拉起来（阻塞约 1 秒），再提交翻译。
        LocalEngineConfig cfg;
        cfg.modelPath = LocalEngine::modelsDir() + QLatin1Char('/') + settings.localModelFileName;
        cfg.preferGpu = settings.localUseGpu;

        QString error;
        if (!m_localEngine.ensureStarted(cfg, &error)) {
            emit errorRaised(i18n::s().config.localLlmSection, error);
            setStatus(error, true);
            return;
        }
        req.llm.baseUrl = m_localEngine.baseUrl();
        req.llm.apiKey = QStringLiteral("local");
        req.llm.model = m_localEngine.currentModel();
        req.llm.batchSize = 1;
        req.engineId = QStringLiteral("llm");
        m_lastEngineLabel = i18n::s().translate.engineLlmLocal;
    }

    m_running = true;
    emit runningChanged();
    setWarning(QString());
    setStatus(i18n::s().translate.running, false);
    m_job->start(req);
}

void AppBackend::cancel()
{
    // TranslateJob 没有 cancel 接口；这里只把界面恢复成可再点。
    m_running = false;
    emit runningChanged();
    setStatus(i18n::s().translate.cancelHint, false);
}

void AppBackend::clearSource()
{
    setSourceText(QString());
    m_translatedText.clear();
    emit translatedTextChanged();
}

void AppBackend::copyTranslated()
{
    QGuiApplication::clipboard()->setText(m_translatedText);
}

void AppBackend::pasteFromClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty()) {
        setSourceText(text);
    }
}

void AppBackend::onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs)
{
    m_running = false;
    emit runningChanged();

    if (!result.outcome.ok) {
        m_translatedText.clear();
        emit translatedTextChanged();
        setStatus(result.outcome.error, true);
        refreshEngineStatuses();
        return;
    }

    QStringList sections;
    sections.reserve(m_dstLangs.size());
    for (const QString &dst : m_dstLangs) {
        const QStringList paragraphs = result.outcome.byLang.value(dst);
        const int count = paragraphs.isEmpty() ? m_lastParagraphs.size() : paragraphs.size();
        QStringList blocks;
        blocks.reserve(count);
        for (int i = 0; i < count; ++i) {
            blocks << paragraphs.value(i);
        }
        const QString block = blocks.join(QStringLiteral("\n\n"));
        if (m_dstLangs.size() > 1) {
            sections << QStringLiteral("━━ %1 ━━\n%2")
                            .arg(studious::core::LanguageRegistry::displayName(dst), block);
        } else {
            sections << block;
        }
    }
    m_translatedText = sections.join(QStringLiteral("\n\n"));
    emit translatedTextChanged();

    // 保住结构化结果：译文栏的 tab 与逐段徽标都从这里重新切片。
    m_byLang = result.outcome.byLang;
    m_lastPassthrough = result.outcome.passthrough;
    if (!m_dstLangs.contains(m_activeDst)) {
        m_activeDst = m_dstLangs.value(0, QStringLiteral("zh"));
        emit activeDstChanged();
    }
    emit blocksChanged();

    setStatus(QStringLiteral("%1 · %2 ms").arg(m_lastEngineLabel).arg(elapsedMs), false);

    if (!result.outcome.warning.isEmpty()) {
        setWarning(result.outcome.warning);
    } else {
        setWarning(QString());
    }

    refreshCacheStats();
    refreshEngineStatuses();
}

// -------------------------------------------------------------------- 状态

void AppBackend::refreshEngineStatuses()
{
    AppSettings settings = AppSettings::load();

    studious::net::QtHttpClient http;
    auto translators = studious::factory::makeDefaultTranslators(http, settings.llm, {});
    const QStringList priority =
        settings.enginePriority.isEmpty()
            ? QStringList{QStringLiteral("mock"), QStringLiteral("web"),
                          QStringLiteral("llm"), QStringLiteral("local-mt")}
            : settings.enginePriority;
    const studious::router::TranslatorRouter router(std::move(translators), priority);

    const QString dst = m_dstLangs.value(0, QStringLiteral("zh"));
    QVariantList list;

    for (const auto &row : router.status(m_srcLang, dst)) {
        if (row.first == QLatin1String("mock")) {
            continue;
        }
        QVariantMap item;
        item.insert(QStringLiteral("id"), row.first);
        item.insert(QStringLiteral("ready"), row.second.ready);
        item.insert(QStringLiteral("detail"), row.second.reason);
        list.append(item);
    }

    // 本地 LLM 不在 Router 里，单独按 LocalEngine 的实况补一条。
    QVariantMap local;
    local.insert(QStringLiteral("id"), QStringLiteral("llm-local"));
    local.insert(QStringLiteral("ready"), m_localEngine.isRunning());
    local.insert(QStringLiteral("detail"),
                 m_localEngine.isRunning() ? m_localEngine.baseUrl()
                                           : i18n::s().common.notReady);
    list.append(local);

    m_engineStatuses = list;
    emit engineStatusesChanged();
}

void AppBackend::refreshCacheStats()
{
    emit cacheStatsChanged();
}

void AppBackend::clearCache()
{
    if (m_cache != nullptr) {
        m_cache->clear();
    }
    refreshCacheStats();
}

void AppBackend::resetCacheStats()
{
    if (m_cache != nullptr) {
        m_cache->resetStats();
    }
    refreshCacheStats();
}

void AppBackend::startLocalServer()
{
    AppSettings settings = AppSettings::load();
    LocalEngineConfig cfg;
    cfg.modelPath = LocalEngine::modelsDir() + QLatin1Char('/') + settings.localModelFileName;
    cfg.preferGpu = settings.localUseGpu;

    QString error;
    if (!m_localEngine.ensureStarted(cfg, &error)) {
        emit errorRaised(i18n::s().config.localLlmSection, error);
    }
    refreshEngineStatuses();
}

void AppBackend::stopLocalServer()
{
    m_localEngine.shutdown();
    refreshEngineStatuses();
}

// -------------------------------------------------------------------- 私有

void AppBackend::setStatus(const QString &text, bool isError)
{
    if (m_statusText == text && m_statusIsError == isError) {
        return;
    }
    m_statusText = text;
    m_statusIsError = isError;
    emit statusTextChanged();
}

void AppBackend::setWarning(const QString &warning)
{
    if (m_statusWarning == warning) {
        return;
    }
    m_statusWarning = warning;
    emit statusWarningChanged();
}

void AppBackend::saveSettings()
{
    AppSettings settings = AppSettings::load();
    settings.srcLang = m_srcLang;
    settings.dstLang = joinLangs(m_dstLangs);
    settings.engineId = routerEngineId();
    settings.useLocalEngine = useLocalEngine();
    settings.save();
}

void AppBackend::refreshAssets()
{
    m_assets.clear();

    const QString installRoot = assetInstallRootPath();
    m_assetInstallRoot = installRoot;

    studious::asset::Manifest manifest;
    if (!parseBundledManifest(&manifest)) {
        emit assetsChanged();
        return;
    }

    studious::asset::AssetRegistry registry(installRoot, manifest);
    registry.refresh();

    const QVector<studious::asset::AssetStatus> statuses = registry.listStatus();
    m_assets.reserve(statuses.size());

    for (const studious::asset::AssetStatus &status : statuses) {
        const studious::asset::AssetEntry *entry = manifest.find(status.id);
        if (entry == nullptr) {
            continue;
        }

        const bool placeholder = entry->isPlaceholder();
        const bool installed = status.state == studious::asset::AssetState::Installed;

        QVariantMap item;
        item.insert(QStringLiteral("id"), status.id);
        item.insert(QStringLiteral("category"), assetCategory(status.id, entry->family));
        item.insert(QStringLiteral("type"), entry->type);
        item.insert(QStringLiteral("installed"), installed);
        item.insert(QStringLiteral("placeholder"), placeholder);
        // 已装显示实际占盘，未装显示清单里写的下载体积。
        item.insert(QStringLiteral("sizeBytes"),
                    installed ? status.installedBytes : entry->size);
        item.insert(QStringLiteral("sizeText"),
                    studious::frontend::humanBytes(installed ? status.installedBytes : entry->size));
        item.insert(QStringLiteral("path"), status.installPath);
        // 只给状态码，文案由 QML 按界面语言出。
        // （不能在这儿用 i18n::s —— 这个命名空间里 i18n 指向前端文案表，
        //   而后端 i18n 被产品约定固定成英文，两边都不是界面要的中文。）
        switch (status.state) {
        case studious::asset::AssetState::Installed:
            item.insert(QStringLiteral("state"), QStringLiteral("installed"));
            break;
        case studious::asset::AssetState::Corrupted:
            item.insert(QStringLiteral("state"), QStringLiteral("corrupted"));
            break;
        case studious::asset::AssetState::NotInstalled:
        default:
            item.insert(QStringLiteral("state"), QStringLiteral("missing"));
            break;
        }
        item.insert(QStringLiteral("note"), entry->note);
        m_assets.append(item);
    }

    emit assetsChanged();
}

void AppBackend::downloadAsset(const QString &id)
{
    if (id.isEmpty() || m_assetProgress.contains(id)) {
        return;   // 已经在下了
    }

    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_assetCancel.insert(id, cancelFlag);

    QVariantMap started = m_assetProgress;
    started.insert(id, 0);
    m_assetProgress = started;
    emit assetProgressChanged();

    auto *watcher = new QFutureWatcher<studious::asset::DownloadResult>(this);
    connect(watcher,
            &QFutureWatcher<studious::asset::DownloadResult>::finished,
            this,
            [this, watcher, id] {
                const studious::asset::DownloadResult result = watcher->result();
                watcher->deleteLater();

                m_assetCancel.remove(id);
                QVariantMap done = m_assetProgress;
                done.remove(id);
                m_assetProgress = done;
                emit assetProgressChanged();

                if (!result.ok) {
                    emit errorRaised(i18n::s().settings.assets, result.error);
                }
                refreshAssets();
            });

    // 工作线程内构造 QtHttpClient / Registry / Downloader：
    // QNetworkAccessManager 是线程亲和的，绝不能跨线程共享。
    watcher->setFuture(QtConcurrent::run([this, id, cancelFlag]() {
        studious::asset::DownloadResult result;

        studious::asset::Manifest manifest;
        if (!parseBundledManifest(&manifest)) {
            result.ok = false;
            result.error = QStringLiteral("找不到或无法解析资产清单");
            return result;
        }

        studious::net::QtHttpClient http;
        studious::asset::AssetRegistry registry(assetInstallRootPath(), manifest);
        studious::asset::Downloader downloader(http, registry);

        const auto onProgress = [this, id](const studious::asset::DownloadProgress &pr) {
            const int percent = pr.total > 0
                                    ? static_cast<int>(pr.received * 100 / pr.total)
                                    : -1;
            emit assetProgressTick(id, percent);
        };

        return downloader.download(id, onProgress, cancelFlag.get());
    }));
}

void AppBackend::cancelAssetDownload(const QString &id)
{
    const auto it = m_assetCancel.find(id);
    if (it != m_assetCancel.end() && it.value()) {
        it.value()->store(true);
    }
}

void AppBackend::uninstallAsset(const QString &id)
{    studious::asset::Manifest manifest;
    if (!parseBundledManifest(&manifest)) {
        emit errorRaised(i18n::s().settings.assets,
                         QStringLiteral("找不到或无法解析资产清单"));
        return;
    }

    studious::asset::AssetRegistry registry(assetInstallRootPath(), manifest);
    registry.refresh();

    QString error;
    if (!registry.uninstall(id, &error)) {
        emit errorRaised(i18n::s().settings.assets, error);
    }
    refreshAssets();
}

QString AppBackend::systemLanguage() const
{
    return studious::ui::i18n::detectSystemLanguage() == studious::ui::i18n::Lang::Chinese
               ? QStringLiteral("zh")
               : QStringLiteral("en");
}

void AppBackend::setUiLanguage(const QString &code)
{    const QString next = (code == QLatin1String("zh") || code == QLatin1String("en"))
                             ? code
                             : QStringLiteral("system");
    if (m_uiLanguage == next) {
        return;
    }

    m_uiLanguage = next;

    // 落盘：空串交给 AppSettings 表示「跟随系统」。
    AppSettings settings = AppSettings::load();
    settings.uiLanguage = (next == QLatin1String("system")) ? QString() : next;
    settings.save();

    // 这里**故意不热切换**语言。界面文案在 QML 里是写死的，
    // 而且 i18n 必须在加载 QML 之前定下来，热切只会切一半。
    // 由界面上弹出的「重启」按钮完成。
    emit uiLanguageChanged();
}

void AppBackend::restartApplication()
{
    // 用 detached 拉起自己再退出，这样父进程的退出不会带走新进程。
    // 不传参：避免把 --ui-shot 一类的调试开关带进新实例。
    const QString exe = QCoreApplication::applicationFilePath();
    if (QProcess::startDetached(exe, QStringList())) {
        QCoreApplication::quit();
    }
}

void AppBackend::copyText(const QString &text)
{    if (text.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(text);
}

void AppBackend::applyPalette(const QVariantMap &colors)
{
    if (colors.isEmpty()) {
        return;
    }

    const auto pick = [&colors](const char *key, const QColor &fallback) {
        const QVariant value = colors.value(QLatin1String(key));
        return value.canConvert<QColor>() && value.value<QColor>().isValid()
                   ? value.value<QColor>()
                   : fallback;
    };

    const QColor window = pick("window", QColor(Qt::white));
    const QColor windowText = pick("windowText", QColor(Qt::black));
    const QColor base = pick("base", window);
    const QColor text = pick("text", windowText);

    QPalette pal;
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, windowText);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, pick("alternateBase", base));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, pick("button", base));
    pal.setColor(QPalette::ButtonText, pick("buttonText", text));
    pal.setColor(QPalette::Highlight, pick("highlight", window));
    pal.setColor(QPalette::HighlightedText, pick("highlightedText", text));
    pal.setColor(QPalette::ToolTipBase, pick("toolTipBase", base));
    pal.setColor(QPalette::ToolTipText, pick("toolTipText", text));
    pal.setColor(QPalette::PlaceholderText, pick("placeholderText", windowText));

    // 禁用态跟着正文色走，避免系统默认的灰在深色主题下看不清。
    QColor disabled = text;
    disabled.setAlpha(110);
    pal.setColor(QPalette::Disabled, QPalette::Text, disabled);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    QGuiApplication::setPalette(pal);
}

} // namespace studious::ui
