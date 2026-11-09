#include "translators/LlmRequest.h"

#include "i18n/L10n.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace studious::translate {

namespace {

// 单次请求的超时兜底：配置里给的 timeoutMs 更优先，非法值退回 60s。
int effectiveTimeout(const config::LlmConfig &cfg)
{
    return cfg.timeoutMs > 0 ? cfg.timeoutMs : 60000;
}

} // namespace

QUrl apiUrl(const QString &baseUrl, const QString &path)
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + path);
}

QString chatCompletionsUrl(const QString &baseUrl)
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    // 用户可能填了 "…/v1/chat/completions" 的完整地址，猜错就重复拼一层，这里顺手兜住。
    if (base.endsWith(QLatin1String("/chat/completions"))) {
        return base;
    }
    return base + QStringLiteral("/chat/completions");
}

void appendAuthorization(net::HttpRequest &req, const QString &apiKey)
{
    const QString key = apiKey.trimmed();
    if (key.isEmpty()) {
        return;   // 本地 llama-server 之类不需要鉴权
    }
    req.headers.append(qMakePair(QByteArrayLiteral("Authorization"),
                                 QByteArrayLiteral("Bearer ") + key.toUtf8()));
}

net::HttpRequest makeChatRequest(const config::LlmConfig &cfg,
                                 const QJsonArray &messages,
                                 int maxTokens,
                                 double temperature,
                                 const ChatRequestOverrides &overrides)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("model"), cfg.model.trimmed());
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("temperature"), temperature);
    payload.insert(QStringLiteral("max_tokens"), maxTokens);
    payload.insert(QStringLiteral("stream"), false);

    // 思考等级：翻译是确定性任务，开思考只涨延迟不涨质量。
    // "none" 时再补一个 Anthropic 风格的 thinking 字段，兼容只认后者的网关。
    // isNull() 才表示"没指定"：显式空串是"别发这个字段"（跟随服务端默认）。
    const QString effort = overrides.reasoningEffort.isNull()
                               ? cfg.reasoningEffort.trimmed().toLower()
                               : overrides.reasoningEffort.trimmed().toLower();
    if (!effort.isEmpty()) {
        payload.insert(QStringLiteral("reasoning_effort"), effort);
        if (effort == QLatin1String("none")) {
            QJsonObject thinking;
            thinking.insert(QStringLiteral("type"), QStringLiteral("disabled"));
            payload.insert(QStringLiteral("thinking"), thinking);
        }
    }

    net::HttpRequest req;
    req.url = QUrl(chatCompletionsUrl(cfg.baseUrl));
    req.method = QByteArrayLiteral("POST");
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.timeoutMs = overrides.timeoutMs > 0 ? overrides.timeoutMs : effectiveTimeout(cfg);
    req.firstByteTimeoutMs = overrides.firstByteTimeoutMs;
    req.headers.append(qMakePair(QByteArrayLiteral("Content-Type"),
                                 QByteArrayLiteral("application/json")));
    appendAuthorization(req, cfg.apiKey);

    return req;
}

QString describeHttpFailure(const net::HttpResponse &resp)
{
    const int status = resp.status;

    if (status == 401) {
        return i18n::s("LLM API Key 无效或已过期（401）",
                       "The LLM API key is invalid or has expired (401).");
    }
    if (status == 402) {
        return i18n::s("LLM 账户余额不足（402）",
                       "The LLM account has insufficient credit (402).");
    }
    if (status == 403) {
        return i18n::s("LLM 无访问权限（403）", "The LLM denied access (403).");
    }
    if (status == 404) {
        // 本地 MT 的目录名与网关路径无关，这条只对 HTTP 端点有意义。
        return i18n::s("LLM 接口不存在（404）：请检查 Base URL 是否以 /v1 结尾",
                       "The LLM endpoint was not found (404). Check that the base URL ends "
                       "with /v1.");
    }
    if (status == 429) {
        return i18n::s("LLM 请求过于频繁（429），请稍后重试",
                       "Too many LLM requests (429). Please try again later.");
    }
    if (status >= 500 && status < 600) {
        return i18n::sf("LLM 服务端错误（%1）", "The LLM service failed with HTTP %1.", {status});
    }
    if (status == 0) {
        // 0 = 根本没拿到 HTTP 响应：DNS / 连接被拒 / TLS / 超时都走这里。
        return i18n::sf("LLM 连接失败或超时：%1",
                        "Could not connect to the LLM (failure or timeout): %1",
                        {resp.error.isEmpty()
                             ? i18n::s("未知网络错误", "unknown network error")
                             : resp.error});
    }
    // 其余 4xx（模型名写错、body 不合法等）：把状态码 + 服务端错误体带出来。
    // 网关通常会在 body 里写清原因（如 {"error":{"code":"model_not_found"...}}），比只给状态码有用得多。
    const QString body = QString::fromUtf8(resp.body.left(200)).trimmed();
    return i18n::sf("LLM 请求被拒绝（HTTP %1）：%2",
                    "The LLM rejected the request (HTTP %1): %2",
                    {status, body.isEmpty() ? resp.error : body});
}

} // namespace studious::translate
