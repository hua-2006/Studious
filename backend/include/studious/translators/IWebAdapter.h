#pragma once

#include "net/IHttpClient.h"

#include <QString>
#include <QStringList>

namespace studious::translate {

// 单适配器的一次翻译结果。
struct WebAdapterResult {
    bool ok = false;
    QString text;
    QString error;
    int httpStatus = 0;
};

// 一个 Web 翻译源（Google 免费接口 / MyMemory / 未来 DeepL、百度……）。
// 适配器只负责"一次单文本请求"，多段落循环与降级由 WebTranslator 负责。
class IWebAdapter {
public:
    virtual ~IWebAdapter() = default;

    virtual QString id() const = 0;                       // "google-free" | "mymemory"
    virtual QStringList supportedLangs() const = 0;       // 内部语言码
    virtual bool supports(const QString &dst) const = 0;
    virtual WebAdapterResult translate(net::IHttpClient &http,
                                       const QString &text,
                                       const QString &srcLang,
                                       const QString &dstLang) = 0;
};

} // namespace studious::translate
