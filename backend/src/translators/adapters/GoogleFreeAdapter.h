#pragma once

#include "translators/IWebAdapter.h"

namespace studious::translate {

// Google 免费翻译接口（translate_a/single，client=gtx）。
// 无需 API key，语言覆盖最广，但稳定性无保障：不承诺 SLA，大批量请求会被限流。
//
// 接口形态：
//   GET https://translate.googleapis.com/translate_a/single?client=gtx&sl=<src>&tl=<dst>&dt=t&q=<text>
// 响应是嵌套 JSON 数组：result[0] 为分段数组，每段 [译文, 原文, ...]。
class GoogleFreeAdapter final : public IWebAdapter {
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
