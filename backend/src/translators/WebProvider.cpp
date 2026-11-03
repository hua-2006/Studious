#include "translators/WebProvider.h"

#include "i18n/L10n.h"
#include "translators/adapters/AzureTranslatorAdapter.h"
#include "translators/adapters/GoogleFreeAdapter.h"
#include "translators/adapters/MyMemoryAdapter.h"

#include <QSet>

namespace studious::translate {

namespace {

// 来源元信息表。顺序即默认优先级：Google（覆盖广）-> MyMemory（能兜底）。
// 表格里的 configured 只是"静态默认值"（有没有可用凭据），
// 真正装进 WebTranslator 后以适配器自己的 isConfigured() 为准。
//
// displayName / note 都是面向用户的文案，**每次调用重新取词**：
// 语言切换后立刻生效，所以这张表不能做成 static 缓存。
QVector<WebProviderInfo> buildProviderTable()
{
    return {
        {QStringLiteral("google"),
         i18n::s("Google 在线翻译", "Google translate (web)"),
         false,
         true,
         i18n::s("免费，无需 key；国内网络通常需要代理，高频请求会被限流",
                 "Free, no key needed. Usually requires a proxy in mainland China, and heavy use "
                 "gets rate limited")},
        {QStringLiteral("azure"),
         i18n::s("Microsoft Azure 翻译", "Microsoft Azure Translator"),
         true,
         false,
         i18n::s("需 Azure 订阅 key + 区域；免费层 F0 约 200 万字符/月，v1 仅保留接口尚未接入",
                 "Needs an Azure subscription key and region; the free F0 tier covers about 2 "
                 "million characters per month. v1 keeps the interface only, not wired up yet")},
        {QStringLiteral("mymemory"),
         i18n::s("MyMemory 记忆库", "MyMemory translation memory"),
         false,
         true,
         i18n::s("免费，无需 key；不支持源语言自动检测（源语言未知时固定按 en 处理），额度有限",
                 "Free, no key needed. No source-language auto detection (falls back to en when the "
                 "source is unknown), and the quota is limited")},
    };
}

} // namespace

QVector<WebProviderInfo> listWebProviders()
{
    return buildProviderTable();
}

WebProviderInfo findWebProvider(const QString &id)
{
    const QString key = id.trimmed().toLower();
    const QVector<WebProviderInfo> table = buildProviderTable();
    for (const WebProviderInfo &info : table) {
        if (info.id == key) {
            return info;
        }
    }
    return WebProviderInfo{};
}

QVector<std::shared_ptr<IWebAdapter>> makeWebAdapters(const QStringList &providerIds)
{
    // 空列表 = 默认顺序，与旧版 Factory 的行为完全一致。
    static const QStringList defaultOrder = {QStringLiteral("google"),
                                             QStringLiteral("mymemory")};
    const QStringList ids = providerIds.isEmpty() ? defaultOrder : providerIds;

    QVector<std::shared_ptr<IWebAdapter>> adapters;
    adapters.reserve(static_cast<qsizetype>(ids.size()));

    QSet<QString> seen;
    for (const QString &rawId : ids) {
        const QString id = rawId.trimmed().toLower();
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        seen.insert(id);

        if (id == QLatin1String("google")) {
            adapters.push_back(std::make_shared<GoogleFreeAdapter>());
        } else if (id == QLatin1String("azure")) {
            // v1 占位：isConfigured()=false，WebTranslator 会跳过它并继续下一个来源。
            adapters.push_back(std::make_shared<AzureTranslatorAdapter>());
        } else if (id == QLatin1String("mymemory")) {
            adapters.push_back(std::make_shared<MyMemoryAdapter>());
        }
        // 未知 id 直接忽略：前端可能还存着旧版本的自定义来源 id。
    }
    return adapters;
}

} // namespace studious::translate
