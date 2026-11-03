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
#include <QSpinBox>
#include <QVBoxLayout>

#include <studious/Factory.h>
#include <studious/contract/Types.h>
#include <studious/net/QtHttpClient.h>

namespace {

const char *const kStyleOk = "color: #1e8449; font-weight: 600;";
const char *const kStyleError = "color: #c0392b; font-weight: 600;";
const char *const kStyleNeutral = "color: #7f8c8d;";

// 「测试连接」用的固定探针句子。
const char *const kProbeText = "Hello, world!";

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

} // namespace

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_settings(current)
    , m_testWatcher(new QFutureWatcher<LlmTestResult>(this))
{
    setWindowTitle(QStringLiteral("设置 — LLM 翻译"));
    setModal(true);
    resize(620, 520);

    buildUi();
    applyToUi(current.llm);

    connect(m_testWatcher, &QFutureWatcher<LlmTestResult>::finished,
            this, &SettingsDialog::onTestFinished);
}

SettingsDialog::~SettingsDialog()
{
    // 不等测试线程：runTestInWorkerThread 内部对象全是线程局部的，不依赖本对话框。
    // 这里只用 disconnect 断掉回调，免得 future 结束时摸到已析构的 watcher。
    m_testWatcher->disconnect(this);
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

    m_model = new QLineEdit;
    m_model->setPlaceholderText(QStringLiteral("deepseek-v4.1-flash"));
    form->addRow(QStringLiteral("模型"), m_model);

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
    m_reasoningEffort = new QComboBox;
    m_reasoningEffort->addItem(QStringLiteral("关闭（推荐，最快）"), QStringLiteral("none"));
    m_reasoningEffort->addItem(QStringLiteral("极低"), QStringLiteral("minimal"));
    m_reasoningEffort->addItem(QStringLiteral("低"), QStringLiteral("low"));
    m_reasoningEffort->addItem(QStringLiteral("中"), QStringLiteral("medium"));
    m_reasoningEffort->addItem(QStringLiteral("高"), QStringLiteral("high"));
    m_reasoningEffort->addItem(QStringLiteral("跟随模型默认（最慢）"), QString());
    m_reasoningEffort->setToolTip(
        QStringLiteral("推理模型的思考等级。翻译建议选「关闭」：实测延迟减半，译文质量无差别。\n"
                       "选「跟随模型默认」则完全不发送该参数，由服务端决定。"));
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
}

void SettingsDialog::applyToUi(const studious::config::LlmConfig &cfg)
{
    m_baseUrl->setText(cfg.baseUrl);
    m_apiKey->setText(cfg.apiKey);
    m_model->setText(cfg.model);
    m_timeoutMs->setValue(cfg.timeoutMs);
    m_temperature->setValue(cfg.temperature);
    {
        const int idx = m_reasoningEffort->findData(cfg.reasoningEffort);
        m_reasoningEffort->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    m_systemPrompt->setPlainText(cfg.systemPrompt);
}

studious::config::LlmConfig SettingsDialog::uiToConfig() const
{
    studious::config::LlmConfig cfg;
    cfg.baseUrl = m_baseUrl->text().trimmed();
    cfg.apiKey = m_apiKey->text().trimmed();
    cfg.model = m_model->text().trimmed();
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

void SettingsDialog::onRestoreDefaults()
{
    applyToUi(AppSettings::defaults().llm);
    m_showApiKey->setChecked(false);
    m_apiKey->setEchoMode(QLineEdit::Password);
    showTestNeutral(QStringLiteral("已恢复默认值（尚未保存，点「确定」才写回配置）。"));
}
