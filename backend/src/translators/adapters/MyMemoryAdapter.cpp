#include "translators/adapters/MyMemoryAdapter.h"

#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>
#include <QVariant>

namespace studious::translate {

namespace {

const QStringList &myMemoryLangs()
{
    static const QStringList langs = {
        QStringLiteral("zh"),   QStringLiteral("zh-TW"), QStringLiteral("en"),
        QStringLiteral("ja"),   QStringLiteral("ko"),    QStringLiteral("fr"),
        QStringLiteral("de"),   QStringLiteral("es"),    QStringLiteral("ru"),
        QStringLiteral("pt"),   QStringLiteral("pt-BR"), QStringLiteral("it"),
    };
    return langs;
}

} // namespace

QString MyMemoryAdapter::id() const
{
    return QStringLiteral("mymemory");
}

QString MyMemoryAdapter::displayName() const
{
    return QStringLiteral("MyMemory");
}

bool MyMemoryAdapter::requiresApiKey() const
{
    // 匿名接口即可用（有额度上限，官方支持填 email 提额，但不填也能翻）。
    return false;
}

bool MyMemoryAdapter::isConfigured() const
{
    // 无需 key 的来源恒为"已配置"；真实可用性只能由 translate() 的 error 反馈。
    return true;
}

QStringList MyMemoryAdapter::supportedLangs() const
{
    return myMemoryLangs();
}

bool MyMemoryAdapter::supports(const QString &dst) const
{
    const QString n = core::LanguageRegistry::normalize(dst);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    return myMemoryLangs().contains(n);
}

WebAdapterResult MyMemoryAdapter::translate(net::IHttpClient &http,
                                            const QString &text,
                                            const QString &srcLang,
                                            const QString &dstLang)
{
    WebAdapterResult r;

    const QString srcNormalized = core::LanguageRegistry::normalize(srcLang);
    const bool autoFallback = srcNormalized.isEmpty() || srcNormalized == QLatin1String("auto");

    // MyMemory 的 langpair 不支持 auto：源语言必须显式给出。
    // 这里固定用 en 作为代理源语言——这是接口限制，不是我们的选择，可能影响非英文原文的准确性。
    const QString effectiveSrc = autoFallback ? QStringLiteral("en")
                                              : core::LanguageRegistry::toMyMemory(srcNormalized);
    const QString dst = core::LanguageRegistry::toMyMemory(dstLang);
    const QString limitationNote = autoFallback
        ? i18n::s("（限制：MyMemory 不支持 auto 自动检测，已固定使用 en 作为代理源语言）",
                  " (limitation: MyMemory cannot auto-detect the source language, so en is used as "
                  "the proxy source language)")
        : QString();

    if (dst.isEmpty() || dst == QLatin1String("auto")) {
        r.error = i18n::s("mymemory: 目标语言无效", "mymemory: invalid target language")
                  + limitationNote;
        return r;
    }

    const QString encodedText = QString::fromLatin1(QUrl::toPercentEncoding(text));
    const QString langpair = QString::fromLatin1(
        QUrl::toPercentEncoding(effectiveSrc + QLatin1Char('|') + dst));
    const QString url = QStringLiteral("https://api.mymemory.translated.net/get?q=%1&langpair=%2")
                            .arg(encodedText, langpair);

    net::HttpRequest req;
    req.url = QUrl(url);
    req.method = QByteArrayLiteral("GET");
    // MyMemory 是降级链末位，给足时间（它限速较严格，偶发需要排队）。
    req.timeoutMs = 10000;

    const net::HttpResponse resp = http.send(req);
    r.httpStatus = resp.status;

    if (!resp.ok) {
        r.error = i18n::sf("mymemory: %1%2", "mymemory: %1%2",
                           {resp.error.isEmpty() ? i18n::s("请求失败", "the request failed")
                                                 : resp.error,
                            limitationNote});
        return r;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = i18n::sf("mymemory: 响应解析失败（%1）；响应片段：%2",
                           "mymemory: failed to parse the response (%1); response snippet: %2",
                           {perr.errorString(), QString::fromUtf8(resp.body.left(200))})
                  + limitationNote;
        return r;
    }

    const QJsonObject obj = doc.object();

    // responseStatus 有时是数字、有时是字符串，两种都吃。
    const QString statusText = obj.value(QStringLiteral("responseStatus")).toVariant().toString();
    bool statusOk = false;
    const int statusCode = statusText.toInt(&statusOk);
    if (!statusOk || statusCode != 200) {
        const QString detail = obj.value(QStringLiteral("responseDetails")).toString();
        r.error = i18n::sf("mymemory: responseStatus=%1，%2%3",
                           "mymemory: responseStatus=%1, %2%3",
                           {statusText,
                            detail.isEmpty() ? i18n::s("接口返回失败", "the API reported a failure")
                                             : detail,
                            limitationNote});
        return r;
    }

    const QJsonObject data = obj.value(QStringLiteral("responseData")).toObject();
    const QString translated = data.value(QStringLiteral("translatedText")).toString().trimmed();
    if (translated.isEmpty()) {
        r.error = i18n::sf("mymemory: 译文为空；响应片段：%1",
                           "mymemory: the translation is empty; response snippet: %1",
                           {QString::fromUtf8(resp.body.left(200))})
                  + limitationNote;
        return r;
    }

    // 配额耗尽时 translatedText 里放的是告警文案（HTTP 200、responseStatus 甚至也是 200）：
    // "MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY..."
    // 以前只判非空就当译文返回，Router 再把它写进缓存 —— 一段告警于是永久命中。
    // 这里一律当失败上报。注意 error 不能含"已取消/不支持/请求被拒绝"这类语义类关键字，
    // 否则 Router 会认为是"换谁都一样"的失败而不再降级到下一家。
    if (webLooksLikeServiceNotice(translated)) {
        r.error = i18n::sf("mymemory: 返回的是限流/告警文案而不是译文：%1",
                           "mymemory: the API returned a rate-limit/notice message instead of a "
                           "translation: %1",
                           {translated.left(160)})
                  + limitationNote;
        return r;
    }

    // 原样退回输入同样是假成功（语言对不支持、源语言猜错时很常见），不能当译文。
    if (webLooksLikeSourceEcho(translated, text)) {
        r.error = i18n::s("mymemory: 译文与原文完全一致，已丢弃",
                          "mymemory: the translation is identical to the source text, discarded")
                  + limitationNote;
        return r;
    }

    r.ok = true;
    r.text = translated;
    return r;
}

} // namespace studious::translate
