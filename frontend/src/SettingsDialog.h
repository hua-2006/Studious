#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QString>

#include "AppSettings.h"

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

// 「测试连接」的结果：工作线程只回传纯数据，界面在主线程渲染。
struct LlmTestResult {
    bool ok = false;
    QString text;        // 成功时的译文
    QString error;       // 失败原因（引擎原文）
    qint64 elapsedMs = 0;
};

// LLM 设置对话框：一份 LlmConfig 的全部字段 + "测试连接"。
//
// 线程约束同 MainWindow：QtHttpClient 是线程亲和的，测试请求必须在工作线程里
// 现造 http + translator，主线程只拿回 LlmTestResult（纯数据）。
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // 当前界面上的配置（非 LLM 字段沿用构造时传入的值）。
    AppSettings settings() const;

private slots:
    void onTestClicked();
    void onTestFinished();
    void onRestoreDefaults();

private:
    void buildUi();
    void applyToUi(const studious::config::LlmConfig &cfg);
    studious::config::LlmConfig uiToConfig() const;
    void showTestMessage(const QString &text, bool ok);
    void showTestNeutral(const QString &text);

    AppSettings m_settings;   // 只保存非 LLM 字段，LLM 字段由界面控件产出

    QLineEdit *m_baseUrl = nullptr;
    QLineEdit *m_apiKey = nullptr;
    QCheckBox *m_showApiKey = nullptr;
    QLineEdit *m_model = nullptr;
    QSpinBox *m_timeoutMs = nullptr;
    QDoubleSpinBox *m_temperature = nullptr;
    QPlainTextEdit *m_systemPrompt = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel *m_testResult = nullptr;
    QDialogButtonBox *m_buttons = nullptr;

    QFutureWatcher<LlmTestResult> *m_testWatcher = nullptr;
};
