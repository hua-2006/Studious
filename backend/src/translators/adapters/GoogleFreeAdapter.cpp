#include "translators/adapters/GoogleFreeAdapter.h"

#include "contract/LanguageRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>

namespace studious::translate {

namespace {

// Google 免费接口对内部支持的语言都能翻（个别冷门对可能机翻质量差，但不是"不支持"）。
const QStringList &googleLangs()
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

QString GoogleFreeAdapter::id() const
{
    return QStringLiteral("google");
}

QString GoogleFreeAdapter::displayName() const
{
    return QStringLiteral("Google");
}

bool GoogleFreeAdapter::requiresApiKey() const
{
    // 走 translate_a/single 的 gtx 免费端点，不需要任何凭据。
    return false;
}

bool GoogleFreeAdapter::isConfigured() const
{
    // 无需 key 的来源恒为"已配置"；真实可用性只能由 translate() 的 error 反馈。
    return true;
}

QStringList GoogleFreeAdapter::supportedLangs() const
{
    return googleLangs();
}

bool GoogleFreeAdapter::supports(const QString &dst) const
{
    const QString n = core::LanguageRegistry::normalize(dst);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    return googleLangs().contains(n);
}

WebAdapterResult GoogleFreeAdapter::translate(net::IHttpClient &http,
                                             const QString &text,
                                             const QString &srcLang,
                                             const QString &dstLang)
{
    WebAdapterResult r;

    // sl 支持 auto（由 Google 自行检测），tl 必须是具体语言。
    const QString sl = core::LanguageRegistry::toGoogle(srcLang);
    const QString tl = core::LanguageRegistry::toGoogle(dstLang);
    if (tl.isEmpty() || tl == QLatin1String("auto")) {
        r.error = QStringLiteral("google: 目标语言无效");
        return r;
    }

    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(text));
    const QString url = QStringLiteral("https://translate.googleapis.com/translate_a/single"
                                       "?client=gtx&sl=%1&tl=%2&dt=t&q=%3")
                            .arg(sl, tl, encoded);

    net::HttpRequest req;
    req.url = QUrl(url);
    req.method = QByteArrayLiteral("GET");
    // 超时偏短：Google 在部分网络下不可达，尽快失败以便降级到下一个适配器。
    // 15s 会让用户干等，5s 足够正常网络完成一次单段翻译。
    req.timeoutMs = 5000;

    const net::HttpResponse resp = http.send(req);
    r.httpStatus = resp.status;

    if (!resp.ok) {
        r.error = QStringLiteral("google: %1")
                      .arg(resp.error.isEmpty() ? QStringLiteral("请求失败") : resp.error);
        return r;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        // 把响应前 200 字节带出来，便于排查（限流页 / 验证码页都长这样）。
        r.error = QStringLiteral("google: 响应解析失败（%1）；响应片段：%2")
                      .arg(perr.errorString(), QString::fromUtf8(resp.body.left(200)));
        return r;
    }

    const QJsonArray root = doc.array();
    if (root.isEmpty() || !root.at(0).isArray()) {
        r.error = QStringLiteral("google: 响应结构异常，缺少 result[0]；响应片段：%1")
                      .arg(QString::fromUtf8(resp.body.left(200)));
        return r;
    }

    // result[0] 是分段数组：[[译文, 原文, ...], [译文, 原文, ...], ...]
    QString merged;
    const QJsonArray segments = root.at(0).toArray();
    for (const QJsonValue &v : segments) {
        const QJsonArray item = v.toArray();
        if (item.isEmpty()) {
            continue;
        }
        const QJsonValue first = item.at(0);
        if (first.isString()) {
            merged += first.toString();
        }
    }

    merged = merged.trimmed();
    if (merged.isEmpty()) {
        r.error = QStringLiteral("google: 译文为空；响应片段：%1")
                      .arg(QString::fromUtf8(resp.body.left(200)));
        return r;
    }

    r.ok = true;
    r.text = merged;
    return r;
}

} // namespace studious::translate
