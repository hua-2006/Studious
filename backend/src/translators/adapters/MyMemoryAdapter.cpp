#include "translators/adapters/MyMemoryAdapter.h"

#include "contract/LanguageRegistry.h"

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
        ? QStringLiteral("（限制：MyMemory 不支持 auto 自动检测，已固定使用 en 作为代理源语言）")
        : QString();

    if (dst.isEmpty() || dst == QLatin1String("auto")) {
        r.error = QStringLiteral("mymemory: 目标语言无效") + limitationNote;
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
        r.error = QStringLiteral("mymemory: %1%2")
                      .arg(resp.error.isEmpty() ? QStringLiteral("请求失败") : resp.error,
                           limitationNote);
        return r;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("mymemory: 响应解析失败（%1）；响应片段：%2")
                      .arg(perr.errorString(), QString::fromUtf8(resp.body.left(200)))
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
        r.error = QStringLiteral("mymemory: responseStatus=%1，%2%3")
                      .arg(statusText,
                           detail.isEmpty() ? QStringLiteral("接口返回失败") : detail,
                           limitationNote);
        return r;
    }

    const QJsonObject data = obj.value(QStringLiteral("responseData")).toObject();
    const QString translated = data.value(QStringLiteral("translatedText")).toString().trimmed();
    if (translated.isEmpty()) {
        r.error = QStringLiteral("mymemory: 译文为空；响应片段：%1")
                      .arg(QString::fromUtf8(resp.body.left(200)))
                  + limitationNote;
        return r;
    }

    r.ok = true;
    r.text = translated;
    return r;
}

} // namespace studious::translate
