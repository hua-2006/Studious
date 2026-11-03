#include "llm/LlmCatalog.h"

#include "i18n/L10n.h"
#include "net/IHttpClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace studious::llm {
namespace {

// baseUrl 末尾允许带斜杠，".../v1/" 与 ".../v1" 都要能拼出正确的路径。
QString endpointFor(const QString &baseUrl, const QString &path)
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return base + path;
}

// 把 HTTP 层失败翻译成中文。4xx 优先贴服务端 body——网关通常在那里写明原因，
// 比只给状态码有用（Qt 遇到非 2xx 只会报 "网络错误(203)"，没有细节）。
QString describeFailure(const net::HttpResponse &resp)
{
    switch (resp.status) {
    case 401:
        return i18n::s("API Key 无效或已过期（401）",
                       "The API key is invalid or has expired (401).");
    case 402:
        return i18n::s("账户余额不足（402）", "The account has insufficient credit (402).");
    case 403:
        return i18n::s("无访问权限（403）", "Access denied (403).");
    case 404:
        return i18n::s("接口不存在（404）：请检查 Base URL 是否以 /v1 结尾",
                       "Endpoint not found (404). Check that the base URL ends with /v1.");
    case 429:
        return i18n::s("请求过于频繁（429），请稍后重试",
                       "Too many requests (429). Please try again later.");
    default:
        break;
    }
    if (resp.status >= 500) {
        return i18n::sf("服务端错误（%1）", "Server error (%1).", {resp.status});
    }
    const QString body = QString::fromUtf8(resp.body.left(200)).trimmed();
    if (!body.isEmpty()) {
        return resp.error.isEmpty() ? body
                                    : (resp.error + i18n::s("：", ": ") + body);
    }
    return resp.error.isEmpty() ? i18n::s("请求失败", "The request failed") : resp.error;
}

net::HttpRequest makeRequest(const config::LlmConfig &cfg,
                             const QString &path,
                             int timeoutMs)
{
    net::HttpRequest req;
    req.url = QUrl(endpointFor(cfg.baseUrl, path));
    req.method = QByteArrayLiteral("GET");
    req.timeoutMs = timeoutMs;
    req.headers.append(qMakePair(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json")));
    const QString key = cfg.apiKey.trimmed();
    if (!key.isEmpty()) {
        req.headers.append(qMakePair(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + key.toUtf8()));
    }
    return req;
}

} // namespace

QVector<ReasoningOption> reasoningOptionsFor(const LlmModelInfo &model)
{
    QVector<ReasoningOption> options;

    if (!model.supportsReasoning) {
        // 不支持推理的模型：只给一项，且 value 为空 => 请求里完全不发思考相关字段，
        // 避免严格的网关因为未知字段直接 400。
        ReasoningOption only;
        only.value = QString();
        only.label = i18n::s("不适用（该模型不支持推理模式）",
                             "Not applicable (this model does not support reasoning mode)");
        only.recommended = true;
        options.append(only);
        return options;
    }

    ReasoningOption none;
    none.value = QStringLiteral("none");
    none.label = i18n::s("关闭（推荐，最快）", "Off (recommended, fastest)");
    none.recommended = true;
    options.append(none);

    const QVector<QPair<QString, QString>> rest = {
        {QStringLiteral("minimal"), i18n::s("极低", "Very low")},
        {QStringLiteral("low"), i18n::s("低", "Low")},
        {QStringLiteral("medium"), i18n::s("中", "Medium")},
        {QStringLiteral("high"), i18n::s("高", "High")},
    };
    for (const auto &pair : rest) {
        ReasoningOption option;
        option.value = pair.first;
        option.label = pair.second;
        options.append(option);
    }

    ReasoningOption fallback;
    fallback.value = QString();
    fallback.label = i18n::s("跟随模型默认（最慢）", "Follow the model default (slowest)");
    options.append(fallback);
    return options;
}

int indexOfReasoningOption(const QVector<ReasoningOption> &options, const QString &value)
{
    for (int i = 0; i < options.size(); ++i) {
        if (options.at(i).value == value) {
            return i;
        }
    }
    return 0;
}

ModelListResult listModels(const config::LlmConfig &cfg, net::IHttpClient &http)
{
    ModelListResult out;

    if (cfg.baseUrl.trimmed().isEmpty()) {
        out.error = i18n::s("Base URL 为空，请先填写接口地址",
                            "The base URL is empty. Please fill in the API endpoint first.");
        return out;
    }

    const net::HttpResponse resp = http.send(makeRequest(cfg, QStringLiteral("/models"), 15000));
    if (!resp.ok) {
        out.error = describeFailure(resp);
        return out;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = i18n::sf("响应不是合法 JSON：%1", "The response is not valid JSON: %1",
                             {QString::fromUtf8(resp.body.left(200))});
        return out;
    }

    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty()) {
        out.error = i18n::s("/models 返回的 data 为空（部分网关不支持该接口，请手动填写模型名）",
                            "The /models response contains no data (some gateways do not implement "
                            "this endpoint; please enter the model name manually).");
        return out;
    }

    for (const QJsonValue &value : data) {
        const QJsonObject object = value.toObject();

        LlmModelInfo model;
        model.id = object.value(QStringLiteral("id")).toString().trimmed();
        if (model.id.isEmpty()) {
            continue;
        }
        model.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (model.name.isEmpty()) {
            model.name = model.id;
        }
        model.ownedBy = object.value(QStringLiteral("owned_by")).toString();
        model.kind = object.value(QStringLiteral("kind")).toString();
        model.supportsReasoning = object.value(QStringLiteral("supports_reasoning")).toBool(false);
        model.supportsTools = object.value(QStringLiteral("supports_tool_call")).toBool(false);
        model.maxInputTokens = object.value(QStringLiteral("max_input_tokens")).toInt(0);
        model.maxOutputTokens = object.value(QStringLiteral("max_output_tokens")).toInt(0);

        // 只要聊天模型：图片/嵌入模型拿来翻译没有意义，列出来只会干扰用户。
        if (!model.isChatModel()) {
            continue;
        }
        out.models.append(model);
    }

    if (out.models.isEmpty()) {
        out.error = i18n::s("/models 里没有任何聊天模型",
                            "The /models response contains no chat models.");
        return out;
    }

    out.ok = true;
    return out;
}

ReasoningProbeResult probeReasoning(const config::LlmConfig &cfg, net::IHttpClient &http)
{
    ReasoningProbeResult out;

    if (!cfg.isValid()) {
        out.error = i18n::s("LLM 未配置：Base URL 或模型名为空",
                            "The LLM is not configured: the base URL or the model name is empty.");
        return out;
    }

    QJsonObject sys;
    sys.insert(QStringLiteral("role"), QStringLiteral("system"));
    sys.insert(QStringLiteral("content"), QStringLiteral("你是专业翻译引擎，只输出译文，不要解释。"));

    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"),
                QStringLiteral("将下面 <<< 与 >>> 之间的文本翻译成【中文】，只输出译文。\n\n"
                               "<<<\nHello, world!\n>>>"));

    QJsonArray messages;
    messages.append(sys);
    messages.append(user);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), cfg.model.trimmed());
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("temperature"), 0.0);
    payload.insert(QStringLiteral("max_tokens"), 256);
    payload.insert(QStringLiteral("stream"), false);
    // 探测的正是"关闭思考"这条路：两个字段都带上（实测网关两种都认）。
    payload.insert(QStringLiteral("reasoning_effort"), QStringLiteral("none"));
    QJsonObject thinking;
    thinking.insert(QStringLiteral("type"), QStringLiteral("disabled"));
    payload.insert(QStringLiteral("thinking"), thinking);

    net::HttpRequest req = makeRequest(cfg, QStringLiteral("/chat/completions"), 60000);
    req.method = QByteArrayLiteral("POST");
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.headers.append(qMakePair(QByteArrayLiteral("Content-Type"),
                                 QByteArrayLiteral("application/json")));
    // makeRequest 加的是 Accept，GET/POST 都要 Content-Type，这里补上即可。

    const net::HttpResponse resp = http.send(req);
    if (!resp.ok) {
        out.error = describeFailure(resp);
        return out;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = i18n::sf("响应不是合法 JSON：%1", "The response is not valid JSON: %1",
                             {QString::fromUtf8(resp.body.left(200))});
        return out;
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        out.error = i18n::s("响应里没有 choices", "The response contains no choices.");
        return out;
    }

    const QJsonObject message =
        choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    out.sample = message.value(QStringLiteral("content")).toString().trimmed();
    out.hasReasoningContent =
        !message.value(QStringLiteral("reasoning_content")).toString().trimmed().isEmpty();

    // usage 明细各家写法不同，能拿到就用，拿不到就靠 reasoning_content 判断。
    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    const QJsonObject details =
        usage.value(QStringLiteral("completion_tokens_details")).toObject();
    const bool hasTokenDetail = details.contains(QStringLiteral("reasoning_tokens"));
    out.reasoningTokens = details.value(QStringLiteral("reasoning_tokens")).toInt(0);

    out.ok = true;
    if (out.hasReasoningContent || out.reasoningTokens > 0) {
        out.probe = ReasoningProbe::StillThinking;
    } else if (hasTokenDetail) {
        out.probe = ReasoningProbe::Disabled;
    } else {
        // 没明细但也没有 reasoning_content：大概率已关闭，但不敢打包票。
        out.probe = ReasoningProbe::Unknown;
    }
    return out;
}

} // namespace studious::llm
