#include "SettingsDialog.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <studious/Factory.h>
#include <studious/contract/Types.h>
#include <studious/llm/LlmCatalog.h>
#include <studious/net/QtHttpClient.h>

namespace {

const char *const kStyleOk = "color: #1e8449; font-weight: 600;";
const char *const kStyleError = "color: #c0392b; font-weight: 600;";
const char *const kStyleNeutral = "color: #7f8c8d;";

// 「模型」行下面那行状态提示用的是同色系的小字版本。
const char *const kStyleSmallOk = "color: #1e8449; font-weight: 600; font-size: 11px;";
const char *const kStyleSmallError = "color: #c0392b; font-weight: 600; font-size: 11px;";
const char *const kStyleSmallNeutral = "color: #7f8c8d; font-size: 11px;";

// 「测试连接」用的固定探针句子。
const char *const kProbeText = "Hello, world!";

// 下拉里显示 id（请求时用的就是它），tooltip 补上展示名 / 能力 / 上下文上限。
// 上限为 0 表示网关没告知（标准 OpenAI /models 只有 id 与 owned_by）。
QString modelToolTip(const studious::llm::LlmModelInfo &info)
{
    const QString name = info.name.isEmpty() ? info.id : info.name;
    const QString limits = (info.maxInputTokens > 0 || info.maxOutputTokens > 0)
                               ? QStringLiteral("%1/%2")
                                     .arg(info.maxInputTokens)
                                     .arg(info.maxOutputTokens)
                               : QStringLiteral("未告知");
    return QStringLiteral("%1 / 推理=%2 / 上限=%3")
        .arg(name,
             info.supportsReasoning ? QStringLiteral("是") : QStringLiteral("否"),
             limits);
}

// 工作线程入口：http / translator 都在本线程内构造、使用、销毁。
// 只把 LlmTestResult（纯数据）交回去。
LlmTestResult runTestInWorkerThread(const studious::config::LlmConfig &cfg)
{
    LlmTestResult out;

    QElapsedTimer timer;
    timer.start();

    studious::net::QtHttpClient http;   // 必须与 translator 同线程
    auto translator = studious::factory::makeLlmTranslator(cfg, http);
    if (!translator || !translator->isReady()) {
        out.error = QStringLiteral("LLM 配置不可用：Base URL 与模型名不能为空");
        return out;
    }

    const studious::core::TranslateOutcome outcome =
        translator->translate(QStringList{QString::fromUtf8(kProbeText)},
                              QStringLiteral("auto"),
                              QStringList{QStringLiteral("zh")},
                              QJsonObject{});
    out.elapsedMs = timer.elapsed();

    if (!outcome.ok) {
        out.error = outcome.error.isEmpty() ? QStringLiteral("翻译失败（后端未给出原因）")
                                           : outcome.error;
        return out;
    }

    const QStringList lines = outcome.byLang.value(QStringLiteral("zh"));
    const QString text = lines.isEmpty() ? QString() : lines.first();
    if (text.trimmed().isEmpty()) {
        out.error = QStringLiteral("连接成功但译文为空（模型返回了空 content）");
        return out;
    }

    out.ok = true;
    out.text = text.trimmed();
    return out;
}

// 工作线程入口：拉模型目录。同样在本线程内现造 QtHttpClient，
// 只把 ModelListResult（纯数据）交回主线程。
studious::llm::ModelListResult runListModelsInWorkerThread(const studious::config::LlmConfig &cfg)
{
    studious::net::QtHttpClient http;
    return studious::llm::listModels(cfg, http);
}

} // namespace

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_settings(current)
    , m_testWatcher(new QFutureWatcher<LlmTestResult>(this))
    , m_modelsWatcher(new QFutureWatcher<studious::llm::ModelListResult>(this))
{
    setWindowTitle(QStringLiteral("设置 — LLM 翻译"));
    setModal(true);
    resize(620, 560);

    buildUi();
    applyToUi(current.llm);

    connect(m_testWatcher, &QFutureWatcher<LlmTestResult>::finished,
            this, &SettingsDialog::onTestFinished);
    connect(m_modelsWatcher, &QFutureWatcher<studious::llm::ModelListResult>::finished,
            this, &SettingsDialog::onFetchModelsFinished);
}

SettingsDialog::~SettingsDialog()
{
    // 不等工作线程：两个 worker 内部对象全是线程局部的，不依赖本对话框。
    // 这里只用 disconnect 断掉回调，免得 future 结束时摸到已析构的 watcher。
    m_testWatcher->disconnect(this);
    m_modelsWatcher->disconnect(this);
}

void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_baseUrl = new QLineEdit;
    m_baseUrl->setPlaceholderText(QStringLiteral("http://127.0.0.1:3065/v1"));
    m_baseUrl->setToolTip(QStringLiteral(
        "OpenAI 兼容网关的 /v1 根地址；本地 Ollama 一般填 http://127.0.0.1:11434/v1"));
    form->addRow(QStringLiteral("Base URL"), m_baseUrl);

    // API Key：密码态 + 右侧「显示」勾选框。
    m_apiKey = new QLineEdit;
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(QStringLiteral("网关或云厂商的 API Key"));
    m_showApiKey = new QCheckBox(QStringLiteral("显示"));
    connect(m_showApiKey, &QCheckBox::toggled, this, [this](bool on) {
        m_apiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    auto *keyRow = new QWidget;
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(6);
    keyLayout->addWidget(m_apiKey, 1);
    keyLayout->addWidget(m_showApiKey);
    form->addRow(QStringLiteral("API Key"), keyRow);

    // ---------- 模型：可编辑下拉 + 「获取模型」 ----------
    // 可编辑的原因：不少网关（尤其是自建代理）不实现 /models，
    // 或者只返回 chat 之外的模型，用户必须能直接手输模型名。
    m_model = new QComboBox;
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);   // 手输不该污染下拉列表
    m_model->setMinimumWidth(240);
    m_model->setToolTip(QStringLiteral(
        "点「获取模型」从网关拉取可用模型；网关不支持 /models 时可直接手输模型名"));
    if (m_model->lineEdit() != nullptr) {
        m_model->lineEdit()->setPlaceholderText(QStringLiteral("deepseek-v4.1-flash"));
    }

    m_fetchModelsButton = new QPushButton(QStringLiteral("获取模型"));
    m_fetchModelsButton->setToolTip(
        QStringLiteral("用当前的 Base URL + API Key 请求 /models，拉取可用模型列表"));

    auto *modelRow = new QWidget;
    auto *modelLayout = new QVBoxLayout(modelRow);
    modelLayout->setContentsMargins(0, 0, 0, 0);
    modelLayout->setSpacing(2);
    auto *modelLine = new QHBoxLayout;
    modelLine->setContentsMargins(0, 0, 0, 0);
    modelLine->setSpacing(6);
    modelLine->addWidget(m_model, 1);
    modelLine->addWidget(m_fetchModelsButton);
    modelLayout->addLayout(modelLine);

    m_fetchStatus = new QLabel;
    m_fetchStatus->setWordWrap(true);
    m_fetchStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fetchStatus->setStyleSheet(QString::fromLatin1(kStyleSmallNeutral));
    m_fetchStatus->setText(QString());
    m_fetchStatus->setVisible(false);   // 没内容时不占视觉空间
    modelLayout->addWidget(m_fetchStatus);

    form->addRow(QStringLiteral("模型"), modelRow);

    m_timeoutMs = new QSpinBox;
    m_timeoutMs->setRange(1000, 180000);
    m_timeoutMs->setSingleStep(1000);
    m_timeoutMs->setSuffix(QStringLiteral(" ms"));
    m_timeoutMs->setToolTip(QStringLiteral("单次 HTTP 请求超时；长段落 + 慢模型请调大"));
    form->addRow(QStringLiteral("超时"), m_timeoutMs);

    m_temperature = new QDoubleSpinBox;
    m_temperature->setRange(0.0, 1.5);
    m_temperature->setSingleStep(0.1);
    m_temperature->setDecimals(1);
    m_temperature->setToolTip(QStringLiteral("翻译任务建议 0.0~0.3；越大越发散"));
    form->addRow(QStringLiteral("Temperature"), m_temperature);

    // ---------- 思考等级 ----------
    // 推理模型默认会先"想"再答，翻译是确定性任务，思考只增加延迟不提升质量。
    // 实测本机网关：不传约 2.0s；reasoning_effort=none 降到约 1.1s 且 reasoning_tokens=0。
    // 具体条目由 rebuildReasoningOptions() 按当前模型能力生成，这里不写死。
    m_reasoningEffort = new QComboBox;
    m_reasoningEffort->setToolTip(
        QStringLiteral("推理模型的思考等级。翻译建议选「关闭」：实测延迟减半，译文质量无差别。\n"
                       "选「跟随模型默认」则完全不发送该参数，由服务端决定。\n"
                       "模型不支持推理时该项会置灰，下拉里只有「不适用」。"));
    form->addRow(QStringLiteral("思考等级"), m_reasoningEffort);

    m_systemPrompt = new QPlainTextEdit;
    m_systemPrompt->setPlaceholderText(
        QStringLiteral("留空则使用后端内置 prompt（推荐）；填写则完全覆盖内置 system prompt"));
    m_systemPrompt->setTabChangesFocus(true);
    m_systemPrompt->setFixedHeight(110);
    form->addRow(QStringLiteral("System Prompt"), m_systemPrompt);

    root->addLayout(form);

    // ---------- 测试连接 ----------
    m_testButton = new QPushButton(QStringLiteral("测试连接"));
    m_testButton->setToolTip(QStringLiteral("用当前填写的参数真实翻译一次 \"Hello, world!\" -> 中文"));

    auto *testRow = new QHBoxLayout;
    testRow->setSpacing(8);
    testRow->addWidget(m_testButton);
    testRow->addStretch(1);
    root->addLayout(testRow);

    m_testResult = new QLabel;
    m_testResult->setWordWrap(true);
    m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_testResult->setMinimumHeight(40);
    m_testResult->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    showTestNeutral(QStringLiteral("尚未测试。点「测试连接」会真的发一次翻译请求。"));
    root->addWidget(m_testResult, 1);

    // ---------- Ok / Cancel / 恢复默认 ----------
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                     | QDialogButtonBox::RestoreDefaults);
    m_buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    m_buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("恢复默认"));
    root->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_buttons, &QDialogButtonBox::clicked, this, [this](QAbstractButton *button) {
        if (m_buttons->standardButton(button) == QDialogButtonBox::RestoreDefaults) {
            onRestoreDefaults();
        }
    });
    connect(m_testButton, &QPushButton::clicked, this, &SettingsDialog::onTestClicked);
    connect(m_fetchModelsButton, &QPushButton::clicked, this, &SettingsDialog::onFetchModelsClicked);
    // 换模型（下拉选或手输）都要重算思考等级：不同模型支持的等级不一样。
    connect(m_model, &QComboBox::currentTextChanged, this, [this](const QString &) {
        rebuildReasoningOptions();
    });
}

void SettingsDialog::applyToUi(const studious::config::LlmConfig &cfg)
{
    m_baseUrl->setText(cfg.baseUrl);
    m_apiKey->setText(cfg.apiKey);
    {
        // setCurrentText 对可编辑下拉：列表里有就选中，没有就填进编辑框。
        const QSignalBlocker blocker(m_model);
        m_model->setCurrentText(cfg.model);
    }
    m_timeoutMs->setValue(cfg.timeoutMs);
    m_temperature->setValue(cfg.temperature);
    m_systemPrompt->setPlainText(cfg.systemPrompt);

    // 先按模型能力把思考等级重填一次（有缓存能力表就用，没有就当支持推理），
    // 再选中配置里保存的等级；找不到就落回第一项。
    rebuildReasoningOptions();
    const int idx = m_reasoningEffort->findData(cfg.reasoningEffort);
    m_reasoningEffort->setCurrentIndex(idx >= 0 ? idx : 0);
}

void SettingsDialog::rebuildReasoningOptions()
{
    if (m_reasoningEffort == nullptr || m_model == nullptr) {
        return;
    }

    const bool hadItems = m_reasoningEffort->count() > 0;
    const QString previous = m_reasoningEffort->currentData().toString();

    // 按当前模型名在能力表里查；查不到（手输的模型 / 还没点过「获取模型」）
    // 就造一个 supportsReasoning = true 的默认对象，让用户至少能看到全部等级。
    studious::llm::LlmModelInfo target;
    const QString wanted = m_model->currentText().trimmed();
    bool found = false;
    for (const studious::llm::LlmModelInfo &info : m_modelInfos) {
        if (info.id.compare(wanted, Qt::CaseInsensitive) == 0) {
            target = info;
            found = true;
            break;
        }
    }
    if (!found) {
        target.supportsReasoning = true;
    }

    const QVector<studious::llm::ReasoningOption> options =
        studious::llm::reasoningOptionsFor(target);

    // 重填期间屏蔽信号：currentData 会随 clear/addItem 变化，别触发无谓回调。
    const QSignalBlocker blocker(m_reasoningEffort);
    m_reasoningEffort->clear();
    for (const studious::llm::ReasoningOption &option : options) {
        // value 可能是空串（= 不发送 reasoning_effort），空串也要能正确存取。
        m_reasoningEffort->addItem(option.label, option.value);
    }

    int index = 0;
    if (hadItems) {
        // 记住原值再恢复；恢复不到（比如换了不支持推理的模型）indexOf 会给 0。
        index = studious::llm::indexOfReasoningOption(options, previous);
    } else {
        // 首次填充：直接选推荐项（不支持推理时只有一项，也是它）。
        for (int i = 0; i < options.size(); ++i) {
            if (options.at(i).recommended) {
                index = i;
                break;
            }
        }
    }
    index = qBound(0, index, static_cast<int>(options.size()) - 1);
    m_reasoningEffort->setCurrentIndex(index);

    // 只有「不适用」一项 = 模型不支持推理：置灰但仍保留 data，
    // 这样「测试连接」通过 uiToConfig() 还能读到值（空串 = 不发送该参数）。
    m_reasoningEffort->setEnabled(options.size() > 1);
}

studious::config::LlmConfig SettingsDialog::uiToConfig() const
{
    studious::config::LlmConfig cfg;
    cfg.baseUrl = m_baseUrl->text().trimmed();
    cfg.apiKey = m_apiKey->text().trimmed();
    cfg.model = m_model->currentText().trimmed();
    cfg.timeoutMs = m_timeoutMs->value();
    cfg.maxTokens = m_settings.llm.maxTokens;   // UI 不暴露，沿用传入值
    cfg.temperature = m_temperature->value();
    cfg.reasoningEffort = m_reasoningEffort->currentData().toString();
    cfg.systemPrompt = m_systemPrompt->toPlainText().trimmed();
    return cfg;
}

AppSettings SettingsDialog::settings() const
{
    AppSettings out = m_settings;
    out.llm = uiToConfig();
    return out;
}

void SettingsDialog::showTestNeutral(const QString &text)
{
    m_testResult->setStyleSheet(QString::fromLatin1(kStyleNeutral));
    m_testResult->setText(text);
}

void SettingsDialog::showTestMessage(const QString &text, bool ok)
{
    m_testResult->setStyleSheet(QString::fromLatin1(ok ? kStyleOk : kStyleError));
    m_testResult->setText(text);
}

void SettingsDialog::showFetchNeutral(const QString &text)
{
    m_fetchStatus->setStyleSheet(QString::fromLatin1(kStyleSmallNeutral));
    m_fetchStatus->setText(text);
    m_fetchStatus->setVisible(true);
}

void SettingsDialog::showFetchMessage(const QString &text, bool ok)
{
    m_fetchStatus->setStyleSheet(QString::fromLatin1(ok ? kStyleSmallOk : kStyleSmallError));
    m_fetchStatus->setText(text);
    m_fetchStatus->setVisible(true);
}

void SettingsDialog::onTestClicked()
{
    if (m_testWatcher->isRunning()) {
        return;
    }

    const studious::config::LlmConfig cfg = uiToConfig();
    if (cfg.baseUrl.isEmpty() || cfg.model.isEmpty()) {
        showTestMessage(QStringLiteral("请先填写 Base URL 和模型名。"), false);
        return;
    }

    m_testButton->setEnabled(false);
    showTestNeutral(QStringLiteral("测试中…（正在请求 %1）").arg(cfg.baseUrl));

    m_testWatcher->setFuture(QtConcurrent::run(&runTestInWorkerThread, cfg));
}

void SettingsDialog::onTestFinished()
{
    m_testButton->setEnabled(true);

    const LlmTestResult result = m_testWatcher->result();
    if (!result.ok) {
        // 失败一律贴引擎原文：网关 401 / 模型名写错 / 超时 都只能靠这行定位。
        showTestMessage(result.error, false);
        return;
    }

    showTestMessage(QStringLiteral("连接成功：%1 → %2（%3 ms）")
                        .arg(QString::fromUtf8(kProbeText), result.text, QString::number(result.elapsedMs)),
                    true);
}

void SettingsDialog::onFetchModelsClicked()
{
    if (m_modelsWatcher->isRunning()) {
        return;
    }

    // 用界面上当前的 baseUrl / apiKey 组临时配置：用户改完地址就该能立刻拉，
    // 不必先点「确定」写回配置。
    const studious::config::LlmConfig cfg = uiToConfig();
    if (cfg.baseUrl.isEmpty()) {
        showFetchMessage(QStringLiteral("请先填写 Base URL，再点「获取模型」。"), false);
        return;
    }

    m_fetchModelsButton->setEnabled(false);
    m_fetchModelsButton->setText(QStringLiteral("获取中…"));
    showFetchNeutral(QStringLiteral("正在请求 %1/models …").arg(cfg.baseUrl));

    m_modelsWatcher->setFuture(QtConcurrent::run(&runListModelsInWorkerThread, cfg));
}

void SettingsDialog::onFetchModelsFinished()
{
    m_fetchModelsButton->setEnabled(true);
    m_fetchModelsButton->setText(QStringLiteral("获取模型"));

    const studious::llm::ModelListResult result = m_modelsWatcher->result();
    if (!result.ok) {
        // 失败贴后端原文（后端已经给了中文原因：key 无效 / 404 路径 / 超时…）。
        showFetchMessage(result.error.isEmpty() ? QStringLiteral("获取模型失败（后端未给出原因）")
                                               : result.error,
                         false);
        return;
    }

    const QString previous = m_model->currentText().trimmed();
    m_modelInfos = result.models;

    {
        // 重填下拉时逐个 addItem 会反复触发 currentTextChanged，
        // 统一屏蔽后只重建一次思考等级。
        const QSignalBlocker blocker(m_model);
        m_model->clear();
        for (const studious::llm::LlmModelInfo &info : m_modelInfos) {
            m_model->addItem(info.id);   // 显示 id：请求里 model 字段要用的就是它
            const int row = m_model->count() - 1;
            m_model->setItemData(row, info.id, Qt::UserRole);
            m_model->setItemData(row, modelToolTip(info), Qt::ToolTipRole);
        }
        // 尽量保住用户原来的输入：还在列表里就选它，否则退回第一项。
        const int same = m_model->findText(previous, Qt::MatchFixedString);
        m_model->setCurrentIndex(same >= 0 ? same : 0);
    }

    // 能力表到位了：按当前选中模型重填思考等级（内部会保留原选择）。
    rebuildReasoningOptions();

    showFetchMessage(QStringLiteral("已获取 %1 个模型").arg(m_modelInfos.size()), true);
}

void SettingsDialog::onRestoreDefaults()
{
    applyToUi(AppSettings::defaults().llm);
    m_showApiKey->setChecked(false);
    m_apiKey->setEchoMode(QLineEdit::Password);
    showTestNeutral(QStringLiteral("已恢复默认值（尚未保存，点「确定」才写回配置）。"));
}
