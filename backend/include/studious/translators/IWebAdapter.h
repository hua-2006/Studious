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

// 免费接口的"限流/拦截文案"识别。
//
// 这些接口配额耗尽时 HTTP 依然是 200，告警直接塞在译文槽里，例如：
//   "MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY..."
// Google 免费接口被打回时也可能是带链接的提示页。
// 只判"非空"的话它们会被当译文返回，还会被 Router 写进缓存永久生效（实测踩过）。
//
// 保守起见只认两类明显信号：官方告警前缀、URL。译文里带链接的正常文本极少。
inline bool webLooksLikeServiceNotice(const QString &text)
{
    static constexpr const char *kNeedles[] = {
        "MYMEMORY WARNING",
        "QUERY LENGTH LIMIT EXCEEDED",
        "ALL AVAILABLE FREE TRANSLATIONS",
        "http://",
        "https://",
        "www.",
        "rate limit",
        "too many requests",
        "QUERY LIMIT",
        "请求过于频繁",
        "超出配额",
    };
    for (const char *needle : kNeedles) {
        if (text.contains(QLatin1String(needle), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

// 去掉所有空白并折成小写，只用于"是不是原文照抄"的判定。
inline QString webNormalizeForEcho(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (!ch.isSpace()) {
            out.append(ch);
        }
    }
    return out.toLower();
}

// 适配器返回的译文像不像"原文照抄"？（大小写/空白归一后比较）
// 免费接口在"语言对不支持"或源语言判断错误时经常原样退回输入，那是假成功。
//
// 只对 5 个字符以上的文本判定：'OK'、'USB'、'Wi-Fi' 这种极短串原样保留
// 往往就是正确答案（缩写/专有名词），把它们判成失败会让本来能用的段落到不了手。
inline bool webLooksLikeSourceEcho(const QString &translated, const QString &source)
{
    const QString norm = webNormalizeForEcho(translated);
    if (norm.size() < 5) {
        return false;
    }
    return norm == webNormalizeForEcho(source);
}

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
