#include "translators/WebTranslator.h"

#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"

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

QString WebTranslator::cacheTag() const
{
    // 基类默认实现是 id()，对所有 web 来源都返回 "web"：
    // google 与 mymemory 的译文因此落在同一个缓存键上，用户切换来源后
    // 拿到的是另一家的译文（且缓存命中率看起来还"很健康"）。
    //
    // 这里编进去的是"本次真正产出译文的来源"，不是"已经装配/已配置的来源"：
    // 装了 google+mymemory 但 google 一直失败时，实际译文全部来自 mymemory，
    // 旧实现仍把 key 记成 google+mymemory —— 之后 google 恢复，用户拿到的是
    // mymemory 的旧译文却以为来自 google。
    QStringList sources = m_activeSources;
    if (sources.isEmpty()) {
        // 还没成功产出过译文（本进程第一次查缓存，或每次都是全失败）：
        // 退回已配置的装配顺序，保证 tag 非空且稳定，不会和别的引擎或空串撞上。
        // 这一次可能查不到之前按真实产出者存的键，成功一次之后 tag 就切成产出者。
        for (const auto &adapter : m_adapters) {
            if (adapter && adapter->isConfigured()) {
                sources << adapter->id().trimmed();
            }
        }
    }
    if (sources.isEmpty()) {
        // 一个都用不了时回落到"全部已装配来源"：tag 仍然稳定、非空。
        for (const auto &adapter : m_adapters) {
            if (adapter) {
                sources << adapter->id().trimmed();
            }
        }
    }
    return QStringLiteral("web|") + sources.join(QLatin1Char('+'));
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
    r.reason = i18n::s("在线接口，无需下载资产", "Online endpoints, nothing to download");
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

    // 取消标志不在这里清：清零由 Router 在每个请求开始时做一次（见 ICancelControl.h）。
    // 以前每个引擎入口都清，降级边界上（上一个引擎刚失败、用户此刻点取消）到达的
    // 取消会被下一个引擎的入口直接吞掉。
    //
    // "本次真实产出者"先记在局部变量里，整轮跑完再提交（见函数末尾）：
    // 中途取消或全失败时不该把上一轮有效的产出者记录抹掉 —— 抹掉会让 cacheTag()
    // 退回"已配置的来源"，之前按真实产出者存的键下一次就查不到了。
    QStringList producedSources;

    if (m_adapters.isEmpty()) {
        out.error = i18n::s("web 引擎没有注册任何适配器",
                            "The web engine has no adapters registered");
        return out;
    }
    if (dstLangs.isEmpty()) {
        out.error = i18n::s("web 引擎未收到目标语言",
                            "The web engine received no target language");
        return out;
    }

    const QString src = core::LanguageRegistry::normalize(srcLang).isEmpty()
                            ? QStringLiteral("auto")
                            : core::LanguageRegistry::normalize(srcLang);

    QStringList langErrors;

    for (const QString &rawDst : dstLangs) {
        const QString dst = core::LanguageRegistry::normalize(rawDst);
        if (dst.isEmpty() || dst == QLatin1String("auto")) {
            langErrors << i18n::sf("目标语言 \"%1\" 无效", "Invalid target language \"%1\"",
                                   {rawDst});
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
            langErrors << i18n::sf("目标语言 %1 没有适配器支持",
                                   "No adapter supports the target language %1", {dst});
            continue;
        }

        QStringList translated;
        translated.reserve(paragraphs.size());
        QString firstParagraphError;

        for (int i = 0; i < paragraphs.size(); ++i) {
            if (m_cancelRequested) {
                out.error = i18n::s("web 翻译已取消", "The web translation was cancelled");
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
                    adapterErrors << i18n::sf(
                                         "%1: 尚未配置（需要 API key 才能使用），已跳过",
                                         "%1: not configured yet (an API key is required), skipped",
                                         {adapter->displayName().isEmpty() ? adapter->id()
                                                                          : adapter->displayName()});
                    continue;
                }

                const WebAdapterResult r = adapter->translate(m_http, trimmed, src, dst);
                if (r.ok) {
                    const QString text = r.text.trimmed();
                    if (text.isEmpty()) {
                        adapterErrors << i18n::sf("%1 返回空译文", "%1 returned an empty translation",
                                                  {adapter->id()});
                        continue;
                    }
                    // 适配器内部已经筛过一遍，这里再挡一道：第三方适配器可能没有实现拒收，
                    // 而"限流告警"一旦被当成译文就会进缓存，永久生效。
                    if (webLooksLikeServiceNotice(text)) {
                        adapterErrors << i18n::sf(
                                             "%1 返回的是限流/告警文案而不是译文",
                                             "%1 returned a rate-limit/notice message instead of a "
                                             "translation",
                                             {adapter->id()});
                        continue;
                    }
                    if (webLooksLikeSourceEcho(text, trimmed)) {
                        adapterErrors << i18n::sf("%1 原样退回了原文，已丢弃",
                                                  "%1 returned the source text unchanged, discarded",
                                                  {adapter->id()});
                        continue;
                    }
                    translated << text;
                    // 记录"这次真的是谁翻的"：cacheTag() 只用它，不用"装了谁"。
                    // 顺序参与 tag：多来源混用时，"google 先、mymemory 兜底"和
                    // "mymemory 先、google 兜底"是两种不同的译文分布。
                    if (!producedSources.contains(adapter->id())) {
                        producedSources << adapter->id();
                    }
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
                    firstParagraphError =
                        i18n::sf("目标语言 %1 第 %2 段全部适配器失败 [%3]",
                                 "Target language %1, paragraph %2: every adapter failed [%3]",
                                 {dst, i + 1, adapterErrors.join(QStringLiteral(" | "))});
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
        out.error = langErrors.join(i18n::s("；", "; "));
    }

    // 真的产出过译文才提交：Router 在本函数返回后读 cacheTag()，用这个 tag 写缓存，
    // 于是下一次请求按同样的 tag 查 —— 一次热身之后命中率就稳定了。
    // 全失败/中途取消时保留上一轮的值，tag 不会在"产出者"和"已配置"之间来回跳。
    if (!producedSources.isEmpty()) {
        m_activeSources = producedSources;
    }
    return out;
}

void WebTranslator::cancel()
{
    m_cancelRequested = true;
}

void WebTranslator::resetCancel()
{
    // 只允许 Router 在请求开始时调用（见 ICancelControl.h）。
    m_cancelRequested = false;
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
