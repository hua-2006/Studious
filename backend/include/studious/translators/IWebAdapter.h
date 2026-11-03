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

// 一个 Web 翻译源（Google 免费接口 / MyMemory / Azure / 未来 DeepL、百度……）。
// 适配器只负责"一次单文本请求"，多段落循环与降级由 WebTranslator 负责。
class IWebAdapter {
public:
    virtual ~IWebAdapter() = default;

    virtual QString id() const = 0;                       // "google" | "azure" | "mymemory"

    // 面向用户的名字，设置页下拉框用： "Google" / "Microsoft Azure" / "MyMemory"。
    virtual QString displayName() const = 0;

    // 是否必须先配 API key 才能调用（Azure=true，免费源=false）。
    virtual bool requiresApiKey() const = 0;

    // 当前是否已具备调用条件：需要 key 的看 key 是否填好；无需 key 的一律 true。
    // WebTranslator 用它决定"直接跳过还是发真请求"。
    virtual bool isConfigured() const = 0;

    virtual QStringList supportedLangs() const = 0;       // 内部语言码
    virtual bool supports(const QString &dst) const = 0;
    virtual WebAdapterResult translate(net::IHttpClient &http,
                                       const QString &text,
                                       const QString &srcLang,
                                       const QString &dstLang) = 0;
};

} // namespace studious::translate
