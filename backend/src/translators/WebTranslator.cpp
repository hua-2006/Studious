#include "translators/WebTranslator.h"

#include "contract/LanguageRegistry.h"

#include <QSet>

#include <utility>

namespace studious::translate {

WebTranslator::WebTranslator(net::IHttpClient &http, QVector<std::shared_ptr<IWebAdapter>> adapters)
    : m_http(http)
    , m_adapters(std::move(adapters))
{
}

QString WebTranslator::id() const
{
    return QStringLiteral("web");
}

bool WebTranslator::isReady() const
{
    // web 无资产依赖：能联网就能用，联网失败以 error 形式反馈，不算"未就绪"。
    return true;
}

core::ResolveResult WebTranslator::resolve(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    core::ResolveResult r;
    r.ready = true;
    r.reason = QStringLiteral("web 无需资产");
    return r;
}

QStringList WebTranslator::supportedTargetLangs() const
{
    QStringList langs;
    QSet<QString> seen;
    for (const auto &adapter : m_adapters) {
        if (!adapter) {
            continue;
        }
        const QStringList list = adapter->supportedLangs();
        for (const QString &lang : list) {
            if (!seen.contains(lang)) {
                seen.insert(lang);
                langs << lang;
            }
        }
    }
    return langs;
}

bool WebTranslator::supports(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)   // 源语言不做限制：一家不支持 auto 时适配器自己兜（如 MyMemory 退化为 en）
    const QString n = core::LanguageRegistry::normalize(dst);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    for (const auto &adapter : m_adapters) {
        if (adapter && adapter->supports(n)) {
            return true;
        }
    }
    return false;
}

core::TranslateOutcome WebTranslator::translate(const QStringList &paragraphs,
                                                const QString &srcLang,
                                                const QStringList &dstLangs,
                                                const QJsonObject &options)
{
    Q_UNUSED(options)
    core::TranslateOutcome out;
    m_cancelRequested = false;

    if (m_adapters.isEmpty()) {
        out.error = QStringLiteral("web 引擎没有注册任何适配器");
        return out;
    }
    if (dstLangs.isEmpty()) {
        out.error = QStringLiteral("web 引擎未收到目标语言");
        return out;
    }

    const QString src = core::LanguageRegistry::normalize(srcLang).isEmpty()
                            ? QStringLiteral("auto")
                            : core::LanguageRegistry::normalize(srcLang);

    QStringList langErrors;

    for (const QString &rawDst : dstLangs) {
        const QString dst = core::LanguageRegistry::normalize(rawDst);
        if (dst.isEmpty() || dst == QLatin1String("auto")) {
            langErrors << QStringLiteral("目标语言 \"%1\" 无效").arg(rawDst);
            continue;
        }

        // 先筛出能支持该语言的适配器，避免每个段落都重复判定。
        QVector<std::shared_ptr<IWebAdapter>> candidates;
        for (const auto &adapter : m_adapters) {
            if (adapter && adapter->supports(dst)) {
                candidates.push_back(adapter);
            }
        }
        if (candidates.isEmpty()) {
            langErrors << QStringLiteral("目标语言 %1 没有适配器支持").arg(dst);
            continue;
        }

        QStringList translated;
        translated.reserve(paragraphs.size());
        QString firstParagraphError;

        for (int i = 0; i < paragraphs.size(); ++i) {
            if (m_cancelRequested) {
                out.error = QStringLiteral("web 翻译已取消");
                return out;
            }

            // 空段落（或纯空白）原样返回空串，不发请求，省流量也省限流额度。
            const QString trimmed = paragraphs.at(i).trimmed();
            if (trimmed.isEmpty()) {
                translated << QString();
                continue;
            }

            bool done = false;
            QStringList adapterErrors;
            for (const auto &adapter : candidates) {
                // 未配置的来源（如 Azure 还没填 key）不必发请求：记一条原因，
                // 继续尝试下一个适配器，绝不因为"这家没配"就整体中断。
                if (!adapter->isConfigured()) {
                    adapterErrors << QStringLiteral("%1: 尚未配置（需要 API key 才能使用），已跳过")
                                         .arg(adapter->displayName().isEmpty() ? adapter->id()
                                                                              : adapter->displayName());
                    continue;
                }

                const WebAdapterResult r = adapter->translate(m_http, trimmed, src, dst);
                if (r.ok) {
                    const QString text = r.text.trimmed();
                    if (text.isEmpty()) {
                        adapterErrors << QStringLiteral("%1 返回空译文").arg(adapter->id());
                        continue;
                    }
                    translated << text;
                    done = true;
                    break;   // 任一适配器成功即采用，不再打别家
                }

                // 适配器失败原因可能有两类：网络/服务不可用，或凭据/配置缺失。
                // 两类都只记录并继续换下一家，只有全部失败才把原因合并上报。
                // 适配器的 error 通常自带 "id: " 前缀，避免重复拼一次。
                const QString prefixed = adapter->id() + QLatin1Char(':');
                adapterErrors << (r.error.startsWith(prefixed)
                                      ? r.error
                                      : QStringLiteral("%1: %2").arg(adapter->id(), r.error));
            }

            if (!done) {
                translated << QString();
                if (firstParagraphError.isEmpty()) {
                    firstParagraphError = QStringLiteral("目标语言 %1 第 %2 段全部适配器失败 [%3]")
                                              .arg(dst)
                                              .arg(i + 1)
                                              .arg(adapterErrors.join(QStringLiteral(" | ")));
                }
            }
        }

        out.byLang.insert(rawDst, translated);
        if (!firstParagraphError.isEmpty()) {
            langErrors << firstParagraphError;
        }
    }

    out.ok = langErrors.isEmpty();
    if (!out.ok) {
        out.error = langErrors.join(QStringLiteral("；"));
    }
    return out;
}

void WebTranslator::cancel()
{
    m_cancelRequested = true;
}

QVector<std::shared_ptr<IWebAdapter>> WebTranslator::adapters() const
{
    return m_adapters;
}

QVector<WebProviderInfo> WebTranslator::providers() const
{
    QVector<WebProviderInfo> list;
    list.reserve(m_adapters.size());
    for (const auto &adapter : m_adapters) {
        if (!adapter) {
            continue;
        }

        // 已知来源用注册表里的中文名与备注；未知（第三方扩展）来源退回适配器自报的名字。
        WebProviderInfo info = findWebProvider(adapter->id());
        if (info.id.isEmpty()) {
            info.id = adapter->id();
            info.displayName = adapter->displayName();
        }

        // requiresApiKey / configured 一律以适配器实现为准，注册表里的只是静态默认值。
        info.requiresApiKey = adapter->requiresApiKey();
        info.configured = adapter->isConfigured();
        list.push_back(info);
    }
    return list;
}

} // namespace studious::translate
