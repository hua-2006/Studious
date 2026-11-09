#include "llm/LlmCatalog.h"

#include "i18n/L10n.h"
#include "net/IHttpClient.h"
#include "translators/LlmRequest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace studious::llm {
namespace {

// endpoint 拼接、Bearer 头、错误文案全部来自 translators/LlmRequest.h：
// 这个文件只管"模型目录 / 能力"这一层，不再自己拼一遍 HTTP 细节。
// 以前这里有一份 endpointFor() + describeFailure() + 手写 Authorization，
// 与 LlmTranslator 各说各话（同一个 401 两处措辞不同）。

// GET 类请求（/models）：只带 Accept，不带 body。
net::HttpRequest makeRequest(const config::LlmConfig &cfg,
                             const QString &path,
                             int timeoutMs)
{
    net::HttpRequest req;
    req.url = translate::apiUrl(cfg.baseUrl, path);
    req.method = QByteArrayLiteral("GET");
    req.timeoutMs = timeoutMs;
    req.headers.append(qMakePair(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json")));
    translate::appendAuthorization(req, cfg.apiKey);
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
        out.error = translate::describeHttpFailure(resp);
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

    // 探测的正是"关闭思考"这条路：reasoning_effort=none + Anthropic 的 thinking=disabled。
    // 请求体 / 鉴权 / 超时都走翻译用的同一个构造器，否则"翻译能通、探测 401"这类
    // 问题会一直存在（以前这里是各拼一套，只有探测多带了 Accept）。
    translate::ChatRequestOverrides overrides;
    overrides.timeoutMs = 60000;             // 探测固定等 60s，不跟随用户的翻译超时
    overrides.firstByteTimeoutMs = 0;        // 也不设首字节门槛（探测只看最终返回）
    overrides.reasoningEffort = QStringLiteral("none");

    net::HttpRequest req = translate::makeChatRequest(cfg, messages, 256, 0.0, overrides);
    req.headers.append(qMakePair(QByteArrayLiteral("Accept"),
                                 QByteArrayLiteral("application/json")));

    const net::HttpResponse resp = http.send(req);
    if (!resp.ok) {
        out.error = translate::describeHttpFailure(resp);
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
