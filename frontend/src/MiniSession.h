// 小窗翻译（设计稿 B 页）的引擎：一段文本进、按目标语言出若干段译文。
//
// 和 LiveSession 的分工：那边是「一块屏幕反复翻」，节拍由定时器驱动；
// 这里是「用户敲完按一次」，一次请求一次结果，所以状态机只有
// Empty / Editing / Translating / Done / Failed 五档。
//
// 本类不碰界面：只持有文本、语言与结果，窗口由 QML 画。
// 翻译入口用独立的 QuickTranslate（自带 TranslateJob），不碰 AppBackend::m_job。
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

namespace studious::ui {

class QuickTranslate;
struct QuickTranslateResult;

class MiniSession final : public QObject
{
    Q_OBJECT
    // Empty（没内容）/ Editing（有内容待翻）/ Translating / Done / Failed
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(QString sourceText READ sourceText WRITE setSourceText NOTIFY sourceTextChanged)
    // 空串 = 自动检测（界面上显示「自动检测 (auto)」）
    Q_PROPERTY(QString srcLang READ srcLang WRITE setSrcLang NOTIFY srcLangChanged)
    // 多选：至少一个。顺序就是译文标签页的顺序。
    Q_PROPERTY(QStringList dstLangs READ dstLangs WRITE setDstLangs NOTIFY dstLangsChanged)
    // 当前在看哪个目标语言的译文（多选时对应标签页）
    Q_PROPERTY(QString activeDst READ activeDst WRITE setActiveDst NOTIFY activeDstChanged)
    // 每段 { index:int, text:QString, passthrough:bool, empty:bool }
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY blocksChanged)
    Q_PROPERTY(int charCount READ charCount NOTIFY statsChanged)
    Q_PROPERTY(int paragraphCount READ paragraphCount NOTIFY statsChanged)
    Q_PROPERTY(QString engineLabel READ engineLabel NOTIFY engineLabelChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(int elapsedMs READ elapsedMs NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY phaseChanged)
    // 输入框左上角那个小标签：手动输入 / 划词获取
    Q_PROPERTY(QString sourceTag READ sourceTag NOTIFY sourceTagChanged)

public:
    explicit MiniSession(QObject *parent = nullptr);
    ~MiniSession() override;

    QString phase() const { return m_phase; }
    QString sourceText() const { return m_sourceText; }
    void setSourceText(const QString &text);
    QString srcLang() const { return m_srcLang; }
    void setSrcLang(const QString &code);
    QStringList dstLangs() const { return m_dstLangs; }
    void setDstLangs(const QStringList &codes);
    QString activeDst() const { return m_activeDst; }
    void setActiveDst(const QString &code);
    QVariantList blocks() const { return m_blocks; }
    int charCount() const;
    int paragraphCount() const;
    QString engineLabel() const { return m_engineLabel; }
    QString errorText() const { return m_errorText; }
    int elapsedMs() const { return m_elapsedMs; }
    bool busy() const { return m_phase == QStringLiteral("Translating"); }
    QString sourceTag() const { return m_sourceTag; }

    Q_INVOKABLE void translate();
    Q_INVOKABLE void cancel();
    // 清空输入与结果，回到 Empty。
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void copyAll();
    Q_INVOKABLE void copyBlock(int index);
    // 把当前结果写进 mini.db（走 AppBackend::saveMiniRecord 的那条路）。
    Q_INVOKABLE void saveRecord();
    // 「划词获取」：目前读系统剪贴板（真正的划词要模拟 Ctrl+C，见 TODO.md）。
    Q_INVOKABLE bool grabClipboard();
    // 打开窗口时把焦点送进输入框：QML 自己 requestActivate()，这里只清上一次的错。
    Q_INVOKABLE void prepare();

    // 语言对是全局一份（settings/lang/*），小窗改了要让主页/截屏/实时跟着变：
    // 改完发 languagesChanged()，由 QML 把它们转给 AppBackend；反过来别处改了，
    // QML 会调这个把值拉回来（本地内存缓存只是显示用，真值永远在 AppSettings）。
    Q_INVOKABLE void reloadLanguagesFromSettings();

signals:
    void phaseChanged();
    void sourceTextChanged();
    void srcLangChanged();
    void dstLangsChanged();
    void languagesChanged();
    void activeDstChanged();
    void blocksChanged();
    void statsChanged();
    void engineLabelChanged();
    void errorTextChanged();
    void resultChanged();
    void sourceTagChanged();
    // 保存到记录：由 QML 转给 backend.saveMiniRecord(...)，本类不认识 AppBackend。
    void saveRecordRequested(const QVariantList &originals, const QVariantList &blocks,
                            const QString &engineLabel);

private:
    void onFinished(const QuickTranslateResult &result);
    void rebuildBlocks();
    void setPhase(const QString &phase);
    void setError(const QString &text);

    QuickTranslate *m_qt = nullptr;
    QString m_phase = QStringLiteral("Empty");
    QString m_sourceText;
    QString m_srcLang;
    QStringList m_dstLangs;
    QString m_activeDst;
    QString m_engineLabel;
    QString m_errorText;
    QString m_sourceTag = QStringLiteral("manual");
    QVariantList m_blocks;
    QStringList m_paragraphs;
    QHash<QString, QStringList> m_byLang;
    int m_elapsedMs = 0;
    // 请求代号：每次 translate() 领一个新号；输入被改/被清/被取消也把号推走。
    // 迟到的旧结果回来时号对不上就直接丢，不能拿旧 paragraphs 盖新原文。
    int m_generation = 0;
    int m_requestGeneration = 0;
};

} // namespace studious::ui
