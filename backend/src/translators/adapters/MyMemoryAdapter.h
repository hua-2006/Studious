#pragma once

#include "translators/IWebAdapter.h"

namespace studious::translate {

// MyMemory 免费翻译接口。
// 接口形态：
//   GET https://api.mymemory.translated.net/get?q=<text>&langpair=<src>|<dst>
// 限制：不支持 auto 源语言检测，源语言必须显式给出（本实现里 auto 一律降级成 en）。
class MyMemoryAdapter final : public IWebAdapter {
public:
    QString id() const override;
    QStringList supportedLangs() const override;
    bool supports(const QString &dst) const override;
    WebAdapterResult translate(net::IHttpClient &http,
                               const QString &text,
                               const QString &srcLang,
                               const QString &dstLang) override;
};

} // namespace studious::translate
