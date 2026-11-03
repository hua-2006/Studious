#pragma once

#include "translators/IWebAdapter.h"

namespace studious::translate {

// Microsoft Azure AI Translator 适配器（v1 占位，尚未真正接入）。
//
// 定位：Azure 是唯一"官方 SLA + 免费层"的来源，适合作为商业场景的兜底：
// 免费层 F0 约 200 万字符/月，超出后可升 S1 按量付费。
// 与 Google/MyMemory 的关键差异是需要两项凭据，因此 requiresApiKey()=true，
// isConfigured() 在 v1 恒为 false（本版本没有设置界面写入凭据的链路）。
//
// ---- 正式接入时的接口契约（照着写即可） ----
//   请求：
//     POST https://api.cognitive.microsofttranslator.com/translate?api-version=3.0
//     头：Ocp-Apim-Subscription-Key:      <订阅 key>
//         Ocp-Apim-Subscription-Region:   <区域，如 eastasia>
//         Content-Type:                   application/json; charset=UTF-8
//     body：[{"Text":"..."}]            // 数组，支持一次多段
//   响应：
//     [{"translations":[{"text":"...","to":"zh-Hans"}]}]
//   注意：
//     1. 响应是"数组"，与请求数组按下标一一对应（批量时可直接合并成一次请求，省额度）。
//     2. 目标语言码用 Azure 自己的写法（zh-Hans / zh-Hant / pt-pt / pt-br），
//        需要新增 LanguageRegistry::toAzure()，禁止在适配器里硬编码。
//     3. 语言检测走 /detect?api-version=3.0，可用于补上 auto 源语言。
//     4. 401=key 无效 / 403=区域不匹配或额度耗尽 / 429=限流（响应头带 Retry-After）。
class AzureTranslatorAdapter final : public IWebAdapter {
public:
    QString id() const override;
    QString displayName() const override;
    bool requiresApiKey() const override;
    bool isConfigured() const override;
    QStringList supportedLangs() const override;
    bool supports(const QString &dst) const override;
    WebAdapterResult translate(net::IHttpClient &http,
                               const QString &text,
                               const QString &srcLang,
                               const QString &dstLang) override;
};

} // namespace studious::translate
