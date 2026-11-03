#include "translators/MockTranslator.h"

#include "contract/LanguageRegistry.h"

namespace studious::translate {

QString MockTranslator::id() const
{
    return QStringLiteral("mock");
}

bool MockTranslator::isReady() const
{
    return true;
}

core::ResolveResult MockTranslator::resolve(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    core::ResolveResult r;
    r.ready = true;
    r.reason = QStringLiteral("mock 无需资产");
    return r;
}

QStringList MockTranslator::supportedTargetLangs() const
{
    // 全部内部语言码；auto 不是合法目标，剔除。
    QStringList langs = core::LanguageRegistry::allInternal();
    langs.removeAll(QStringLiteral("auto"));
    return langs;
}

bool MockTranslator::supports(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    const QString n = core::LanguageRegistry::normalize(dst);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    return supportedTargetLangs().contains(n);
}

QString MockTranslator::detectLanguage(const QStringList &texts) const
{
    // 假检测：出现 CJK 统一汉字就判 zh，否则判 en。仅用于自测链路，不追求准确。
    for (const QString &text : texts) {
        for (const QChar c : text) {
            const char16_t u = c.unicode();
            if (u >= 0x4E00 && u <= 0x9FFF) {
                return QStringLiteral("zh");
            }
        }
    }
    return QStringLiteral("en");
}

core::TranslateOutcome MockTranslator::translate(const QStringList &paragraphs,
                                                 const QString &srcLang,
                                                 const QStringList &dstLangs,
                                                 const QJsonObject &options)
{
    Q_UNUSED(srcLang)
    Q_UNUSED(options)
    core::TranslateOutcome out;
    m_cancelRequested = false;

    if (dstLangs.isEmpty()) {
        out.error = QStringLiteral("mock 未收到目标语言");
        return out;
    }

    for (const QString &rawDst : dstLangs) {
        const QString dst = core::LanguageRegistry::normalize(rawDst);
        QStringList translated;
        translated.reserve(paragraphs.size());
        for (const QString &para : paragraphs) {
            if (m_cancelRequested) {
                out.error = QStringLiteral("mock 翻译已取消");
                return out;
            }
            translated << (QStringLiteral("【") + dst + QStringLiteral("】") + para);
        }
        out.byLang.insert(rawDst, translated);
    }

    out.ok = true;
    return out;
}

void MockTranslator::cancel()
{
    m_cancelRequested = true;
}

} // namespace studious::translate
