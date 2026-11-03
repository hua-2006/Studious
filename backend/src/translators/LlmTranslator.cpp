#include "translators/LlmTranslator.h"

#include "contract/LanguageRegistry.h"

namespace studious::translate {

QString LlmTranslator::id() const
{
    return QStringLiteral("llm");
}

bool LlmTranslator::isReady() const
{
    // v1 未实现：llama-server 子进程管理 + 模型加载链还没接，恒为未就绪。
    return false;
}

core::ResolveResult LlmTranslator::resolve(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    core::ResolveResult r;
    r.ready = false;
    r.missingAssets = {QStringLiteral("runtime.llama.vulkan.win-x64"),
                       QStringLiteral("model.qwen2.5-3b.q4km")};
    r.reason = QStringLiteral("LLM 推理引擎未安装（v1 未实现）");
    return r;
}

QStringList LlmTranslator::supportedTargetLangs() const
{
    // LLM 理论上全语言覆盖，先按内部码全集声明；实际能力由 resolve() 决定。
    QStringList langs = core::LanguageRegistry::allInternal();
    langs.removeAll(QStringLiteral("auto"));
    return langs;
}

bool LlmTranslator::supports(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    // 未实现阶段一律返回 false，保证 Router 不会误选。
    return false;
}

core::TranslateOutcome LlmTranslator::translate(const QStringList &paragraphs,
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
    out.error = QStringLiteral("llm 尚未实现");
    return out;
}

void LlmTranslator::cancel()
{
    // 未来用于中断进行中的 HTTP 流式请求；v1 空实现。
}

} // namespace studious::translate
