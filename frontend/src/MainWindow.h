#pragma once

#include <QMainWindow>
#include <QString>

#include <studious/contract/Types.h>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class TranslateJob;

// Studious 前端 demo 主窗口：纯文本输入 / 输出，不含截图、OCR、浮层。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onTranslateClicked();
    void onTranslationFinished(const studious::core::TranslateOutcome &outcome,
                               qint64 elapsedMs);

private:
    void buildUi();
    void populateLanguageBoxes();
    void populateEngineBox();
    void refreshEngineStatus();
    void setStatusOk(const QString &text);
    void setStatusError(const QString &text);

    QString currentSrcLang() const;
    QString currentDstLang() const;
    QString currentEngine() const;
    QString composeOutput(const QStringList &srcParagraphs,
                          const QStringList &dstParagraphs) const;

    QComboBox *m_srcLangBox = nullptr;
    QComboBox *m_dstLangBox = nullptr;
    QComboBox *m_engineBox = nullptr;
    QPushButton *m_translateButton = nullptr;
    QCheckBox *m_showSourceCheck = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_engineStatus = nullptr;

    TranslateJob *m_job = nullptr;
    QStringList m_lastSrcParagraphs;   // 最近一次请求的段落，用于"原文对照"
    QString m_lastEngineId;            // 最近一次实际生效的引擎（自动模式下未必等于下拉框值）
};
