#include "MiniSession.h"

#include "AppSettings.h"
#include "QuickTranslate.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHash>
#include <QRegularExpression>
#include <QVariantMap>

namespace studious::ui {

namespace {

// 和 QuickTranslate 的切段规则保持一致：空行分段，段内单换行保留。
QStringList splitParagraphs(const QString &text)
{
    QStringList out;
    const QStringList raw = text.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")),
                                       Qt::SkipEmptyParts);
    for (const QString &one : raw) {
        const QString t = one.trimmed();
        if (!t.isEmpty())
            out.append(t);
    }
    return out;
}

QStringList settingsTargets()
{
    const AppSettings settings = AppSettings::load();
    QStringList dsts;
    const QStringList raw = settings.dstLang.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &one : raw) {
        const QString code = one.trimmed();
        if (!code.isEmpty() && !dsts.contains(code))
            dsts.append(code);
    }
    if (dsts.isEmpty())
        dsts.append(QStringLiteral("zh"));
    return dsts;
}

} // namespace

MiniSession::MiniSession(QObject *parent)
    : QObject(parent)
    , m_qt(new QuickTranslate(this))
{
    const AppSettings settings = AppSettings::load();
    m_srcLang = settings.srcLang == QStringLiteral("auto") ? QString() : settings.srcLang;
    m_dstLangs = settingsTargets();
    m_activeDst = m_dstLangs.first();

    connect(m_qt, &QuickTranslate::finished, this,
            [this](const QuickTranslateResult &result) { onFinished(result); });
}

MiniSession::~MiniSession() = default;

void MiniSession::setSourceText(const QString &text)
{
    if (m_sourceText == text)
        return;
    m_sourceText = text;
    // 原文一改，在飞的那一单就作废（它算的是旧文本）：先推代号把结果作废，
    // 再把请求真的收掉 —— 否则界面已经回到 Editing，QuickTranslate 还占着「忙」，
    // 下一次 translate() 会被拒成「上一次翻译还没结束」。
    ++m_generation;
    if (busy())
        m_qt->cancel();   // 同步回来的那次「已取消」finished 已被上面那行代号挡掉
    emit sourceTextChanged();
    emit statsChanged();
    // 手动改内容就离开「已完成」，别让旧译文顶着新原文。
    if (!busy() && m_phase != QStringLiteral("Empty")) {
        m_byLang.clear();
        m_blocks.clear();
        m_errorText.clear();
        emit blocksChanged();
        emit errorTextChanged();
    }
    setPhase(splitParagraphs(m_sourceText).isEmpty() ? QStringLiteral("Empty")
                                                     : QStringLiteral("Editing"));
    if (m_sourceTag != QStringLiteral("manual")) {
        m_sourceTag = QStringLiteral("manual");
        emit sourceTagChanged();
    }
}

void MiniSession::setSrcLang(const QString &code)
{
    const QString next = (code == QStringLiteral("auto")) ? QString() : code;
    if (m_srcLang == next)
        return;
    m_srcLang = next;
    // 语言对是全局一份（主页、截图、实时、小窗共用 settings/srcLang），
    // 这里改完就落盘，别的窗口下一单会读到同一个值。
    AppSettings settings = AppSettings::load();
    settings.srcLang = next.isEmpty() ? QStringLiteral("auto") : next;
    settings.save();
    emit srcLangChanged();
    emit languagesChanged();
}

void MiniSession::setDstLangs(const QStringList &codes)
{
    QStringList next;
    for (const QString &one : codes) {
        const QString code = one.trimmed();
        if (!code.isEmpty() && !next.contains(code))
            next.append(code);
    }
    if (next.isEmpty())
        next.append(QStringLiteral("zh"));
    // 本地 MT 只按语言对装模型，一次只能翻一个目标语言 —— 多选在这里收成单选
    // （界面按 backend.dstMultiSupported 把「多选」关掉，这里兜住切换引擎那一刻）。
    if (AppSettings::load().engineId == QStringLiteral("local-mt") && next.size() > 1)
        next = {next.first()};
    if (m_dstLangs == next)
        return;
    m_dstLangs = next;
    // 与 setSrcLang 同理：目标语言也只有一份，落盘给别的窗口用。
    AppSettings settings = AppSettings::load();
    settings.dstLang = next.join(QLatin1Char(','));
    settings.save();
    emit dstLangsChanged();
    emit languagesChanged();
    if (!m_dstLangs.contains(m_activeDst)) {
        m_activeDst = m_dstLangs.first();
        emit activeDstChanged();
        rebuildBlocks();
    }
}

void MiniSession::reloadLanguagesFromSettings()
{
    const AppSettings settings = AppSettings::load();
    const QString src = settings.srcLang == QStringLiteral("auto") ? QString() : settings.srcLang;
    QStringList dsts = settings.dstLang.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString &one : dsts)
        one = one.trimmed();
    dsts.removeAll(QString());
    if (dsts.isEmpty())
        dsts.append(QStringLiteral("zh"));
    if (settings.engineId == QStringLiteral("local-mt") && dsts.size() > 1)
        dsts = {dsts.first()};

    if (m_srcLang != src) {
        m_srcLang = src;
        emit srcLangChanged();
    }
    if (m_dstLangs != dsts) {
        m_dstLangs = dsts;
        emit dstLangsChanged();
        if (!m_dstLangs.contains(m_activeDst)) {
            m_activeDst = m_dstLangs.first();
            emit activeDstChanged();
            rebuildBlocks();
        }
    }
}

void MiniSession::setActiveDst(const QString &code)
{
    if (code.isEmpty() || m_activeDst == code)
        return;
    m_activeDst = code;
    emit activeDstChanged();
    rebuildBlocks();
}

int MiniSession::charCount() const
{
    return m_sourceText.size();
}

int MiniSession::paragraphCount() const
{
    return splitParagraphs(m_sourceText).size();
}

void MiniSession::translate()
{
    if (busy())
        return;
    const QStringList paragraphs = splitParagraphs(m_sourceText);
    if (paragraphs.isEmpty()) {
        setError(tr("先输入或粘贴要翻译的文本。"));
        setPhase(QStringLiteral("Empty"));
        return;
    }
    if (m_dstLangs.isEmpty())
        setDstLangs(settingsTargets());
    // 主页把引擎切成本地 MT 之后，小窗这边可能还留着几个目标语言：
    // 先在发单前收成一个，免得因为缺模型整单失败。
    if (AppSettings::load().engineId == QStringLiteral("local-mt") && m_dstLangs.size() > 1)
        setDstLangs({m_dstLangs.first()});

    m_paragraphs = paragraphs;
    m_errorText.clear();
    emit errorTextChanged();
    m_blocks.clear();
    emit blocksChanged();

    // 领号：这一单的结果只有代号还对得上时才准写回界面。
    m_requestGeneration = ++m_generation;

    if (!m_qt->translate(m_sourceText, m_srcLang.isEmpty() ? QStringLiteral("auto") : m_srcLang,
                         m_dstLangs)) {
        setError(m_qt->lastError());
        setPhase(QStringLiteral("Failed"));
        return;
    }
    setPhase(QStringLiteral("Translating"));
}

void MiniSession::cancel()
{
    if (!busy())
        return;
    // 先推代号再 cancel：QuickTranslate::cancel() 会同步 emit 一个「已取消」的 finished，
    // 那道结果必须被 onFinished 丢掉，取消不该在界面上留一条错误。
    ++m_generation;
    m_qt->cancel();
    m_errorText.clear();
    emit errorTextChanged();
    setPhase(QStringLiteral("Editing"));
}

void MiniSession::clearAll()
{
    // 清空即作废在飞的那一单：先推代号，cancel() 同步回来的 finished 也会被丢掉。
    ++m_generation;
    if (busy())
        m_qt->cancel();
    m_sourceText.clear();
    m_byLang.clear();
    m_blocks.clear();
    m_errorText.clear();
    m_engineLabel.clear();
    m_elapsedMs = 0;
    m_paragraphs.clear();
    m_sourceTag = QStringLiteral("manual");
    emit sourceTextChanged();
    emit sourceTagChanged();
    emit statsChanged();
    emit blocksChanged();
    emit errorTextChanged();
    emit engineLabelChanged();
    emit resultChanged();
    setPhase(QStringLiteral("Empty"));
}

void MiniSession::copyAll()
{
    QStringList texts;
    for (const QVariant &one : m_blocks) {
        const QVariantMap map = one.toMap();
        const QString text = map.value(QStringLiteral("text")).toString();
        if (!text.isEmpty())
            texts.append(text);
    }
    if (texts.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(texts.join(QStringLiteral("\n\n")));
}

void MiniSession::copyBlock(int index)
{
    const int count = m_blocks.size();
    if (index < 0 || index >= count)
        return;
    const QString text = m_blocks.at(index).toMap().value(QStringLiteral("text")).toString();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void MiniSession::saveRecord()
{
    if (m_blocks.isEmpty())
        return;

    QVariantList originals;
    for (const QString &one : m_paragraphs)
        originals.append(one);

    QVariantList blocks;
    for (const QVariant &one : m_blocks) {
        const QVariantMap src = one.toMap();
        QVariantMap item;
        item.insert(QStringLiteral("original"), src.value(QStringLiteral("source")));
        item.insert(QStringLiteral("text"), src.value(QStringLiteral("text")));
        blocks.append(item);
    }
    emit saveRecordRequested(originals, blocks, m_engineLabel);
}

bool MiniSession::grabClipboard()
{
    // 翻译中不换输入：在飞的那一单对应的是旧文本，换掉只会让结果错位。
    if (busy())
        return false;

    const QString text = QGuiApplication::clipboard()->text().trimmed();
    if (text.isEmpty())
        return false;
    m_sourceTag = QStringLiteral("clip");
    m_sourceText = text;
    // 划词换掉原文，同样要作废可能还在飞的旧请求。
    ++m_generation;
    m_byLang.clear();
    m_blocks.clear();
    m_errorText.clear();
    m_engineLabel.clear();
    m_elapsedMs = 0;
    emit sourceTagChanged();
    emit sourceTextChanged();
    emit statsChanged();
    emit blocksChanged();
    emit errorTextChanged();
    emit engineLabelChanged();
    emit resultChanged();
    setPhase(QStringLiteral("Editing"));
    return true;
}

void MiniSession::prepare()
{
    m_errorText.clear();
    emit errorTextChanged();
    if (m_phase == QStringLiteral("Failed"))
        setPhase(splitParagraphs(m_sourceText).isEmpty() ? QStringLiteral("Empty")
                                                         : QStringLiteral("Editing"));
}

void MiniSession::onFinished(const QuickTranslateResult &result)
{
    // 迟到的旧结果（用户已经改了原文 / 取消 / 清空）：直接丢，
    // 绝不能拿它去盖当前的 m_paragraphs。
    if (m_requestGeneration != m_generation)
        return;

    m_elapsedMs = static_cast<int>(result.elapsedMs);
    if (!result.ok) {
        m_engineLabel = result.engineLabel;
        emit engineLabelChanged();
        setError(result.error.isEmpty() ? tr("翻译失败。") : result.error);
        setPhase(QStringLiteral("Failed"));
        return;
    }

    m_byLang = result.byLang;
    if (!result.paragraphs.isEmpty())
        m_paragraphs = result.paragraphs;
    m_engineLabel = result.engineLabel;
    m_errorText.clear();
    emit engineLabelChanged();
    emit errorTextChanged();

    if (!m_byLang.contains(m_activeDst)) {
        // 退回用户选过的第一门语言，而不是 m_byLang.keys().first()：
        // 后者是按哈希序来的，可能跳到用户根本没勾的语言上。
        const QString fallback = m_dstLangs.isEmpty() ? QString() : m_dstLangs.first();
        if (!fallback.isEmpty() && fallback != m_activeDst) {
            m_activeDst = fallback;
            emit activeDstChanged();
        }
    }
    rebuildBlocks();
    emit resultChanged();
    setPhase(QStringLiteral("Done"));
}

void MiniSession::rebuildBlocks()
{
    m_blocks.clear();
    const QStringList translated = m_byLang.value(m_activeDst);
    for (int i = 0; i < m_paragraphs.size(); ++i) {
        const QString source = m_paragraphs.at(i);
        const QString text = i < translated.size() ? translated.at(i) : QString();
        QVariantMap item;
        item.insert(QStringLiteral("index"), i + 1);
        item.insert(QStringLiteral("source"), source);
        item.insert(QStringLiteral("text"), text);
        // 原文与译文一样 = 没真正翻（后端按原文返回），界面上标一个小徽章。
        item.insert(QStringLiteral("passthrough"), !text.isEmpty() && text.trimmed() == source.trimmed());
        item.insert(QStringLiteral("empty"), text.isEmpty());
        m_blocks.append(item);
    }
    emit blocksChanged();
}

void MiniSession::setPhase(const QString &phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged();
}

void MiniSession::setError(const QString &text)
{
    m_errorText = text;
    emit errorTextChanged();
}

} // namespace studious::ui
