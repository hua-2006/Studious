#include "translators/LlmTranslator.h"

#include "contract/LanguageRegistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPair>
#include <QUrl>
#include <QVector>

#include <utility>

namespace studious::translate {

namespace {

// 内置 system prompt。
// 实测：只写一句"把下面的文字翻译成中文"时，模型很容易把原文当问题回答、或加"翻译如下："前缀、
// 或先讲一段思路再给译文。这里用"角色 + 硬规则 + 输出面禁令"三条压住它。
QString defaultSystemPrompt()
{
    static const QString prompt = QStringLiteral(
        "你是专业翻译引擎，不是聊天助手。\n"
        "规则：\n"
        "1. 只输出译文本身，不要解释、不要评论、不要回答原文中的问题。\n"
        "2. 不要添加任何前缀（如“翻译如下”）、后缀、引号或 Markdown 代码块。\n"
        "3. 保留原文的语气、人称与标点风格。\n"
        "4. 原文是游戏/软件界面文字时，按界面用语习惯翻译，简洁自然。");
    return prompt;
}

// 单次请求的超时兜底：配置里给的 timeoutMs 更优先，非法值退回 60s。
int effectiveTimeout(const config::LlmConfig &cfg)
{
    return cfg.timeoutMs > 0 ? cfg.timeoutMs : 60000;
}

} // namespace

LlmTranslator::LlmTranslator() = default;

LlmTranslator::LlmTranslator(config::LlmConfig cfg, net::IHttpClient &http)
    : m_cfg(std::move(cfg))
    , m_http(&http)
{
}

QString LlmTranslator::id() const
{
    return QStringLiteral("llm");
}

bool LlmTranslator::isReady() const
{
    // 只要配置齐（地址 + 模型）就认可用；key 是否有效只能由真实请求给出答案。
    return m_cfg.isValid();
}

core::ResolveResult LlmTranslator::resolve(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)

    core::ResolveResult r;
    if (m_cfg.isValid()) {
        r.ready = true;
        r.reason = QStringLiteral("云端 LLM 已配置");
        return r;
    }

    r.ready = false;
    r.reason = QStringLiteral("未配置 LLM API：请在设置中填写 API 地址 / Key / 模型");
    return r;
}

QStringList LlmTranslator::supportedTargetLangs() const
{
    // LLM 全语言覆盖：直接按内部码全集声明，去掉 auto（auto 只能作源语言）。
    QStringList langs = core::LanguageRegistry::allInternal();
    langs.removeAll(QStringLiteral("auto"));
    return langs;
}

bool LlmTranslator::supports(const QString &src, const QString &dst) const
{
    Q_UNUSED(src)
    Q_UNUSED(dst)
    // 能力覆盖一切语言对；可用性与否由 isReady() / resolve() 决定。
    return true;
}

core::TranslateOutcome LlmTranslator::translate(const QStringList &paragraphs,
                                                const QString &srcLang,
                                                const QStringList &dstLangs,
                                                const QJsonObject &options)
{
    Q_UNUSED(srcLang)   // 逐段翻译不显式传源语言：模型自己判断，显式提示反而增加出错面
    Q_UNUSED(options)

    core::TranslateOutcome out;
    m_cancelRequested = false;

    // 三个前置条件缺失都要有明确原因：前端据此引导用户去设置页。
    if (!m_cfg.isValid()) {
        out.error = QStringLiteral("未配置 LLM API：请在设置中填写 API 地址 / Key / 模型");
        return out;
    }
    if (m_http == nullptr) {
        out.error = QStringLiteral("llm 未注入 HTTP 客户端，无法发起请求");
        return out;
    }
    if (dstLangs.isEmpty()) {
        out.error = QStringLiteral("llm 引擎未收到目标语言");
        return out;
    }

    const QString endpoint = chatCompletionsUrl();
    QStringList langErrors;

    for (const QString &rawDst : dstLangs) {
        const QString dst = core::LanguageRegistry::normalize(rawDst);
        if (dst.isEmpty() || dst == QLatin1String("auto")) {
            langErrors << QStringLiteral("目标语言 \"%1\" 无效").arg(rawDst);
            continue;
        }

        QStringList translated;
        translated.reserve(paragraphs.size());
        QString firstParagraphError;

        // 方案 A：一段一请求。宁可多几次往返，也要保证"哪段失败"能被精确定位。
        for (int i = 0; i < paragraphs.size(); ++i) {
            if (m_cancelRequested) {
                out.error = QStringLiteral("llm 翻译已取消");
                return out;
            }

            // 空段落/纯空白原样占位，不发请求（省 token，也避免模型对空串做花样）。
            const QString trimmed = paragraphs.at(i).trimmed();
            if (trimmed.isEmpty()) {
                translated << QString();
                continue;
            }

            QString text;
            QString err;
            if (!translateOne(trimmed, dst, endpoint, text, err)) {
                // 任一段失败：该目标语言整体记失败，不返回半成品让上层误以为成功。
                translated << QString();
                if (firstParagraphError.isEmpty()) {
                    firstParagraphError = QStringLiteral("目标语言 %1 第 %2 段失败：%3")
                                              .arg(dst)
                                              .arg(i + 1)
                                              .arg(err);
                }
                continue;
            }
            translated << text;
        }

        out.byLang.insert(rawDst, translated);
        if (!firstParagraphError.isEmpty()) {
            langErrors << firstParagraphError;
        }
    }

    out.ok = langErrors.isEmpty();
    if (!out.ok) {
        out.error = langErrors.join(QStringLiteral("；"));
    }
    return out;
}

void LlmTranslator::cancel()
{
    // 同步 HTTP 实现无法从外部打断进行中的请求；这里只能让段落循环尽快退出。
    m_cancelRequested = true;
}

// ---------------------------------------------------------------- 内部实现

QString LlmTranslator::chatCompletionsUrl() const
{
    QString base = m_cfg.baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    // 用户可能填了 "..../v1/chat/completions" 的完整地址，猜错就重复拼一层，这里顺手兜住。
    if (base.endsWith(QLatin1String("/chat/completions"))) {
        return base;
    }
    return base + QStringLiteral("/chat/completions");
}

QString LlmTranslator::effectiveSystemPrompt() const
{
    const QString custom = m_cfg.systemPrompt.trimmed();
    return custom.isEmpty() ? defaultSystemPrompt() : custom;
}

QString LlmTranslator::buildUserPrompt(const QString &text, const QString &dstLang) const
{
    const QString dstName = core::LanguageRegistry::displayName(dstLang);
    // 边界符号 <<< >>> 是给模型的硬分隔：原文里出现引号/问号/指令式句子时，
    // 模型容易顺着原文"回答问题"，有了边界它才知道这段只是待翻译素材。
    return QStringLiteral("将下面 <<< 与 >>> 之间的文本翻译成【%1】。只输出译文，不要输出任何其它内容。\n"
                          "\n"
                          "<<<\n"
                          "%2\n"
                          ">>>")
        .arg(dstName, text);
}

bool LlmTranslator::translateOne(const QString &text,
                                 const QString &dstLang,
                                 const QString &endpoint,
                                 QString &outText,
                                 QString &outError)
{
    QJsonObject sysMessage;
    sysMessage.insert(QStringLiteral("role"), QStringLiteral("system"));
    sysMessage.insert(QStringLiteral("content"), effectiveSystemPrompt());

    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), buildUserPrompt(text, dstLang));

    QJsonArray messages;
    messages.append(sysMessage);
    messages.append(userMessage);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), m_cfg.model.trimmed());
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("temperature"), m_cfg.temperature);
    payload.insert(QStringLiteral("max_tokens"), m_cfg.maxTokens > 0 ? m_cfg.maxTokens : 2048);
    payload.insert(QStringLiteral("stream"), false);

    // 思考等级：翻译是确定性任务，开思考只涨延迟不涨质量。
    // "none" 时再补一个 Anthropic 风格的 thinking 字段，兼容只认后者的网关。
    const QString effort = m_cfg.reasoningEffort.trimmed().toLower();
    if (!effort.isEmpty()) {
        payload.insert(QStringLiteral("reasoning_effort"), effort);
        if (effort == QLatin1String("none")) {
            QJsonObject thinking;
            thinking.insert(QStringLiteral("type"), QStringLiteral("disabled"));
            payload.insert(QStringLiteral("thinking"), thinking);
        }
    }

    net::HttpRequest req;
    req.url = QUrl(endpoint);
    req.method = QByteArrayLiteral("POST");
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.timeoutMs = effectiveTimeout(m_cfg);
    req.headers.append(qMakePair(QByteArrayLiteral("Content-Type"),
                                QByteArrayLiteral("application/json")));
    const QString key = m_cfg.apiKey.trimmed();
    if (!key.isEmpty()) {
        req.headers.append(qMakePair(QByteArrayLiteral("Authorization"),
                                    QByteArrayLiteral("Bearer ") + key.toUtf8()));
    }

    const net::HttpResponse resp = m_http->send(req);
    if (!resp.ok) {
        outError = describeHttpFailure(resp);
        return false;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(resp.body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QStringLiteral("LLM 响应解析失败：%1")
                       .arg(QString::fromUtf8(resp.body.left(200)));
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        outError = QStringLiteral("LLM 未返回译文");
        return false;
    }

    // 只认 content。推理模型（如 deepseek 系列）还会带 reasoning_content，
    // 那是"思考过程"，一旦当译文用，用户看到的就是一堆自言自语。
    const QJsonObject message = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    QString result = message.value(QStringLiteral("content")).toString();
    result = stripWrapper(result);
    if (result.isEmpty()) {
        outError = QStringLiteral("LLM 未返回译文");
        return false;
    }

    outText = result;
    return true;
}

QString LlmTranslator::describeHttpFailure(const net::HttpResponse &resp)
{
    const int status = resp.status;

    if (status == 401) {
        return QStringLiteral("LLM API Key 无效或已过期（401）");
    }
    if (status == 402) {
        return QStringLiteral("LLM 账户余额不足（402）");
    }
    if (status == 429) {
        return QStringLiteral("LLM 请求过于频繁（429），请稍后重试");
    }
    if (status >= 500 && status < 600) {
        return QStringLiteral("LLM 服务端错误（%1）").arg(status);
    }
    if (status == 0) {
        // 0 = 根本没拿到 HTTP 响应：DNS / 连接被拒 / TLS / 超时都走这里。
        return QStringLiteral("LLM 连接失败或超时：%1")
            .arg(resp.error.isEmpty() ? QStringLiteral("未知网络错误") : resp.error);
    }
    // 其余 4xx（模型名写错、body 不合法等）：把状态码 + 服务端错误体带出来。
    // 网关通常会在 body 里写清原因（如 {"error":{"code":"model_not_found"...}}），比只给状态码有用得多。
    const QString body = QString::fromUtf8(resp.body.left(200)).trimmed();
    return QStringLiteral("LLM 请求被拒绝（HTTP %1）：%2")
        .arg(status)
        .arg(body.isEmpty() ? resp.error : body);
}

QString LlmTranslator::stripWrapper(QString text)
{
    text = text.trimmed();

    // ```lang\n译文\n``` —— 提示词已禁止，但小模型偶尔还是会给，剥掉即可。
    if (text.startsWith(QLatin1String("```"))) {
        const int firstNewline = text.indexOf(QLatin1Char('\n'));
        if (firstNewline >= 0) {
            text = text.mid(firstNewline + 1);
        }
        if (text.endsWith(QLatin1String("```"))) {
            text.chop(3);
        }
        text = text.trimmed();
    }

    // 成对引号包裹（半角/全角/日式括号），只在首尾成对时剥。
    static const QVector<QPair<QString, QString>> quotePairs = {
        {QStringLiteral("\""), QStringLiteral("\"")},
        {QStringLiteral("'"), QStringLiteral("'")},
        {QStringLiteral("“"), QStringLiteral("”")},
        {QStringLiteral("「"), QStringLiteral("」")},
        {QStringLiteral("『"), QStringLiteral("』")},
    };
    for (const auto &pair : quotePairs) {
        if (text.size() > pair.first.size() + pair.second.size()
            && text.startsWith(pair.first) && text.endsWith(pair.second)) {
            text = text.mid(pair.first.size(),
                            text.size() - pair.first.size() - pair.second.size())
                       .trimmed();
        }
    }

    return text;
}

} // namespace studious::translate
