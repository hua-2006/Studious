#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QVector>

#include <studious/llm/LlmModelInfo.h>

#include "AppSettings.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace studious::cache {
class TranslationCache;
}

// 「测试连接」的结果：工作线程只回传纯数据，界面在主线程渲染。
struct LlmTestResult {
    bool ok = false;
    QString text;        // 成功时的译文
    QString error;       // 失败原因（引擎原文）
    qint64 elapsedMs = 0;
};

// LLM 设置对话框：一份 LlmConfig 的全部字段 + "获取模型" + "测试连接"。
//
// 线程约束同 MainWindow：QtHttpClient 是线程亲和的，拉模型列表 / 测试连接都必须在
// 工作线程里现造 http，主线程只拿回纯数据（ModelListResult / LlmTestResult）。
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    // cache：进程级共享的翻译缓存（不拥有，可为 nullptr = 缓存不可用）。
    //   对话框只用它读条数 / 清空，不接管生命周期。
    explicit SettingsDialog(const AppSettings &current,
                            studious::cache::TranslationCache *cache,
                            QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // 当前界面上的配置（非 LLM 字段沿用构造时传入的值）。
    AppSettings settings() const;

private slots:
    void onTestClicked();
    void onTestFinished();
    void onFetchModelsClicked();
    void onFetchModelsFinished();
    void onRestoreDefaults();
    void onClearCache();

private:
    void buildUi();
    void applyToUi(const studious::config::LlmConfig &cfg);
    // 选中配置里的界面语言（空串 = 「跟随系统」）。
    void applyLanguageToUi(const QString &uiLanguage);
    studious::config::LlmConfig uiToConfig() const;
    void showTestMessage(const QString &text, bool ok);
    void showTestNeutral(const QString &text);
    void showFetchNeutral(const QString &text);
    void showFetchMessage(const QString &text, bool ok);
    // 按当前模型能力重填「思考等级」下拉（保留原选择，恢复不到就选推荐项）。
    void rebuildReasoningOptions();
    // 刷新底部「缓存：N 条」文案（读取走缓存实例的 count()）。
    void refreshCacheStatus(const QString &note = QString());

    AppSettings m_settings;   // 只保存非 LLM 字段，LLM 字段由界面控件产出

    studious::cache::TranslationCache *m_cache = nullptr;   // 不拥有

    QLineEdit *m_baseUrl = nullptr;
    QComboBox *m_languageBox = nullptr;        // 界面语言：跟随系统 / 中文 / English（重启生效）
    QLineEdit *m_apiKey = nullptr;
    QCheckBox *m_showApiKey = nullptr;
    QComboBox *m_model = nullptr;              // 可编辑：网关不支持 /models 时手输
    QPushButton *m_fetchModelsButton = nullptr;
    QLabel *m_fetchStatus = nullptr;           // 「获取模型」的状态提示，默认空且隐藏
    QSpinBox *m_timeoutMs = nullptr;
    QDoubleSpinBox *m_temperature = nullptr;
    QComboBox *m_reasoningEffort = nullptr;    // 内容按模型能力动态生成
    QPlainTextEdit *m_systemPrompt = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel *m_testResult = nullptr;

    // 底部「缓存：N 条  [清空缓存]」
    QLabel *m_cacheStatus = nullptr;
    QPushButton *m_clearCacheButton = nullptr;

    QDialogButtonBox *m_buttons = nullptr;

    // 「获取模型」拿到的能力表；思考等级下拉据此填充。
    QVector<studious::llm::LlmModelInfo> m_modelInfos;

    QFutureWatcher<LlmTestResult> *m_testWatcher = nullptr;
    QFutureWatcher<studious::llm::ModelListResult> *m_modelsWatcher = nullptr;
};
