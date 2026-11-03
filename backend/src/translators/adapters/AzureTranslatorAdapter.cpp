#include "translators/adapters/AzureTranslatorAdapter.h"

#include "contract/LanguageRegistry.h"
#include "translators/adapters/GoogleFreeAdapter.h"

namespace studious::translate {

QString AzureTranslatorAdapter::id() const
{
    return QStringLiteral("azure");
}

QString AzureTranslatorAdapter::displayName() const
{
    return QStringLiteral("Microsoft Azure");
}

bool AzureTranslatorAdapter::requiresApiKey() const
{
    // Azure 必须带 Ocp-Apim-Subscription-Key，没有 key 连 401 都拿不到。
    return true;
}

bool AzureTranslatorAdapter::isConfigured() const
{
    // v1 占位：还没有凭据的存储 / 注入链路，恒为"未配置"，WebTranslator 会直接跳过本适配器。
    return false;
}

QStringList AzureTranslatorAdapter::supportedLangs() const
{
    // Azure 官方支持的语言是 Google 超集（含 zh-Hans/zh-Hant、pt-pt/pt-br 等全部我们内部的码），
    // v1 直接复用 Google 支持集合，避免同一份语言表维护两遍。
    static const GoogleFreeAdapter google;
    return google.supportedLangs();
}

bool AzureTranslatorAdapter::supports(const QString &dst) const
{
    const QString n = core::LanguageRegistry::normalize(dst);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    static const GoogleFreeAdapter google;
    return google.supports(n);
}

WebAdapterResult AzureTranslatorAdapter::translate(net::IHttpClient &http,
                                                   const QString &text,
                                                   const QString &srcLang,
                                                   const QString &dstLang)
{
    Q_UNUSED(http)      // 占位实现不发请求：v1 没有凭据，发了也是 401 白等一轮超时。
    Q_UNUSED(text)
    Q_UNUSED(srcLang)
    Q_UNUSED(dstLang)

    WebAdapterResult r;
    r.ok = false;
    r.httpStatus = 0;
    r.error = QStringLiteral("Microsoft Azure 翻译尚未接入：请在设置中填写 Azure 订阅 key"
                             "（免费层 F0 约 200 万字符/月），当前版本仅保留接口");
    return r;
}

} // namespace studious::translate
