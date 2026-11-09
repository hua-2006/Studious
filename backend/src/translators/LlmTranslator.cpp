#include "translators/LlmTranslator.h"

#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"
#include "i18n/LanguageDetector.h"
#include "translators/LlmRequest.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPair>
#include <QSet>
#include <QElapsedTimer>
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
        "4. 原文是游戏/软件界面文字时，按界面用语习惯翻译，简洁自然。\n"
        "5. 不要输出语言名称、括号注释、说明或任何附加文字。\n"
        "6. 必须真正翻译成目标语言。即使原文很短、像专有名词、或看起来无需翻译，"
        "也要给出规范的目标语言译文，**禁止原样照抄原文**。");
    return prompt;
}

// 超时 / 首字节上限 / 请求体构造都在 translators/LlmRequest.h（唯一一份），
// 这里只保留"翻译"自己的东西：prompt 与结果校验。

// 复读检测。小模型（0.5B 级）偶发把同一短语重复上百次，
// 输出可以涨到几千字、耗时飙到七八秒。
//
// 判据二选一：
//   1. 输出长度远超原文（> 4 倍 + 120 字符）—— 正常译文不会这样
//   2. 末尾 12 个字符在全文里出现 ≥ 4 次 —— 典型的"卡住复读"
int trailingRepeatUnit(const QString &text, int minRepeats, int maxUnit);

bool looksDegenerate(const QString &output, const QString &source)
{
    if (output.size() > source.size() * 4 + 120) {
        return true;
    }
    if (output.size() < 40) {
        return false;
    }

    // 碎片化：小模型退化时会吐成 "quick に くしゃ が こ う そ に …" 这种
    // 大量单字符片段。正常中文/日文不用空格，英文不会把单词拆成单字母。
    const QStringList pieces = output.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (pieces.size() >= 8) {
        int single = 0;
        for (const QString &piece : pieces) {
            if (piece.size() <= 1) {
                ++single;
            }
        }
        if (single * 100 / pieces.size() >= 35) {
            return true;
        }
    }

    // 结尾连续复读：同一个短单元连着来 ≥3 次，而且输出明显比原文长一截，
    // 才算退化。只判"结尾连续"是因为全文计数会误伤合法重复。
    const int unit = trailingRepeatUnit(output, 3, 24);
    return unit > 0 && output.size() > source.size() * 2;
}

// 结尾是否由同一个短单元**连续**重复 —— 返回该单元长度（0 = 没重复）。
//
// 只看「紧邻结尾是不是同一段文字连着来了 N 次」，不看全文出现过几次：全文计数会把
// 合法重复也算进去（实测把 806 字的 "hello world. " × 62 的译文截成了两句）。
int trailingRepeatUnit(const QString &text, int minRepeats, int maxUnit)
{
    const int n = text.size();
    const QString last = n >= 1 ? text.right(qMin(maxUnit, n)) : QString();
    Q_UNUSED(last)

    for (int unit = 1; unit <= maxUnit; ++unit) {
        if (unit * minRepeats > n) {
            break;
        }
        const QString block = text.mid(n - unit, unit);
        if (block.trimmed().isEmpty()) {
            continue;
        }
        bool same = true;
        for (int k = 1; k < minRepeats && same; ++k) {
            const int start = n - unit * (k + 1);
            if (start < 0 || text.mid(start, unit) != block) {
                same = false;
            }
        }
        if (same) {
            return unit;
        }
    }
    return 0;
}

// 把复读尾巴截掉：只留重复段的第一份，其余丢掉。
// 源文本身就有这么长时不截 —— 说明模型的输出体量是合理的，重复是原文自带的。
QString truncateRepetition(const QString &output, const QString &source)
{
    if (output.size() < 40) {
        return output;
    }
    const int unit = trailingRepeatUnit(output, 3, 24);
    if (unit <= 0) {
        return output;
    }
    if (source.size() * 2 >= output.size()) {
        return output;
    }

    const QString block = output.mid(output.size() - unit, unit);
    int start = output.size() - unit;
    while (start - unit >= 0 && output.mid(start - unit, unit) == block) {
        start -= unit;
    }
    return output.left(start + unit).trimmed();
}

// 判断文本里是否含中日韩字符。
//
// 用途：目标语言是拉丁语系（英/法/德…）时，译文里不该再出现中日韩文字。
// 一旦出现，说明模型没翻完或直接照抄 —— 这是 0.5B 级模型最常见的失败形态。
bool containsCjk(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3040 && u <= 0x30FF)
            || (u >= 0xAC00 && u <= 0xD7AF)) {
            return true;
        }
    }
    return false;
}

bool targetUsesCjk(const QString &dstLang)
{
    return dstLang == QLatin1String("zh") || dstLang == QLatin1String("zh-TW")
           || dstLang == QLatin1String("ja") || dstLang == QLatin1String("ko");
}

// 目标语言是日文时，正常译文大多含假名 —— 但纯汉字也是合法日文（「東京」「情報」），
// 所以这个函数只回答"有没有假名"，"像不像日文"由 matchesTargetScript 按「假名 or 汉字」判。
bool hasKana(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x3040 && u <= 0x30FF) {
            return true;
        }
    }
    return false;
}

bool hasHangul(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0xAC00 && u <= 0xD7AF) {
            return true;
        }
    }
    return false;
}

// 是否含汉字（假名、谚文不算）。日文只写汉字也是合法输出，所以要单独判。
bool hasHanzi(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

// 一个字母都没有（纯数字 / 纯符号 / 纯空白）。
// 这类段落"翻译"就是原样保留：不该进"照抄原文"判定，也不该触发补救请求。
bool hasNoLetters(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.isLetter()) {
            return false;
        }
    }
    return true;
}

// 判断输出是否"像"目标语言。
//
// 这是业务底线：宁可报错让用户换模型，也不要把意大利语当成中文显示出来。
// 判据只用字符集（不引语言检测模型），所以对拉丁语系之间（英/法/德/意）
// 无法区分——那超出了字符集能判断的范围，需要真正的语言识别。
//
// 但"字符集不符"只在**有足够信息**时才成立：
//   1. 没有任何字母的串（"3.14"、"-->"、"♪"）没有语种可言，放行；
//   2. 极短输出（"OK"、"東京"、"Wi-Fi"）常是缩写/专有名词，原样保留就是正确答案；
//   3. 日文允许纯汉字。
// 以前这里要求"拉丁目标必须含拉丁字母""日文必须含假名"，于是 "3.14"/"OK"/「東京」
// 被判语种不符，进而触发补救请求（每段最多 5 次）—— 全是白花钱。
// 真正的"没翻译"由 isUntranslatedOutput()（译文 == 原文）单独判，那条才是硬失败。
bool matchesTargetScript(const QString &text, const QString &dstLang)
{
    int latinLetters = 0;
    int allLetters = 0;
    for (const QChar ch : text) {
        if (ch.isLetter()) {
            ++allLetters;
            if (ch.unicode() < 0x0250) {
                ++latinLetters;
            }
        }
    }

    // 1. 没有字母：纯数字/符号/emoji，无所谓"语种"。
    if (allLetters == 0) {
        return true;
    }

    // 2. 极短输出不判语种（阈值 4 是实测尺度："OK"、"PDF"、"東京"、"Wi-Fi" 都在内）。
    if (text.trimmed().size() <= 4) {
        return true;
    }

    // 目标语言是中日韩，却有一半以上是拉丁字母 —— 说明只翻了半句
    // （实测 0.5B/1.5B 英文→日文会输出 "quick brown foxがさぼったogeを跳び越える。"）。
    const bool tooMuchLatin = latinLetters * 100 / allLetters > 50;

    if (dstLang == QLatin1String("zh") || dstLang == QLatin1String("zh-TW")) {
        return containsCjk(text) && !tooMuchLatin;
    }
    if (dstLang == QLatin1String("ja")) {
        // 3. 日文允许纯汉字（「東京」「情報」），不再强制必须有假名。
        return (hasKana(text) || hasHanzi(text)) && !tooMuchLatin;
    }
    if (dstLang == QLatin1String("ko")) {
        return hasHangul(text) && !tooMuchLatin;
    }
    // 拉丁语系（en/fr/de/es/it/pt…）：不得整段残留中日韩文字即可。
    // 不再要求"必须含拉丁字母"——数字/符号串与短串已由上面两条放行。
    return !containsCjk(text);
}

// 缓存标签里的地址归一化：回环地址去掉端口。
// 本地 llama-server 每次启动都是动态端口，端口进了 key 会让本地引擎的缓存永远不命中；
// 云端网关的端口可能区分不同服务，所以只动回环地址，其它地址原样保留（也保住已有缓存命中率）。
QString cacheTagBaseUrl(const QString &baseUrl)
{
    const QString trimmed = baseUrl.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const QUrl url(trimmed);
    const QString scheme = url.scheme();
    const QString host = url.host();
    const bool loopback = host.compare(QLatin1String("127.0.0.1"), Qt::CaseInsensitive) == 0
        || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
        || host == QLatin1String("::1");
    if (!loopback || scheme.isEmpty()) {
        // 非回环地址：完整保留原串（含端口与路径），避免打乱已有缓存。
        return trimmed;
    }

    // QUrl::host() 取出的 IPv6 不带方括号，这里还原成 scheme://[::1] 的形式。
    if (host.contains(QLatin1Char(':'))) {
        return scheme + QStringLiteral("://[") + host + QLatin1Char(']');
    }
    return scheme + QStringLiteral("://") + host;
}

// baseUrl 是否指向本机（本地 llama-server / 内置网关）。
// 判定口径与 cacheTagBaseUrl() 保持一致：127.0.0.1 / localhost / ::1。
bool isLoopbackBaseUrl(const QString &baseUrl)
{
    const QUrl url(baseUrl.trimmed());
    const QString host = url.host();
    return host.compare(QLatin1String("127.0.0.1"), Qt::CaseInsensitive) == 0
           || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
           || host == QLatin1String("::1");
}

// utf8 的 Sha1 前 8 个 hex 字符（= 32 bit）。
//
// 用途：把"模型名 / 一组采样参数"压成稳定又短的一段，直接拼进缓存键。
// 8 位对这种小基数足够区分，也不会把 gguf 路径、整段 system prompt 塞进文件名。
QString shortHash(const QString &text)
{
    const QByteArray digest =
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex();
    return QString::fromLatin1(digest.left(8));
}

// 语义白名单摘要：只把**真正影响译文**的参数并进缓存键。
//
// 刻意排除 batchSize / timeoutMs / retryBudgetMs / baseUrl 端口这类传输参数——
// 调并发、加超时、换本地端口都不该让已翻好的译文整盘作废。
//
// 入参用 effectiveSystemPrompt() 的结果而不是原始 m_cfg.systemPrompt：
// "留空"和"显式填了默认那句"对模型来说完全等价，不该算出两个 key。
QString semanticHashPart(const config::LlmConfig &cfg, const QString &systemPrompt)
{
    // 用 \x1f（单元分隔符）串联，避免各字段拼起来有歧义（"a"+"bc" 与 "ab"+"c"）。
    QString material;
    material.reserve(systemPrompt.size() + 96);
    material += QStringLiteral("sys=");
    material += systemPrompt;
    material += QLatin1Char('\x1f');
    // 浮点用 'g' 输出：0.2 就写 "0.2"，不会因为二进制表示差异算出两个 key。
    material += QStringLiteral("t=") + QString::number(cfg.temperature, 'g', 10);
    material += QLatin1Char('\x1f');
    // 与 postChat() 的归一化一致（trim + 小写）：大小写不同不算两种配置。
    material += QStringLiteral("r=") + cfg.reasoningEffort.trimmed().toLower();
    material += QLatin1Char('\x1f');
    material += QStringLiteral("m=") + QString::number(cfg.maxTokens);
    material += QLatin1Char('\x1f');
    material += QStringLiteral("n=")
        + (cfg.stripModelNotes ? QStringLiteral("1") : QStringLiteral("0"));
    return shortHash(material);
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
    m_activeSrcLang = srcLang;
    Q_UNUSED(options)

    core::TranslateOutcome out;

    // 取消标志不在这里清：清零权归 Router（每次请求开始清一次，见 ICancelControl.h）。
    // 以前各引擎在自己的 translate() 入口清，降级边界上到达的取消会被下一个引擎吞掉。

    // 预算时钟在这里起一次，整次 translate() 共用（所有段落、所有目标语言）。
    // 以前 translateOne 内部各自 start()，于是"每段都有 30s 预算"：2 段 × 20s = 40s
    // 早就超了默认预算还在跑，界面看起来像卡死。
    m_retryClock.start();

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
        bool cancelled = false;
        while (cursor < pending.size()) {
            if (m_cancelRequested) {
                cancelled = true;
                break;
            }

            // 整个调用的预算已用完：不再开新块。已经翻好的段落保留在 translated 里，
            // out.ok 会因为 firstParagraphError 变成失败并带上明确原因。
            //
            // 这是"两次请求之间"的检查：正在飞的那一次请求没法被预算打断
            // （同步 HTTP 实现没有外部打断能力），单次上限由 timeoutMs 兜。
            if (cursor > 0 && retryBudgetExhausted()) {
                if (firstParagraphError.isEmpty()) {
                    firstParagraphError =
                        i18n::sf("目标语言 %1 超出重试时间预算（%2 ms），剩余段落已跳过",
                                 "Target language %1 exceeded the retry budget (%2 ms); "
                                 "the remaining paragraphs were skipped",
                                 {dst, m_cfg.retryBudgetMs});
                }
                break;
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
                if (!translateOne(texts.first(), dst, one, err)) {
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
            const bool batchOk = translateBatch(texts, dst, results, batchError)
                                 && results.size() == texts.size();
            if (batchOk) {
                // 批量给回来的结果里，空串表示"这一条被判定可疑"（其余照用）。
                for (int k = 0; k < chunk.size(); ++k) {
                    if (!results.at(k).isEmpty()) {
                        translated[chunk.at(k)] = results.at(k);
                    }
                }
                bool chunkDone = true;
                for (int index : chunk) {
                    if (translated.at(index).isEmpty()) {
                        chunkDone = false;
                        break;
                    }
                }
                if (chunkDone) {
                    continue;
                }
            }

            // 批量整批失败（格式漂移/段落数不符），或有个别条目可疑：
            // 只对还没拿到译文的段落逐段重试——批量是"加速手段"，不该把已经拿到的好结果作废，
            // 也不该成为新的失败点。
            //
            // 例外：预算已经吃光时不再逐段重试——8 段 × 最多 5 次会把界面拖死，
            // 用户的等待时间远超能忍的范围。直接带原因跳过。
            if (retryBudgetExhausted()) {
                if (firstParagraphError.isEmpty()) {
                    firstParagraphError =
                        batchOk
                            ? i18n::sf("目标语言 %1 批量译文部分可疑，且已超出重试时间预算",
                                       "Target language %1: some batch results looked wrong and "
                                       "the retry budget is spent",
                                       {dst})
                            : i18n::sf("目标语言 %1 批量翻译失败，且已超出重试时间预算：%2",
                                       "Target language %1: the batch request failed and the retry "
                                       "budget is spent: %2",
                                       {dst, batchError});
                }
                continue;
            }

            for (int k = 0; k < chunk.size(); ++k) {
                // 取消要能打断"逐段补救"：补救请求同样计费，点了取消就别再发。
                if (m_cancelRequested) {
                    cancelled = true;
                    break;
                }
                if (!translated.at(chunk.at(k)).isEmpty()) {
                    continue;   // 批量已经给了可用译文，不重复请求
                }
                QString one;
                QString err;
                // 补救只给 1 次机会（maxAttempts=1）：
                // 批量请求已经把这一批的"正常节奏"用掉了，逐段再各跑 5 次的话
                // 8 段最坏就是 40 次请求，全部计费，而首轮就失败的段落换个 prompt
                // 通常也翻不出来。1 次足够区分"偶发格式抖动"和"真的不行"。
                if (translateOne(texts.at(k), dst, one, err, 1)) {
                    translated[chunk.at(k)] = one;
                } else if (firstParagraphError.isEmpty()) {
                    firstParagraphError = i18n::sf("目标语言 %1 第 %2 段失败：%3",
                                                   "Target language %1, paragraph %2 failed: %3",
                                                   {dst, chunk.at(k) + 1, err});
                }
            }
        }

        if (cancelled) {
            out.ok = false;
            out.error = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
            return out;
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

void LlmTranslator::resetCancel()
{
    // 只允许 Router 在请求开始时调用（见 ICancelControl.h）。
    m_cancelRequested = false;
}

// ---------------------------------------------------------------- 内部实现

QString LlmTranslator::cacheTag() const
{
    // 四段：引擎 | 模型 | 语义参数 | 网关。
    //
    // 为什么拆成四段而不是一段：模型和采样参数是两个**独立**的失效来源，
    // 合成一段以后"为什么命中了上一个模型的译文"就查不出来了。
    //
    // 段 2（模型）：
    //   本地（回环）= 模型指纹。本地模型名常常就是 gguf 文件名或干脆 "local"，
    //   先前 0.5B 与 1.5B 共用同一个 key，换模型后照样"命中 1/1"、拿到上一个模型的译文。
    //   远程 = 原样的模型名——换成指纹会让云端缓存整盘变冷，得不偿失。
    // 段 3（语义）：systemPrompt / temperature / reasoningEffort / maxTokens / stripModelNotes
    //   的白名单摘要。改提示词或温度后旧译文不再命中；batchSize / 端口这类传输参数不进。
    // 段 4（网关）：走 cacheTagBaseUrl()，回环地址去端口（本地 llama-server 是动态端口）。
    const QString model = m_cfg.model.trimmed();
    const QString modelPart = isLoopbackBaseUrl(m_cfg.baseUrl) ? shortHash(model) : model;

    return QStringLiteral("%1|%2|%3|%4")
        .arg(id(), modelPart, semanticHashPart(m_cfg, effectiveSystemPrompt()),
             cacheTagBaseUrl(m_cfg.baseUrl));
}

bool LlmTranslator::retryBudgetExhausted() const
{
    // retryBudgetMs <= 0 = 不限；时钟没启动过（理论上不会）也当作没超。
    if (m_cfg.retryBudgetMs <= 0 || !m_retryClock.isValid()) {
        return false;
    }
    return m_retryClock.elapsed() >= m_cfg.retryBudgetMs;
}

QString LlmTranslator::effectiveSystemPrompt() const
{
    const QString custom = m_cfg.systemPrompt.trimmed();
    return custom.isEmpty() ? defaultSystemPrompt() : custom;
}

QString LlmTranslator::buildUserPrompt(const QString &text, const QString &dstLang) const
{
    const QString dstName = core::LanguageRegistry::displayName(dstLang);

    QString src = core::LanguageRegistry::normalize(m_activeSrcLang);
    if (src.isEmpty() || src == QLatin1String("auto")) {
        // 用语言识别器替代早先的"数汉字/字母"粗糙启发式。
        // 置信度不足时宁可不指定源语言（保守），也不要给出错误暗示。
        const i18n::DetectedLanguage detected = i18n::detectLanguage(text);
        if (!detected.code.isEmpty() && detected.confidence >= 0.5) {
            src = detected.code;
        } else {
            src.clear();
        }

        // 源语言与目标语言相同就没有"翻译"可言，按自动处理。
        if (!src.isEmpty() && src == dstLang) {
            src.clear();
        }
    }

    // 源语言已知时明确写出来（"把英文翻译成中文"）。
    const QString head =
        (!src.isEmpty() && src != QLatin1String("auto"))
            ? QStringLiteral("将下面 <<< 与 >>> 之间的%1文本翻译成【%2】，"
                             "只输出译文，不要输出任何其它内容。")
                  .arg(core::LanguageRegistry::displayName(src), dstName)
            : QStringLiteral("将下面 <<< 与 >>> 之间的文本翻译成【%1】，"
                             "只输出译文，不要输出任何其它内容。")
                  .arg(dstName);

    // 边界符号 <<< >>> 是给模型的硬分隔：原文里出现引号/问号/指令式句子时，
    // 模型容易顺着原文"回答问题"，有了边界它才知道这段只是待翻译素材。
    QString prompt = head;

    // 中日韩 -> 拉丁语系时加一句硬约束。
    // 实测小模型会把整段原文照抄回来（温度 0 也照旧），点明"不许出现原文文字"后正常。
    const bool srcIsCjk = !src.isEmpty()
                          && (src == QLatin1String("zh") || src == QLatin1String("zh-TW")
                              || src == QLatin1String("ja") || src == QLatin1String("ko"));
    if (srcIsCjk && !targetUsesCjk(dstLang)) {
        prompt += QStringLiteral("译文中不得出现任何中文、日文或韩文字符。");
    }

    return prompt + QStringLiteral("\n\n<<<\n") + text + QStringLiteral("\n>>>");
}

// 判断"译文"是否其实就是原文。
//
// 小模型（尤其 0.5B）遇到看不懂的输入时会偷懒：直接把原文吐回来。
// 这必须当成失败，否则用户看到的是"翻译完和没翻一样"，却没有任何提示。
static bool isUntranslatedOutput(const QString &output, const QString &source)
{
    const auto normalize = [](const QString &text) {
        QString out;
        out.reserve(text.size());
        for (const QChar ch : text) {
            if (!ch.isSpace() && !ch.isPunct()) {
                out.append(ch.toLower());
            }
        }
        return out;
    };

    const QString a = normalize(output);
    const QString b = normalize(source);
    return !a.isEmpty() && a == b;
}

bool LlmTranslator::translateOne(const QString &text,
                                 const QString &dstLang,
                                 QString &outText,
                                 QString &outError,
                                 int maxAttempts)
{
    if (maxAttempts < 1) {
        maxAttempts = 1;
    }

    // 输出上限按输入长度估算：小模型陷入复读时，2,048 token 会烧掉七八秒，
    // 而正常译文不会超过原文两倍长度。这里把损耗封顶。
    const int configured = m_cfg.maxTokens > 0 ? m_cfg.maxTokens : 2048;
    const int maxTokens = qBound(64, qMin(configured, text.size() * 2 + 48), configured);

    // 输入预检：没有任何字母/汉字的内容（纯数字、纯符号、空白）不需要翻译。
    // 直接原样返回，省一次请求；也避免模型对 "2026-11-04" 这类内容自由发挥。
    {
        int letterCount = 0;
        QSet<QChar> uniqueChars;
        for (const QChar ch : text) {
            if (ch.isLetter()) {
                ++letterCount;
            }
            uniqueChars.insert(ch);
        }
        if (letterCount == 0 || uniqueChars.size() <= 2) {
            outText = text;
            return true;
        }
    }

    const QString dstName = core::LanguageRegistry::displayName(dstLang);
    const QString srcNormalized = core::LanguageRegistry::normalize(m_activeSrcLang);
    const bool sameLanguage = !srcNormalized.isEmpty() && srcNormalized == dstLang;

    const QString standardPrompt = buildUserPrompt(text, dstLang);

    // 最多 maxAttempts 次尝试，每次换一种策略（默认 5 次 = 用户直接请求的单段翻译）。
    //
    // 为什么换策略而不是"再问一遍"：小模型一旦卡进某种失败模式
    // （照抄原文 / 译成别的语言 / 复读），同一条 prompt 重问往往得到同样的结果。
    // 实测把「中文指令 + <<<>>> 分隔符」换成「纯英文指令」能跳出很多坑。
    //
    // 批量的"逐段补救"路径传 maxAttempts = 1：那里已经失败过一轮，
    // 再各跑 5 次会让成本按段数线性放大（8 段最坏 40 次请求）。
    QString lastError;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        // 用户按了取消：别再为这一段换下一种 prompt 重试（每次都是一次完整请求）。
        if (m_cancelRequested) {
            outError = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
            return false;
        }
        // 时间预算：m_retryClock 是 translate() 开头起的那个钟，整次调用共享一个 deadline，
        // 不再是"每段各自 30s"。第 1 次尝试永远放行——否则预算一用完连一段都翻不出来，
        // 连"部分结果"都没有；已经发出去的请求也无法被打断（由 timeoutMs 兜底）。
        if (attempt > 1 && retryBudgetExhausted()) {
            lastError = i18n::s("重试超出时间预算", "the retry budget was exceeded");
            break;
        }
        QString systemPrompt = effectiveSystemPrompt();
        QString userPrompt = standardPrompt;
        double temperature = m_cfg.temperature;

        switch (attempt) {
        case 2:
            userPrompt += i18n::s("\n\n注意：必须翻译成目标语言，禁止返回原文。",
                                  "\n\nNote: translate into the target language. Never return the source text.");
            temperature = 0.0;
            break;
        case 3:
            systemPrompt = QStringLiteral(
                "You are a professional translation engine. Output only the translation, nothing else.");
            userPrompt = QStringLiteral("Translate the following text into %1. "
                                        "Output only the translation, with no characters from the source language."
                                        "\n\n%2")
                             .arg(dstName, text);
            temperature = 0.0;
            break;
        case 4:
            systemPrompt = QStringLiteral("Translate to %1. Output only the translated text.").arg(dstName);
            userPrompt = QStringLiteral("%1\n---\n%2").arg(dstName, text);
            temperature = 0.3;
            break;
        case 5:
            systemPrompt = QStringLiteral("You are a translator.");
            userPrompt = QStringLiteral("请把下面内容翻译成%1，只输出译文：\n%2").arg(dstName, text);
            temperature = 0.6;
            break;
        default:
            break;
        }

        QString content;
        QString err;
        if (!postChat(systemPrompt, userPrompt, temperature, maxTokens, content, err)) {
            lastError = err;
            // 连不上 / 没响应：换 prompt、换温度都救不回来，重试只会多等 30 秒。
            if (m_lastTransportFailure) {
                break;
            }
            continue;
        }

        // 本地小模型的两种噪声（语言标签行 / 译文后的括注）先清掉再判定。
        if (m_cfg.stripModelNotes) {
            content = stripAnnotationLines(content);
        }

        if (content.isEmpty()) {
            lastError = i18n::s("LLM 未返回译文", "The LLM returned no translation.");
            continue;
        }

        // 复读：先截断；截完仍离谱就换策略。
        if (looksDegenerate(content, text)) {
            content = truncateRepetition(content, text);
            if (looksDegenerate(content, text)) {
                lastError = i18n::s("模型输出重复内容", "The model produced repeated output.");
                continue;
            }
        }

        // 源语言与目标语言相同时，原文就是正确结果，不做语种校验。
        if (!sameLanguage) {
            // 照抄原文。
            if (isUntranslatedOutput(content, text)) {
                lastError = i18n::s("模型没有翻译（返回了原文）",
                                    "The model returned the source text unchanged.");
                continue;
            }

            // 语种不符：译成了别的语言（实测 1.5B 会把中文目标译成意大利语）。
            if (!matchesTargetScript(content, dstLang)) {
                lastError = i18n::s("译文语种与目标语言不符",
                                    "The output is not in the target language.");
                continue;
            }
        }

        outText = content;
        return true;
    }

    // 文案里的次数跟着实际尝试次数走：补救路径只试了 1 次时，
    // 说"已重试 5 次"会让用户以为我们花了很多钱还失败。
    if (m_lastTransportFailure && !lastError.isEmpty()) {
        // 连不上还劝人"换更大的模型"属于误导，原样报连接失败即可。
        outError = lastError;
        return false;
    }
    outError = i18n::sf("翻译失败（已尝试 %1 次）：%2。可以换成更大的模型，或手动指定源语言。",
                        "Translation failed after %1 attempt(s): %2. "
                        "Try a larger model, or set the source language explicitly.",
                        {maxAttempts,
                         lastError.isEmpty() ? i18n::s("未给出原因", "no reason given") : lastError});
    return false;
}

// messages -> 请求。URL 拼接、鉴权头、超时、payload 字段全部在
// translators/LlmRequest.h 的 makeChatRequest() 里（与 llm::probeReasoning 共用
// 同一份实现），这里只负责把"一次翻译"翻译成两条 message。
net::HttpRequest LlmTranslator::buildChatRequest(const QString &systemPrompt,
                                                 const QString &userPrompt,
                                                 double temperature,
                                                 int maxTokens) const
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

    return makeChatRequest(m_cfg, messages, maxTokens, temperature);
}

bool LlmTranslator::postChat(const QString &systemPrompt,
                             const QString &userPrompt,
                             double temperature,
                             int maxTokens,
                             QString &outContent,
                             QString &outError)
{
    const net::HttpRequest request =
        buildChatRequest(systemPrompt, userPrompt, temperature, maxTokens);
    const bool trace = qEnvironmentVariableIsSet("STUDIOUS_LLM_TRACE");
    if (trace) {
        qInfo("[llm] post start url=%s timeout=%dms body=%lld",
              qUtf8Printable(request.url.toString()),
              request.timeoutMs,
              static_cast<long long>(request.body.size()));
    }
    const net::HttpResponse resp = m_http->send(request);
    if (trace) {
        // 实时翻译这种「每 600 ms 一帧」的场景，出问题时要能分清是没发出去、
        // 还是发出去了对面不回。这一行只在排查时开，正式跑不打印。
        QString head = QString::fromUtf8(resp.body.left(160));
        head.replace(QLatin1Char('\n'), QLatin1Char(' '));
        qInfo("[llm] post done status=%d err=%s chars=%lld body=%s",
              resp.status,
              qUtf8Printable(resp.error),
              static_cast<long long>(resp.body.size()),
              qUtf8Printable(head));
    }
    if (!resp.ok) {
        m_lastTransportFailure = resp.firstByteTimedOut || resp.connectionFailed;
        if (resp.firstByteTimedOut) {
            // 甩毫秒数用户看不懂，也分不清是"网络断了"还是"模型在思考"。
            outError = i18n::sf(
                "云端 LLM 连接失败：%1 秒内没有收到任何响应（地址 %2）",
                "Cloud LLM connection failed: no response within %1 s (%2)",
                {kChatFirstByteTimeoutMs / 1000, m_cfg.baseUrl});
        } else if (resp.connectionFailed) {
            outError = i18n::sf("云端 LLM 连接失败：连不上 %1。检查服务地址、端口和网络。",
                                "Cloud LLM connection failed: cannot reach %1. "
                                "Check the endpoint, port and network.",
                                {m_cfg.baseUrl});
        } else {
            outError = describeHttpFailure(resp);
        }
        return false;
    }
    m_lastTransportFailure = false;

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
                                   QStringList &outTexts,
                                   QString &outError)
{
    if (texts.isEmpty()) {
        outTexts.clear();
        return true;
    }

    // 入口先查一次取消：批量路径以前全程不看标志，用户点了取消还要等这一批
    // （含一次降温重试）全部跑完才回到界面。
    if (m_cancelRequested) {
        outError = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
        return false;
    }

    const QString dstName = core::LanguageRegistry::displayName(dstLang);
    const QString srcNormalized = core::LanguageRegistry::normalize(m_activeSrcLang);
    const bool sameLanguage = !srcNormalized.isEmpty() && srcNormalized == dstLang;

    // 输入直接用 JSON 数组，跟要求的输出形状完全一致。
    //
    // 原先是 "[1] 段一\n[2] 段二\n" 的行编号格式，而段落本身**允许带 \n**
    // （前端 TranslateJob 保留段内换行）——段内换行把编号截断，模型看到的条数
    // 与 texts.size() 对不上，parseJsonStringArray 的"条数必须完全相等"直接判失败，
    // 重试一次还是失败，最后退回逐段：批量加速白写。
    // JSON 把换行转义成 \n，内容再脏也撞不上结构。
    QJsonArray inputArray;
    for (const QString &text : texts) {
        inputArray.append(text);
    }
    const QByteArray inputJson = QJsonDocument(inputArray).toJson(QJsonDocument::Compact);

    // system prompt 与单段路径同源（effectiveSystemPrompt：用户配置 > 内置默认），
    // 这里只在末尾**追加**"输出必须是等长 JSON 数组"这条格式约束。
    // 格式约束是机器接口不是文风，所以不能指望用户去写；而用户的文风要求一个字都不动。
    const QString system =
        effectiveSystemPrompt()
        + i18n::s("\n\n批量模式：输入是一个 JSON 字符串数组，你只输出一个等长的 JSON 字符串数组，"
                  "元素顺序与输入一一对应，数量必须完全相同。"
                  "不要输出任何解释、前言、注释或代码块标记。",
                  "\n\nBatch mode: the input is a JSON array of strings. Output only a JSON array of "
                  "the same length, in the same order (one translated element per input element). "
                  "No explanations, preamble, comments or code fences.");

    const QString user =
        i18n::sf("把下面这个 JSON 数组里的每一条翻译成【%1】，返回等长的 JSON 字符串数组。\n"
                 "只输出 JSON，不要任何其它内容。\n\n%2",
                 "Translate every string in this JSON array into %1 and return a JSON array of the "
                 "same length.\nOutput JSON only, nothing else.\n\n%2",
                 {dstName, QString::fromUtf8(inputJson)});

    // 额度和单段路径同源：配置值是**下限**（用户给多少至少给多少），
    // 再按"每段 2 倍字符数 + 48"（与 translateOne 同一公式）估算防截断，估算部分封顶 8192。
    const int configuredTokens = m_cfg.maxTokens > 0 ? m_cfg.maxTokens : 2048;
    int estimatedTokens = 0;
    for (const QString &text : texts) {
        estimatedTokens += text.size() * 2 + 48;
    }
    const int maxTokens = qMax(configuredTokens, qMin(estimatedTokens, 8192));

    // 逐条校验：同一个 translateBatch 里第一次尝试和降温重试都要过这一关，
    // 所以包成 lambda（不引入新成员，也不让两处校验慢慢长歪）。
    // 返回可疑条目的下标，并把原因写进 reasons；可疑条目的译文会被清空。
    //
    // 单段路径有的一整套保护在这里都要有：stripModelNotes 后处理、复读截断、
    // 照抄原文、语种不符——缺一条，批量就会成为"用户开着批量时唯一的质量黑洞"。
    const auto review = [&](QStringList &results, QStringList &reasons) {
        QVector<int> bad;
        for (int i = 0; i < results.size() && i < texts.size(); ++i) {
            if (m_cfg.stripModelNotes) {
                results[i] = stripAnnotationLines(results.at(i)).trimmed();
            }

            // 纯数字/符号段（"3.14"、"-->"、"2026-11-04"）：原样返回就是正确答案。
            // 不在这里放行的话，它会被判"返回了原文"，进而拖出一轮逐段补救请求。
            // 直接用原文而不是模型给的那份：translateOne() 的输入预检也是原样返回，
            // 这样 batch=1 与 batch>1 的输出完全一致（否则模型顺手改个标点就会让
            // 用户看到"开了批量日期格式就变了"）。
            if (hasNoLetters(texts.at(i))) {
                results[i] = texts.at(i);
                continue;
            }

            QString reason;
            if (results.at(i).isEmpty()) {
                reason = i18n::sf("第 %1 条译文为空", "item %1 is empty", {i + 1});
            } else if (looksDegenerate(results.at(i), texts.at(i))) {
                results[i] = truncateRepetition(results.at(i), texts.at(i));
                if (looksDegenerate(results.at(i), texts.at(i))) {
                    reason = i18n::sf("第 %1 条译文重复", "item %1 is repetitive", {i + 1});
                }
            } else if (!sameLanguage && isUntranslatedOutput(results.at(i), texts.at(i))) {
                // 模型把原文原样退回来了。常见于本来就不该翻的东西：菜单名（文件/编辑）、
                // 型号、人名、已经是目标语言的短句。以前这里判「可疑 → 清空 → 逐段补救 →
                // 还是原样 → 整单失败」，实时翻译每 600 ms 撞一次，界面就永远停在「翻译失败」。
                // 现在收下原文（界面按「未翻译」显示），不再算失败。
                results[i] = texts.at(i);
                continue;
            } else if (!sameLanguage && !matchesTargetScript(results.at(i), dstLang)) {
                reason = i18n::sf("第 %1 条语种不符", "item %1 is not in the target language",
                                  {i + 1});
            }

            if (qEnvironmentVariableIsSet("STUDIOUS_LLM_TRACE")) {
                qInfo("[llm] item %d rejected (%s) src=[%s] out=[%s]",
                      i + 1, qUtf8Printable(reason),
                      qUtf8Printable(texts.at(i).left(60)),
                      qUtf8Printable(results.at(i).left(60)));
            }
            if (!reason.isEmpty()) {
                bad.append(i);
                reasons.append(reason);
                results[i].clear();
            }
        }
        return bad;
    };

    QString content;
    QString err;
    if (!postChat(system, user, m_cfg.temperature, maxTokens, content, err)) {
        outError = err;
        return false;
    }

    // 请求在飞的时候用户可能按了取消：这一批的后续（解析 + 降温重试）都不必做了。
    if (m_cancelRequested) {
        outError = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
        return false;
    }

    QStringList results;
    QStringList reasons;
    QVector<int> bad;
    if (parseJsonStringArray(content, texts.size(), results)) {
        bad = review(results, reasons);
        if (qEnvironmentVariableIsSet("STUDIOUS_LLM_TRACE") && !bad.isEmpty()) {
            qInfo("[llm] batch review: %d/%lld item(s) suspicious: %s",
                  static_cast<int>(bad.size()),
                  static_cast<long long>(results.size()),
                  qUtf8Printable(reasons.join(QStringLiteral("; "))));
        }
        if (bad.isEmpty()) {
            outTexts = results;
            return true;
        }
        // 部分可疑：好条目照用，可疑条目留空交给调用方逐段重试。
        // 一条怪段落不该让同批 8 段的正常结果一起作废，更不该拖出 8 段 × 5 次重试。
        if (bad.size() < results.size()) {
            outTexts = results;
            return true;
        }
    } else {
        reasons.clear();
        reasons << i18n::sf("格式不符（期望 %1 条）", "malformed reply (expected %1 items)",
                            {texts.size()});
    }

    // 整批不可用（格式不符 / 每一条都可疑）：压到温度 0 再试一次
    // （与单段路径 attempt 2 的降温一致）。
    // 但预算已经用完就不再试——慢模型上这一批能把整个调用拖过 deadline，
    // 而"重试"在预算耗尽后的期望收益是负的。
    if (retryBudgetExhausted()) {
        outError = i18n::sf("批量译文不可用（%1），且已超出重试时间预算：%2",
                            "The batch reply was unusable (%1) and the retry budget is spent: %2",
                            {reasons.value(0), content.left(200)});
        return false;
    }

    const QString retryUser =
        user + i18n::s("\n\n再次强调：只输出长度完全相同的 JSON 字符串数组，不要任何其它文字。",
                       "\n\nReminder: output only a JSON array of exactly the same length, "
                       "with no other text.");

    // 降温重试前再查一次取消：这是第二次完整请求，取消之后发出去就是白花的钱。
    if (m_cancelRequested) {
        outError = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
        return false;
    }

    QString retryContent;
    QString retryErr;
    if (!postChat(system, retryUser, 0.0, maxTokens, retryContent, retryErr)) {
        outError = i18n::sf("批量翻译失败：%1", "The batch translation failed: %1", {retryErr});
        return false;
    }

    // 重试请求期间又被取消：同样的道理，直接收手。
    if (m_cancelRequested) {
        outError = i18n::s("llm 翻译已取消", "The llm translation was cancelled.");
        return false;
    }

    results.clear();
    reasons.clear();
    if (parseJsonStringArray(retryContent, texts.size(), results)) {
        bad = review(results, reasons);
        if (bad.size() < results.size()) {   // 含 bad 为空（全部可用）的情况
            outTexts = results;
            return true;
        }
    } else {
        reasons.clear();
        reasons << i18n::sf("格式不符（期望 %1 条）", "malformed reply (expected %1 items)",
                            {texts.size()});
    }

    outError = i18n::sf("批量译文不可用（%1）：%2",
                        "The batch reply was unusable (%1): %2",
                        {reasons.value(0), retryContent.left(200)});
    return false;
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

QString LlmTranslator::stripAnnotationLines(QString text)
{
    // 0.5B 小模型实测出来的两种附加物：
    //   「【中文】\n译文」                          —— 语言标签单独占一行
    //   「译文\n\n（中文（简体））」                 —— 译文后追加语言括注
    //   「译文\n\n（翻译结果：Good morning,... >>>）」
    // 规则刻意保守：只删**整行**就是括注 / 标签 / 空行的行，正文一个字都不动。
    // 宁可漏删，也不要误删——正文里合法的「（笑）」被吃掉比多留一行括注更糟。
    static const QVector<QPair<QString, QString>> wrappers = {
        {QStringLiteral("（"), QStringLiteral("）")},
        {QStringLiteral("("), QStringLiteral(")")},
        {QStringLiteral("【"), QStringLiteral("】")},
        {QStringLiteral("["), QStringLiteral("]")},
    };

    // 静态局部变量不需要捕获（它没有自动存储期），捕了反而是一条 -Wunused 之外的告警。
    const auto isWrapped = [](const QString &line) {
        for (const auto &pair : wrappers) {
            if (line.size() > pair.first.size() + pair.second.size()
                && line.startsWith(pair.first) && line.endsWith(pair.second)) {
                return true;
            }
        }
        return false;
    };

    // 括号没配对（模型被截断 / 把 ">>>" 抄进了括号里）时，只有行首是括号
    // 且行内出现"翻译 / 译文 / Translation / >>>"才认，避免误伤正文。
    const auto looksLikeNote = [&isWrapped](const QString &line) {
        if (isWrapped(line)) {
            return true;
        }
        if (line == QLatin1String(">>>")) {
            return true;   // 提示词里的原文边界被单独回吐出来
        }
        const bool startsWithBracket = line.startsWith(QStringLiteral("（"))
                                       || line.startsWith(QLatin1Char('('))
                                       || line.startsWith(QStringLiteral("【"))
                                       || line.startsWith(QLatin1Char('['));
        if (!startsWithBracket) {
            return false;
        }
        return line.contains(QStringLiteral("翻译")) || line.contains(QStringLiteral("译文"))
               || line.contains(QLatin1String(">>>"))
               || line.contains(QLatin1String("translation"), Qt::CaseInsensitive);
    };

    QStringList lines = text.split(QLatin1Char('\n'));
    // 先去掉行尾 \r：否则 "（中文）\r" 永远不满足 endsWith("）")，尾巴就漏了。
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
    }

    const auto shouldDrop = [&looksLikeNote](const QString &line) {
        const QString trimmed = line.trimmed();
        return trimmed.isEmpty() || looksLikeNote(trimmed);
    };

    while (!lines.isEmpty() && shouldDrop(lines.first())) {
        lines.removeFirst();
    }
    while (!lines.isEmpty() && shouldDrop(lines.last())) {
        lines.removeLast();
    }

    // 全被删光（原文本身就只有一行括注，例如原文就是「(笑)」）：原样返回，绝不吐空。
    if (lines.isEmpty()) {
        return text;
    }

    QString out = lines.join(QLatin1Char('\n')).trimmed();
    // 尾巴上残留的边界符号（提示词用 <<< >>> 标原文范围，小模型偶尔抄回来）。
    while (out.endsWith(QLatin1String(">>>"))) {
        out.chop(3);
        out = out.trimmed();
    }
    return out;
}

} // namespace studious::translate
