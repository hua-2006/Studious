#include "translators/LocalMtTranslator.h"

#include "i18n/L10n.h"

namespace studious::translate {

QString LocalMtTranslator::id() const
{
    return QStringLiteral("local-mt");
}

bool LocalMtTranslator::isReady() const
{
    // v1 未实现：onnxruntime + OPUS-MT 模型加载链还没接，恒为未就绪。
    return false;
}

core::ResolveResult LocalMtTranslator::resolve(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    core::ResolveResult r;
    r.ready = false;
    r.missingAssets = {QStringLiteral("mt.opus.en-zh")};
    r.reason = i18n::s("本地翻译模型未安装（v1 未实现）",
                       "The local translation models are not installed (not implemented in v1)");
    return r;
}

QStringList LocalMtTranslator::supportedTargetLangs() const
{
    // 规划中的语言对先声明出来，供 UI 展示"可下载"；实际能力由 resolve() 决定。
    return {QStringLiteral("zh"), QStringLiteral("en")};
}

bool LocalMtTranslator::supports(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    // 未实现阶段一律返回 false，保证 Router 不会误选。
    return false;
}

core::TranslateOutcome LocalMtTranslator::translate(const QStringList &paragraphs,
                                                    const QString &srcLang,
                                                    const QStringList &dstLangs,
                                                    const QJsonObject &options)
{
    Q_UNUSED(paragraphs)
    Q_UNUSED(srcLang)
    Q_UNUSED(dstLangs)
    Q_UNUSED(options)
    core::TranslateOutcome out;
    out.ok = false;
    out.error = i18n::s("local-mt 尚未实现", "local-mt has not been implemented yet");
    return out;
}

void LocalMtTranslator::cancel()
{
    // 无进行中的任务，空实现。
}

} // namespace studious::translate
