#include "MainWindow.h"

#include "TranslateJob.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QVBoxLayout>

#include <studious/contract/LanguageRegistry.h>
#include <studious/Factory.h>
#include <studious/net/QtHttpClient.h>
#include <studious/router/TranslatorRouter.h>

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

// 临时装配一套组件只为查状态：status() 不联网，但 QtHttpClient 仍必须在当前线程构造。
QString buildEngineStatusText(const QString &src, const QString &dst, QString *tooltip)
{
    studious::net::QtHttpClient http;                          // 主线程内构造，与调用同线程
    auto translators = studious::factory::makeDefaultTranslators(http);
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

QComboBox *newLangBox() { return new QComboBox; }

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_job(new TranslateJob(this))
{
    setWindowTitle(QStringLiteral("Studious — 文本翻译测试"));
    resize(980, 640);

    buildUi();
    populateLanguageBoxes();
    populateEngineBox();

    m_input->setPlainText(QString::fromUtf8(kSampleText));

    connect(m_translateButton, &QPushButton::clicked, this, &MainWindow::onTranslateClicked);
    connect(m_job, &TranslateJob::finished, this, &MainWindow::onTranslationFinished);

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

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // ---------- 顶部一行 ----------
    auto *topBar = new QHBoxLayout;
    topBar->setSpacing(6);

    m_srcLangBox = newLangBox();
    m_dstLangBox = newLangBox();
    m_engineBox = newLangBox();
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
    m_srcLangBox->setCurrentIndex(0);   // 默认自动检测

    m_dstLangBox->clear();
    for (const QString &code : LanguageRegistry::allInternal()) {
        if (code == QLatin1String("auto")) {
            continue;
        }
        m_dstLangBox->addItem(
            QStringLiteral("%1 (%2)").arg(LanguageRegistry::displayName(code), code), code);
    }
    const int zhIndex = m_dstLangBox->findData(QStringLiteral("zh"));
    m_dstLangBox->setCurrentIndex(zhIndex >= 0 ? zhIndex : 0);
}

void MainWindow::populateEngineBox()
{
    m_engineBox->clear();
    m_engineBox->addItem(QStringLiteral("自动"), QString());
    for (const QString &id : {QStringLiteral("web"), QStringLiteral("mock"),
                              QStringLiteral("llm"), QStringLiteral("local-mt")}) {
        m_engineBox->addItem(id, id);
    }
    m_engineBox->setCurrentIndex(0);
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

void MainWindow::refreshEngineStatus()
{
    QString tooltip;
    const QString text =
        buildEngineStatusText(currentSrcLang(), currentDstLang(), &tooltip);
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

    studious::core::TranslateRequest req;
    req.paragraphs = paragraphs;
    req.srcLang = currentSrcLang();
    req.dstLangs = QStringList{currentDstLang()};
    req.preferredEngine = currentEngine();

    m_lastSrcParagraphs = paragraphs;
    m_lastEngineId = req.preferredEngine.isEmpty() ? QStringLiteral("自动") : req.preferredEngine;

    m_translateButton->setEnabled(false);
    m_output->clear();
    setStatusOk(QStringLiteral("翻译中…（%1 段 -> %2）")
                    .arg(paragraphs.size())
                    .arg(m_dstLangBox->currentText()));

    m_job->start(req);
}

void MainWindow::onTranslationFinished(const studious::core::TranslateOutcome &outcome,
                                       qint64 elapsedMs)
{
    m_translateButton->setEnabled(true);

    const QString dst = currentDstLang();

    if (!outcome.ok) {
        m_output->clear();
        setStatusError(outcome.error.isEmpty() ? QStringLiteral("翻译失败（后端未给出原因）")
                                               : outcome.error);
        refreshEngineStatus();
        return;
    }

    const QStringList dstParagraphs = outcome.byLang.value(dst);
    m_output->setPlainText(composeOutput(m_lastSrcParagraphs, dstParagraphs));

    setStatusOk(QStringLiteral("引擎: %1    耗时: %2 ms    段落数: %3")
                    .arg(m_lastEngineId)
                    .arg(elapsedMs)
                    .arg(m_lastSrcParagraphs.size()));

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
