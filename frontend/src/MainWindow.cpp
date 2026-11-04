#include "MainWindow.h"

#include "AssetDialog.h"
#include "SettingsDialog.h"
#include "TranslateJob.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <studious/contract/LanguageRegistry.h>
#include <studious/Factory.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>
#include <studious/translators/WebProvider.h>

#include <utility>

namespace {

const char *const kSampleText =
    "Hello, world!\n"
    "\n"
    "This is a simple text translation test.\n"
    "\n"
    "The quick brown fox jumps over the lazy dog.\n";

const char *const kStatusOkStyle = "";
const char *const kStatusErrorStyle = "color: #c0392b; font-weight: 600;";

// 「自动」那一项的说明文案。
const char *const kAutoProviderNote = "按后端默认优先级依次尝试：Google → MyMemory";

// 临时装配一套组件只为查状态：status() 不联网，但 QtHttpClient 仍必须在当前线程构造。
QString buildEngineStatusText(const QString &src,
                              const QString &dst,
                              const studious::config::LlmConfig &llm,
                              const QStringList &webProviders,
                              QString *tooltip)
{
    studious::net::QtHttpClient http;                          // 主线程内构造，与调用同线程
    auto translators = studious::factory::makeDefaultTranslators(http, llm, webProviders);
    const studious::router::TranslatorRouter router(std::move(translators));

    const auto rows = router.status(src, dst);

    QStringList parts;
    QStringList tips;
    for (const auto &row : rows) {
        parts << QStringLiteral("%1:%2").arg(row.first,
                                             row.second.ready ? QStringLiteral("就绪")
                                                              : QStringLiteral("未就绪"));
        const QString reason = row.second.reason.isEmpty() ? QStringLiteral("-") : row.second.reason;
        tips << QStringLiteral("%1  %2  %3").arg(row.first,
                                                 row.second.ready ? QStringLiteral("[就绪]")
                                                                  : QStringLiteral("[未就绪]"),
                                                 reason);
    }

    if (tooltip != nullptr) {
        *tooltip = tips.join(QLatin1Char('\n'));
    }
    return parts.join(QStringLiteral("  "));
}

// Qt 没有"禁用单个下拉项"的公开 API，两步走：
//   1. setItemData(idx, 0, Qt::UserRole - 1)：让默认风格把该项画成灰色（视觉）
//   2. QStandardItemModel::item(idx)->setEnabled(false)：真正禁止选中（行为）
// 只做第 1 步是假灰度——键盘仍能选到它。
void disableComboItem(QComboBox *box, int index)
{
    box->setItemData(index, 0, Qt::UserRole - 1);
    if (auto *model = qobject_cast<QStandardItemModel *>(box->model())) {
        if (QStandardItem *item = model->item(index)) {
            item->setEnabled(false);
        }
    }
}

bool isComboItemEnabled(QComboBox *box, int index)
{
    if (auto *model = qobject_cast<QStandardItemModel *>(box->model())) {
        if (const QStandardItem *item = model->item(index)) {
            return item->isEnabled();
        }
    }
    return true;
}

QComboBox *newLangBox() { return new QComboBox; }

} // namespace

MainWindow::MainWindow(studious::cache::TranslationCache *cache, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(AppSettings::load())
    , m_cache(cache)
    , m_job(new TranslateJob(this))
{
    setWindowTitle(QStringLiteral("Studious — 文本翻译测试"));
    resize(1000, 660);

    buildUi();
    populateLanguageBoxes();
    populateEngineBox();
    populateWebProviderBox();

    m_input->setPlainText(QString::fromUtf8(kSampleText));

    connect(m_translateButton, &QPushButton::clicked, this, &MainWindow::onTranslateClicked);
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::onOpenSettings);
    connect(m_assetsButton, &QPushButton::clicked, this, &MainWindow::onOpenAssets);
    connect(m_localCheck, &QCheckBox::toggled, this, &MainWindow::onLocalEngineToggled);
    connect(m_job, &TranslateJob::finished, this, &MainWindow::onTranslationFinished);

    // Ctrl+Alt+A 打开资产对话框：菜单栏没有「工具」这类入口，给个快捷键省得摸鼠标。
    auto *shortcutAssets = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+A")), this);
    connect(shortcutAssets, &QShortcut::activated, this, &MainWindow::onOpenAssets);

    // Ctrl+Enter / Ctrl+Return 都能触发。两个不同 keysym 各注册一次，
    // 不要给按钮再 setShortcut 同一个序列，否则会变成 ambiguous shortcut 谁都不触发。
    auto *shortcutReturn = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    connect(shortcutReturn, &QShortcut::activated, this, &MainWindow::onTranslateClicked);
    auto *shortcutEnter = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), this);
    connect(shortcutEnter, &QShortcut::activated, this, &MainWindow::onTranslateClicked);

    m_translateButton->setToolTip(QStringLiteral("快捷键：Ctrl+Enter"));

    setStatusOk(QStringLiteral("就绪"));
    refreshEngineStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 关窗即保存：语言 / 引擎 / 来源 / 缓存开关都随手记住，LLM 设置由对话框保存。
    m_settings.srcLang = currentSrcLang();
    m_settings.dstLang = currentDstLang();
    m_settings.engineId = currentEngine();
    m_settings.webProviderId = currentWebProviderId();
    m_settings.cacheEnabled = m_cacheCheck->isChecked();
    m_settings.useLocalEngine = m_localCheck->isChecked();
    m_settings.save();

    // llama-server 的回收放在析构（m_localEngine 的析构函数）里，不在这里做：
    // stop() 要等 terminate() 满 3 秒才 kill（控制台子进程收不到 WM_CLOSE），
    // 放在 closeEvent 里会让窗口在消失前先僵住 3 秒。放到析构 = 窗口先关，进程随后退出。

    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    // ---------- 第一行：语言 / 引擎 / 翻译 ----------
    auto *topBar = new QHBoxLayout;
    topBar->setSpacing(6);

    m_srcLangBox = newLangBox();
    m_dstLangBox = newLangBox();
    m_engineBox = newLangBox();
    // 引擎项可能带后缀（如 "llm（未配置）"），让下拉框自己长宽，避免文字被截成 "llm…配置）"。
    m_engineBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_translateButton = new QPushButton(QStringLiteral("翻译"));
    m_translateButton->setDefault(true);
    m_showSourceCheck = new QCheckBox(QStringLiteral("显示原文对照"));
    m_showSourceCheck->setChecked(true);

    topBar->addWidget(new QLabel(QStringLiteral("源语言")));
    topBar->addWidget(m_srcLangBox);
    topBar->addSpacing(8);
    topBar->addWidget(new QLabel(QStringLiteral("目标语言")));
    topBar->addWidget(m_dstLangBox);
    topBar->addSpacing(8);
    topBar->addWidget(new QLabel(QStringLiteral("引擎")));
    topBar->addWidget(m_engineBox);
    topBar->addStretch(1);
    topBar->addWidget(m_showSourceCheck);
    topBar->addWidget(m_translateButton);
    root->addLayout(topBar);

    // ---------- 第二行：Web 来源 + 设置 ----------
    auto *sourceBar = new QHBoxLayout;
    sourceBar->setSpacing(6);

    m_webProviderBox = newLangBox();
    m_webProviderBox->setMinimumWidth(240);
    m_settingsButton = new QPushButton(QStringLiteral("设置…"));
    m_settingsButton->setToolTip(QStringLiteral("LLM 网关 / API Key / 模型 / 采样参数 / 界面语言"));

    // 资产：模型 / 运行时的下载、删除、手动导入（对应清单 manifest.json）。
    m_assetsButton = new QPushButton(QStringLiteral("资产…"));
    m_assetsButton->setToolTip(QStringLiteral(
        "模型与运行时管理：下载 / 取消 / 删除 / 手动导入（快捷键 Ctrl+Alt+A）"));

    // 缓存开关：勾着 = 命中缓存的段落不再发请求（重复文本秒出）。
    m_cacheCheck = new QCheckBox(QStringLiteral("启用缓存"));
    m_cacheCheck->setChecked(m_settings.cacheEnabled);
    m_cacheCheck->setToolTip(
        QStringLiteral("命中缓存的句子不再请求翻译接口，重复文本秒出结果"));

    // 本地引擎开关：勾着 = 走本机 llama.cpp（离线、不花 API 费用）。
    // 资产（runtime / 模型）由「资产…」页下载，没装时第一次翻译会明确报缺哪个。
    m_localCheck = new QCheckBox(QStringLiteral("本地引擎"));
    m_localCheck->setChecked(m_settings.useLocalEngine);
    m_localCheck->setToolTip(QStringLiteral(
        "用本机 llama.cpp + 已下载的 GGUF 离线翻译，需要先在「资产」里下载 "
        "runtime.llama.cpu.win-x64 与 model.qwen2.5-0.5b-instruct.q4km"));

    sourceBar->addWidget(new QLabel(QStringLiteral("来源")));
    sourceBar->addWidget(m_webProviderBox);
    sourceBar->addStretch(1);
    sourceBar->addWidget(m_localCheck);
    sourceBar->addWidget(m_cacheCheck);
    sourceBar->addWidget(m_assetsButton);
    sourceBar->addWidget(m_settingsButton);
    root->addLayout(sourceBar);

    // ---------- 中间左右分栏 ----------
    m_input = new QPlainTextEdit;
    m_input->setPlaceholderText(QStringLiteral("在此输入待翻译文本，段落之间用空行分隔"));
    m_input->setTabChangesFocus(true);

    m_output = new QPlainTextEdit;
    m_output->setReadOnly(true);
    m_output->setPlaceholderText(QStringLiteral("译文会显示在这里"));

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);
    leftLayout->addWidget(new QLabel(QStringLiteral("原文")));
    leftLayout->addWidget(m_input, 1);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    rightLayout->addWidget(new QLabel(QStringLiteral("译文")));
    rightLayout->addWidget(m_output, 1);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    // ---------- 底部 ----------
    m_status = new QLabel(QStringLiteral("就绪"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setWordWrap(true);

    m_engineStatus = new QLabel;
    m_engineStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_engineStatus->setEnabled(false);   // 灰一点，不抢 status 的注意力

    root->addWidget(m_status);
    root->addWidget(m_engineStatus);

    setCentralWidget(central);
}

void MainWindow::populateLanguageBoxes()
{
    using studious::core::LanguageRegistry;

    m_srcLangBox->clear();
    m_srcLangBox->addItem(QStringLiteral("自动检测(auto)"), QStringLiteral("auto"));
    for (const QString &code : LanguageRegistry::allInternal()) {
        if (code == QLatin1String("auto")) {
            continue;
        }
        m_srcLangBox->addItem(
            QStringLiteral("%1 (%2)").arg(LanguageRegistry::displayName(code), code), code);
    }
    int srcIndex = m_srcLangBox->findData(m_settings.srcLang);
    m_srcLangBox->setCurrentIndex(srcIndex >= 0 ? srcIndex : 0);   // 回落「自动检测」

    m_dstLangBox->clear();
    for (const QString &code : LanguageRegistry::allInternal()) {
        if (code == QLatin1String("auto")) {
            continue;
        }
        m_dstLangBox->addItem(
            QStringLiteral("%1 (%2)").arg(LanguageRegistry::displayName(code), code), code);
    }
    int dstIndex = m_dstLangBox->findData(m_settings.dstLang);
    if (dstIndex < 0) {
        dstIndex = m_dstLangBox->findData(QStringLiteral("zh"));
    }
    m_dstLangBox->setCurrentIndex(dstIndex >= 0 ? dstIndex : 0);
}

void MainWindow::populateEngineBox()
{
    const QString keep = m_settings.engineId;   // 空 = 自动

    m_engineBox->clear();
    m_engineBox->addItem(QStringLiteral("自动"), QString());
    m_engineBox->setItemData(0,
                             QStringLiteral("按后端优先级自动选择：mock → web → llm → local-mt"),
                             Qt::ToolTipRole);

    // 勾了「本地引擎」时 llm 组件一定可用：本地模式不读云端 baseUrl / model，
    // 连接参数在提交任务前被覆盖成 127.0.0.1:<port>/v1。
    const bool llmConfigured = m_settings.llm.isValid() || m_settings.useLocalEngine;
    for (const QString &id : {QStringLiteral("web"), QStringLiteral("mock"),
                              QStringLiteral("llm"), QStringLiteral("local-mt")}) {
        // llm 未配置（baseUrl / model 为空）时置灰：就算选中它也只会拿到 isReady()==false。
        const bool usable = (id != QLatin1String("llm")) || llmConfigured;
        const QString text = usable ? id : QStringLiteral("%1（未配置）").arg(id);

        const int index = m_engineBox->count();
        m_engineBox->addItem(text, id);
        if (!usable) {
            disableComboItem(m_engineBox, index);
            m_engineBox->setItemData(
                index,
                QStringLiteral("LLM 未配置：点「设置…」填写 Base URL 与模型名后即可启用，"
                               "或勾选「本地引擎」用本机 llama.cpp"),
                Qt::ToolTipRole);
        }
    }

    int index = m_engineBox->findData(keep);
    if (index < 0 || !isComboItemEnabled(m_engineBox, index)) {
        index = 0;   // 存着旧引擎但已不可用 -> 回落「自动」
    }
    m_engineBox->setCurrentIndex(index);
}

void MainWindow::populateWebProviderBox()
{
    const QString keep = m_settings.webProviderId;   // 空 = 自动

    m_webProviderBox->clear();
    m_webProviderBox->addItem(QStringLiteral("自动（Google → MyMemory）"), QString());
    m_webProviderBox->setItemData(0, QString::fromUtf8(kAutoProviderNote), Qt::ToolTipRole);

    const QVector<studious::translate::WebProviderInfo> providers =
        studious::translate::listWebProviders();

    for (const studious::translate::WebProviderInfo &info : providers) {
        // requiresApiKey && !configured：后端已声明"需要 key 但没 key"，选它也没意义。
        const bool usable = !(info.requiresApiKey && !info.configured);

        const int index = m_webProviderBox->count();
        m_webProviderBox->addItem(
            usable ? info.displayName : QStringLiteral("%1（未配置）").arg(info.displayName),
            info.id);
        m_webProviderBox->setItemData(index, info.note, Qt::ToolTipRole);
        if (!usable) {
            disableComboItem(m_webProviderBox, index);
        }
    }

    int index = m_webProviderBox->findData(keep);
    if (index < 0 || !isComboItemEnabled(m_webProviderBox, index)) {
        index = 0;   // 存着已下架 / 未配置的来源 -> 回落「自动」
    }
    m_webProviderBox->setCurrentIndex(index);
}

QString MainWindow::currentSrcLang() const
{
    const QString code = m_srcLangBox->currentData().toString();
    return code.isEmpty() ? QStringLiteral("auto") : code;
}

QString MainWindow::currentDstLang() const
{
    const QString code = m_dstLangBox->currentData().toString();
    return code.isEmpty() ? QStringLiteral("zh") : code;
}

QString MainWindow::currentEngine() const
{
    return m_engineBox->currentData().toString();   // "自动" 为空串
}

QString MainWindow::currentWebProviderId() const
{
    return m_webProviderBox->currentData().toString();   // "自动" 为空串
}

QStringList MainWindow::currentWebProviders() const
{
    const QString id = currentWebProviderId();
    // 空 = 自动 = 空列表 = 交给后端默认顺序（google -> mymemory）。
    return id.isEmpty() ? QStringList{} : QStringList{id};
}

void MainWindow::refreshEngineStatus()
{
    // 本地模式下状态行要反映"本机引擎现在是什么状态"：起来了报真实端口，
    // 没起来就是未就绪（后端 LlmConfig 的空的 baseUrl 正好等于 isReady()==false）。
    studious::config::LlmConfig llm = m_settings.llm;
    if (m_settings.useLocalEngine) {
        llm.baseUrl = m_localEngine.baseUrl();
        llm.model = QStringLiteral("local");
        llm.apiKey.clear();
    }

    QString tooltip;
    QString text = buildEngineStatusText(currentSrcLang(),
                                        currentDstLang(),
                                        llm,
                                        currentWebProviders(),
                                        &tooltip);
    if (m_settings.useLocalEngine && !m_localEngine.isRunning()) {
        text += QStringLiteral("｜本地引擎未启动（首次翻译时自动启动）");
    }
    m_engineStatus->setText(text);
    m_engineStatus->setToolTip(tooltip);
}

void MainWindow::setStatusOk(const QString &text)
{
    m_status->setStyleSheet(QString::fromLatin1(kStatusOkStyle));
    m_status->setText(text);
}

void MainWindow::setStatusError(const QString &text)
{
    m_status->setStyleSheet(QString::fromLatin1(kStatusErrorStyle));
    m_status->setText(text);
}

void MainWindow::onOpenSettings()
{
    SettingsDialog dialog(m_settings, m_cache, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_settings = dialog.settings();
    m_settings.save();

    // 改完 LLM 配置后立刻重建两个下拉：llm / azure 的可用性都取决于配置。
    populateEngineBox();
    populateWebProviderBox();
    refreshEngineStatus();

    setStatusOk(m_settings.llm.isValid()
                    ? QStringLiteral("设置已保存（LLM：%1 / %2）")
                          .arg(m_settings.llm.model, m_settings.llm.baseUrl)
                    : QStringLiteral("设置已保存（LLM 未配置，将不会参与自动降级）"));
}

void MainWindow::onOpenAssets()
{
    // 模态对话框：清单默认取 <repo>/backend/asset/manifest.json，可在对话框内换文件。
    AssetDialog dialog(this);
    dialog.exec();

    // 资产可能刚被下载 / 删除：若本地引擎正在跑，让它下次翻译重新校验一遍资产。
    // （这里不主动停服务——用户只是看了一眼资产页就把正在用的引擎打断，体验更差。）
    if (m_settings.useLocalEngine) {
        refreshEngineStatus();
    }
}

void MainWindow::onLocalEngineToggled(bool checked)
{
    m_settings.useLocalEngine = checked;

    if (checked) {
        // 本地引擎只能配 llm 组件，直接把下拉切过去（不靠用户自己记得改）。
        m_settings.engineId = QStringLiteral("llm");
    }

    // 立刻落盘：勾选状态是"下次启动还要生效"的配置，不能等关窗（崩溃 / 强杀就丢了）。
    m_settings.save();

    // 重建下拉：勾上后 llm 项要从「（未配置）」灰态变可用，取消勾选则恢复。
    populateEngineBox();
    refreshEngineStatus();

    if (checked) {
        setStatusOk(QStringLiteral("已启用本地引擎：首次翻译时会自动启动 llama-server（约 1 秒）"));
    } else if (m_localEngine.isRunning()) {
        // 用户明确关掉了本地引擎，就顺手把进程停掉，别让 500MB 模型在后台占内存。
        // stop() 最长要等 3 秒（terminate 对没有窗口的控制台子进程无效），
        // 先把提示刷到状态栏，免得界面看起来像卡死。
        setStatusOk(QStringLiteral("正在停止本地引擎（释放模型占用的内存）…"));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        m_localEngine.shutdown();
        setStatusOk(QStringLiteral("已切回云端 LLM 配置（本地引擎已停止）"));
    } else {
        setStatusOk(QStringLiteral("已切回云端 LLM 配置"));
    }
}

bool MainWindow::applyLocalEngineConfig(studious::config::LlmConfig *cfg, QString *error)
{
    QString startError;
    if (!m_localEngine.ensureStarted(&startError)) {
        if (error != nullptr) {
            *error = startError;
        }
        return false;
    }

    // 本地服务器不校验 model / key，但 LlmConfig::isValid() 要求两者非空。
    // reasoningEffort 清空：本地不传 reasoning_effort（llama-server 其实容忍，
    // 但翻译是确定性任务，多发无意义的字段没有收益）。
    cfg->baseUrl = m_localEngine.baseUrl();
    cfg->apiKey.clear();
    cfg->model = QStringLiteral("local");
    cfg->reasoningEffort.clear();
    // 0.5B 专属收尾（云端配置不受影响，所以只在这里改）：
    //   stripModelNotes：删掉"（中文（简体））""【中文】"这类整行注记；
    //   batchSize = 1：小模型返回的不是 JSON 数组而是对象，合并只会白花两次请求。
    cfg->stripModelNotes = true;
    cfg->batchSize = 1;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void MainWindow::onTranslateClicked()
{
    if (m_job->isRunning()) {
        return;
    }

    const QStringList paragraphs = splitParagraphs(m_input->toPlainText());
    if (paragraphs.isEmpty()) {
        setStatusError(QStringLiteral("输入为空：请先填写要翻译的文本"));
        m_output->clear();
        return;
    }

    // 顺手记住当前界面状态，关窗时就能原样存回 QSettings。
    m_settings.srcLang = currentSrcLang();
    m_settings.dstLang = currentDstLang();
    m_settings.engineId = currentEngine();
    m_settings.webProviderId = currentWebProviderId();
    m_settings.cacheEnabled = m_cacheCheck->isChecked();
    m_settings.useLocalEngine = m_localCheck->isChecked();

    TranslateJobRequest req;
    req.paragraphs = paragraphs;
    req.srcLang = m_settings.srcLang;
    req.dstLang = m_settings.dstLang;
    req.engineId = m_settings.engineId;
    req.webProviders = currentWebProviders();
    req.llm = m_settings.llm;   // 配置快照：翻译过程中改不了，线程内也无需回读界面
    req.cache = m_cache;        // 进程级共享实例（可为空）
    req.cacheEnabled = m_settings.cacheEnabled;

    if (m_settings.useLocalEngine) {
        // 线程约束：ensureStarted() 阻塞约 1 秒且内部是 QProcess（主线程亲和），
        // 必须在**提交任务之前**在主线程跑完；工作线程只拿 baseUrl 快照。
        setStatusOk(QStringLiteral("正在启动本地引擎（首次约 1 秒）…"));
        // 只放行重绘，屏蔽用户输入：启动期间按钮再被点是纯粹的麻烦。
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        QString localError;
        if (!applyLocalEngineConfig(&req.llm, &localError)) {
            QMessageBox::critical(this,
                                  QStringLiteral("本地引擎启动失败"),
                                  localError.isEmpty()
                                      ? QStringLiteral("本地引擎启动失败（未给出原因）")
                                      : localError);
            // 回滚勾选：toggled 里会保存 false、重建引擎下拉、停掉半死的进程。
            m_localCheck->setChecked(false);
            setStatusError(QStringLiteral("本地引擎启动失败：%1")
                               .arg(localError.section(QLatin1Char('\n'), 0, 0)));
            return;
        }
        req.engineId = QStringLiteral("llm");   // 本地引擎只能配 llm 组件
    }

    m_lastSrcParagraphs = paragraphs;
    m_lastEngineId = req.engineId.isEmpty() ? QStringLiteral("自动") : req.engineId;
    if (m_settings.useLocalEngine) {
        m_lastEngineId += QStringLiteral("（本地）");
    }
    m_lastCacheEnabled = req.cacheEnabled;

    m_translateButton->setEnabled(false);
    m_output->clear();
    setStatusOk(QStringLiteral("翻译中…（%1 段 -> %2）")
                    .arg(paragraphs.size())
                    .arg(m_dstLangBox->currentText()));

    m_job->start(req);
}

void MainWindow::onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs)
{
    m_translateButton->setEnabled(true);

    const QString dst = currentDstLang();

    if (!result.outcome.ok) {
        m_output->clear();
        setStatusError(result.outcome.error.isEmpty()
                           ? QStringLiteral("翻译失败（后端未给出原因）")
                           : result.outcome.error);
        refreshEngineStatus();
        return;
    }

    const QStringList dstParagraphs = result.outcome.byLang.value(dst);
    m_output->setPlainText(composeOutput(m_lastSrcParagraphs, dstParagraphs));

    QString status = QStringLiteral("引擎: %1    耗时: %2 ms    段落数: %3")
                         .arg(m_lastEngineId)
                         .arg(elapsedMs)
                         .arg(m_lastSrcParagraphs.size());

    // 只在"这次请求真的走了缓存"时追加命中情况：关掉缓存（或 cacheUsed=false）
    // 就一个字都不提缓存，避免用户以为缓存还在生效。
    if (m_lastCacheEnabled && result.cacheUsed) {
        const int total = result.cacheHits + result.cacheMisses;
        status += QStringLiteral("｜缓存命中 %1/%2").arg(result.cacheHits).arg(total);
    }

    setStatusOk(status);

    refreshEngineStatus();
}

QString MainWindow::composeOutput(const QStringList &srcParagraphs,
                                  const QStringList &dstParagraphs) const
{
    const bool withSource = m_showSourceCheck->isChecked();
    const int count = dstParagraphs.isEmpty() ? srcParagraphs.size() : dstParagraphs.size();

    QStringList blocks;
    blocks.reserve(count);
    for (int i = 0; i < count; ++i) {
        const QString dst = dstParagraphs.value(i);
        if (!withSource) {
            blocks << dst;
            continue;
        }
        const QString src = srcParagraphs.value(i);
        blocks << QStringLiteral("[原文]\n%1\n[译文]\n%2").arg(src, dst);
    }
    return blocks.join(QStringLiteral("\n\n"));
}
