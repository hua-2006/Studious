#pragma once

#include <QMainWindow>
#include <QString>

#include "AppSettings.h"

#include <studious/contract/Types.h>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class TranslateJob;
struct TranslateJobResult;

namespace studious::cache {
class TranslationCache;
}

// Studious 前端 demo 主窗口：纯文本输入 / 输出，不含截图、OCR、浮层。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // cache：进程级共享的翻译缓存（不拥有）。传 nullptr 则不启用缓存——
    // 界面上的勾选框仍会显示，只是 Router 拿不到实例、直接走老路径。
    explicit MainWindow(studious::cache::TranslationCache *cache = nullptr,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    // 关窗时把当前界面状态写回 QSettings。
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onTranslateClicked();
    void onOpenSettings();
    void onOpenAssets();
    void onTranslationFinished(const TranslateJobResult &result, qint64 elapsedMs);

private:
    void buildUi();
    void populateLanguageBoxes();
    void populateEngineBox();
    void populateWebProviderBox();
    void refreshEngineStatus();
    void setStatusOk(const QString &text);
    void setStatusError(const QString &text);

    QString currentSrcLang() const;
    QString currentDstLang() const;
    QString currentEngine() const;
    QString currentWebProviderId() const;
    QStringList currentWebProviders() const;
    QString composeOutput(const QStringList &srcParagraphs,
                          const QStringList &dstParagraphs) const;

    QComboBox *m_srcLangBox = nullptr;
    QComboBox *m_dstLangBox = nullptr;
    QComboBox *m_engineBox = nullptr;
    QComboBox *m_webProviderBox = nullptr;
    QPushButton *m_translateButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QPushButton *m_assetsButton = nullptr;   // 「资产…」：模型 / 运行时下载与管理
    QCheckBox *m_showSourceCheck = nullptr;
    QCheckBox *m_cacheCheck = nullptr;   // 「启用缓存」
    QPlainTextEdit *m_input = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_engineStatus = nullptr;

    AppSettings m_settings;            // 界面背后的配置（LLM 设置 / 默认语言 / 上次选择）

    studious::cache::TranslationCache *m_cache = nullptr;   // 不拥有，进程级共享

    TranslateJob *m_job = nullptr;
    QStringList m_lastSrcParagraphs;   // 最近一次请求的段落，用于"原文对照"
    QString m_lastEngineId;            // 最近一次实际生效的引擎（自动模式下未必等于下拉框值）
    bool m_lastCacheEnabled = false;   // 发请求那一刻的缓存开关（勾选框可能在等待期间被改）
};
