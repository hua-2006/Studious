#include "translators/LlmTranslator.h"

#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"

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
        r.reason = i18n::s("云端 LLM 已配置", "The cloud LLM is configured");
        return r;
    }

    r.ready = false;
    r.reason = i18n::s("未配置 LLM API：请在设置中填写 API 地址 / Key / 模型",
                       "LLM API is not configured. Please set the API URL, key and model in "
                       "Settings.");
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
        out.error = i18n::s("未配置 LLM API：请在设置中填写 API 地址 / Key / 模型",
                            "LLM API is not configured. Please set the API URL, key and model in "
                            "Settings.");
        return out;
    }
    if (m_http == nullptr) {
        out.error = i18n::s("llm 未注入 HTTP 客户端，无法发起请求",
                            "The llm engine has no HTTP client, so it cannot send requests.");
        return out;
    }
    if (dstLangs.isEmpty()) {
        out.error = i18n::s("llm 引擎未收到目标语言", "The llm engine received no target language.");
        return out;
    }

    const QString endpoint = chatCompletionsUrl();
    QStringList langErrors;

    for (const QString &rawDst : dstLangs) {
        const QString dst = core::LanguageRegistry::normalize(rawDst);
        if (dst.isEmpty() || dst == QLatin1String("auto")) {
            langErrors << i18n::sf("目标语言 \"%1\" 无效", "Invalid target language \"%1\"",
                                   {rawDst});
            continue;
        }

        // 结果按原下标存，空段落原样留空——批量翻译后仍要能一一对回原文位置。
        QStringList translated;
        translated.reserve(paragraphs.size());
        QString firstParagraphError;
        QVector<int> pending;   // 需要真正翻译的段落下标（跳过空段/纯空白）
        for (int i = 0; i < paragraphs.size(); ++i) {
            translated << QString();
            if (!paragraphs.at(i).trimmed().isEmpty()) {
                pending.append(i);
            }
        }

        // 分块策略：延迟几乎全部来自"每次请求固定的上游首字延迟"（实测约 1.1s，
        // 与 token 数无关），所以把 N 段合并成一次请求收益极大：
        // 实测 5 段 5662ms -> 1034ms。batchSize <= 1 时退回逐段。
        const int batchSize = m_cfg.batchSize > 1 ? m_cfg.batchSize : 1;
        int cursor = 0;
        while (cursor < pending.size()) {
            if (m_cancelRequested) {
                out.error = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
                return out;
            }

            const int take = qMin(batchSize, pending.size() - cursor);
            const QVector<int> chunk = pending.mid(cursor, take);
            cursor += take;

            QStringList texts;
            texts.reserve(chunk.size());
            for (int index : chunk) {
                texts << paragraphs.at(index).trimmed();
            }

            if (texts.size() == 1) {
                QString one;
                QString err;
                if (!translateOne(texts.first(), dst, endpoint, one, err)) {
                    if (firstParagraphError.isEmpty()) {
                        firstParagraphError =
                            i18n::sf("目标语言 %1 第 %2 段失败：%3",
                                     "Target language %1, paragraph %2 failed: %3",
                                     {dst, chunk.first() + 1, err});
                    }
                } else {
                    translated[chunk.first()] = one;
                }
                continue;
            }

            QStringList results;
            QString batchError;
            if (translateBatch(texts, dst, endpoint, results, batchError)
                && results.size() == texts.size()) {
                for (int k = 0; k < chunk.size(); ++k) {
                    translated[chunk.at(k)] = results.at(k);
                }
                continue;
            }

            // 批量失败（格式漂移/段落数不符）：回退到逐段，把问题定位到具体段落。
            // 这样批量只是"加速手段"，不会成为新的失败点。
            for (int k = 0; k < chunk.size(); ++k) {
                QString one;
                QString err;
                if (translateOne(texts.at(k), dst, endpoint, one, err)) {
                    translated[chunk.at(k)] = one;
                } else if (firstParagraphError.isEmpty()) {
                    firstParagraphError = i18n::sf("目标语言 %1 第 %2 段失败：%3",
                                                   "Target language %1, paragraph %2 failed: %3",
                                                   {dst, chunk.at(k) + 1, err});
                }
            }
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

QString LlmTranslator::cacheTag() const
{
    // 模型或网关一变，同一句原文的译文可能完全不同，缓存必须分开。
    return QStringLiteral("%1|%2|%3").arg(id(), m_cfg.model.trimmed(), m_cfg.baseUrl.trimmed());
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
    Q_UNUSED(endpoint)   // 端点已在 postChat 内部按配置拼接

    QString content;
    QString err;
    const int maxTokens = m_cfg.maxTokens > 0 ? m_cfg.maxTokens : 2048;
    if (!postChat(effectiveSystemPrompt(), buildUserPrompt(text, dstLang),
                  m_cfg.temperature, maxTokens, content, err)) {
        outError = err;
        return false;
    }
    if (content.isEmpty()) {
        outError = i18n::s("LLM 未返回译文", "The LLM returned no translation.");
        return false;
    }
    outText = content;
    return true;
}

bool LlmTranslator::postChat(const QString &systemPrompt,
                             const QString &userPrompt,
                             double temperature,
                             int maxTokens,
                             QString &outContent,
                             QString &outError)
{
    QJsonObject sysMessage;
    sysMessage.insert(QStringLiteral("role"), QStringLiteral("system"));
    sysMessage.insert(QStringLiteral("content"), systemPrompt);

    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), userPrompt);

    QJsonArray messages;
    messages.append(sysMessage);
    messages.append(userMessage);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), m_cfg.model.trimmed());
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("temperature"), temperature);
    payload.insert(QStringLiteral("max_tokens"), maxTokens);
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
    req.url = QUrl(chatCompletionsUrl());
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
        outError = i18n::sf("LLM 响应解析失败：%1",
                            "Failed to parse the LLM response: %1",
                            {QString::fromUtf8(resp.body.left(200))});
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        outError = i18n::s("LLM 未返回译文", "The LLM returned no translation.");
        return false;
    }

    // 只认 content。推理模型（如 deepseek 系列）还会带 reasoning_content，
    // 那是"思考过程"，一旦当译文用，用户看到的就是一堆自言自语。
    const QJsonObject message = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    outContent = stripWrapper(message.value(QStringLiteral("content")).toString());
    return true;
}

bool LlmTranslator::parseJsonStringArray(const QString &content, int expected, QStringList &out)
{
    QString s = content.trimmed();

    // 模型常把 JSON 包在 ```json ... ``` 里，先剥壳。
    if (s.startsWith(QLatin1String("```"))) {
        const int firstNewline = s.indexOf(QLatin1Char('\n'));
        if (firstNewline > 0) {
            s = s.mid(firstNewline + 1);
        }
        if (s.endsWith(QLatin1String("```"))) {
            s.chop(3);
        }
        s = s.trimmed();
    }

    // 也可能前面带一句"好的，以下是翻译："，取首个 [ 到最后一个 ]。
    const int open = s.indexOf(QLatin1Char('['));
    const int close = s.lastIndexOf(QLatin1Char(']'));
    if (open < 0 || close <= open) {
        return false;
    }
    s = s.mid(open, close - open + 1);

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }

    const QJsonArray array = doc.array();
    if (array.size() != expected) {
        return false;   // 段落数不符：宁可整体回退，也不要错位回填
    }

    out.clear();
    for (const QJsonValue &value : array) {
        if (!value.isString()) {
            return false;
        }
        const QString text = value.toString().trimmed();
        if (text.isEmpty()) {
            return false;
        }
        out << text;
    }
    return true;
}

bool LlmTranslator::translateBatch(const QStringList &texts,
                                   const QString &dstLang,
                                   const QString &endpoint,
                                   QStringList &outTexts,
                                   QString &outError)
{
    Q_UNUSED(endpoint)

    const QString dstName = core::LanguageRegistry::displayName(dstLang);

    QString body;
    for (int i = 0; i < texts.size(); ++i) {
        body += QStringLiteral("[%1] %2\n").arg(i + 1).arg(texts.at(i));
    }

    // 批量专用 system prompt：与单段的不同，硬要求"只输出 JSON 数组且数量一致"。
    const QString system = QStringLiteral(
        "你是专业翻译引擎。只输出一个 JSON 字符串数组，数组元素是每条原文的译文，"
        "顺序与输入一一对应，元素数量必须与输入条数完全相同。"
        "不要输出任何解释、前言、注释、代码块标记或其它内容。");

    const QString user = QStringLiteral(
                             "把下面每条翻译成【%1】，返回 JSON 字符串数组（长度必须为 %2）。\n"
                             "只输出 JSON，不要任何其它内容。\n\n%3")
                             .arg(dstName)
                             .arg(texts.size())
                             .arg(body);

    // 批量要装下所有段的译文，按段数给足额度，避免被 max_tokens 截断。
    const int maxTokens = qBound(1024, texts.size() * 128, 8192);

    QString content;
    QString err;
    if (!postChat(system, user, 0.1, maxTokens, content, err)) {
        outError = err;
        return false;
    }
    if (parseJsonStringArray(content, texts.size(), outTexts)) {
        return true;
    }

    // 首次格式漂移很常见（加前言/包代码块）：用更低温度重试一次，并加重语气。
    QString retryContent;
    QString retryErr;
    const QString retryUser =
        user + QStringLiteral("\n\n再次强调：只输出 JSON 数组，长度恰为 %1，不要任何其它文字。").arg(texts.size());
    if (postChat(system, retryUser, 0.0, maxTokens, retryContent, retryErr)
        && parseJsonStringArray(retryContent, texts.size(), outTexts)) {
        return true;
    }

    outError = i18n::sf("批量译文格式不符（期望 %1 条）：%2",
                        "The batch reply was malformed (expected %1 items): %2",
                        {texts.size(), content.left(200)});
    return false;
}

QString LlmTranslator::describeHttpFailure(const net::HttpResponse &resp)
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
