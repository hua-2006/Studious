#include "router/TranslatorRouter.h"

#include "cache/TranslationCache.h"
#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"
#include "i18n/LanguageDetector.h"
#include "translators/ICancelControl.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include <initializer_list>
#include <utility>

namespace studious::router {

namespace {

// 同一条「状态矛盾」只打一次。status() 会被界面反复调用（每次刷新引擎状态一次），
// 每帧都打就会把 stderr 刷满同一行，真出问题时反而看不见。
void warnContradictionOnce(const QString &engineId, const QString &reason)
{
    static QMutex mutex;
    static QSet<QString> seen;
    const QString key = engineId + QLatin1Char('|') + reason;
    QMutexLocker locker(&mutex);
    if (seen.contains(key)) {
        return;
    }
    seen.insert(key);
    qWarning("engine %s: isReady() but resolve() refused: %s",
             qUtf8Printable(engineId),
             qUtf8Printable(reason));
}

// 文本里是否出现任意一个关键字（不区分大小写）。
bool containsAny(const QString &text, std::initializer_list<QLatin1String> needles)
{
    for (const QLatin1String needle : needles) {
        if (text.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

// 只保留"非空白"字符并折成小写，用于判断译文是不是原文照抄。
// 比较时忽略大小写与所有空白（含换行/全角空格），但不忽略标点 ——
// "Hello." 与 "Hello" 算两回事，避免把正常译文误判成照抄。
QString normalizeForEchoCompare(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace()) {
            continue;
        }
        out.append(ch);
    }
    return out.toLower();
}

// 译文 == 原文（大小写/空白归一后比较）？
// 这种"假成功"必须挡在缓存外面：实测 8000 个 'a' 被后端原样吐回，
// 旧代码照样 store，于是第二次起直接命中缓存 7ms 返回那串 'a'，
// 而且缓存里永久留着一条"正确译文"，用户再怎么重试也还是它。
bool isSameTextIgnoringCaseAndSpace(const QString &translated, const QString &source)
{
    const QString norm = normalizeForEchoCompare(translated);
    return !norm.isEmpty() && norm == normalizeForEchoCompare(source);
}

// 段落里是否含字母（汉字/假名也算字母）。
// 纯数字/符号段（"3.14"、"-->"、"2026-11-04"）的"译文"本来就等于原文，
// 这类段落照抄是正确的，不该按缺陷 1 拒收，也不该因为拒收而每次都重翻。
bool hasLetters(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.isLetter()) {
            return true;
        }
    }
    return false;
}

// 命中里出现"原文照抄"行只提示一次：老版本（以及第三方适配器）已经把这种行写进库了，
// 库没有按 key 删除的接口，所以只能"不采信"，让用户自己清缓存回收空间。
void warnCachedEchoOnce()
{
    static QMutex mutex;
    static bool warned = false;
    QMutexLocker locker(&mutex);
    if (warned) {
        return;
    }
    warned = true;
    qWarning("cache: ignoring cached entries whose translation equals the source text; "
             "clear the translation cache to reclaim their space");
}

// 引擎这一次失败属于"可用性类"（换下一个引擎还有救）还是"语义类"（换谁都一样）？
//
// 为什么只能看文本：core::TranslateOutcome 只有 ok / byLang / error / warning /
// passthrough 五个字段（见 contract/Types.h），没有 retryable / httpStatus 这类分类字段；
// 适配器层的 httpStatus 在 WebTranslator / LlmTranslator 里已经被拍平成 error 文案了。
// 而 i18n::s() 是"取词时就按当前 locale 定型"的，所以关键字表同时收录中英两种措辞。
//
// 语义类 = 换个引擎结果还是同样失败：
//   源语言 == 目标语言、目标语言无效 / 未收到、语言对不支持、请求 URL 非法、
//   用户主动取消（不是失败，别换一家再来一遍）、内容被引擎拒绝（4xx）。
// 可用性类 = 可能是暂时的或只是这一家的问题：
//   网络错误 / 连接失败 / 超时、HTTP 429 与 5xx、本地服务或模型没起来、
//   引擎内部临时错误（响应解析失败、空译文、重试预算耗尽……）。
//
// 认不出来的原因按"可用性"处理：宁可多花一次请求，也别让一次偶发错误拖垮整单。
bool isAvailabilityFailure(const core::TranslateOutcome &r)
{
    const QString text = r.error.trimmed();
    if (text.isEmpty()) {
        // 说失败却没给原因：当成可用性问题，换下一个引擎再试。
        return true;
    }

    return !containsAny(text,
                        {
                            // 源语言与目标语言相同
                            QLatin1String("源语言与目标语言相同"),
                            QLatin1String("source and target language"),
                            QLatin1String("target language is the same"),
                            // 目标语言非法 / 缺失
                            QLatin1String("目标语言无效"),
                            QLatin1String("invalid target language"),
                            QLatin1String("未收到目标语言"),
                            QLatin1String("received no target language"),
                            QLatin1String("未指定目标语言"),
                            QLatin1String("no target language specified"),
                            // 语言对不支持（模型词表里没有这种 token）
                            QLatin1String("不支持"),
                            QLatin1String("not support"),
                            QLatin1String("unsupported"),
                            QLatin1String("无法确定源语言"),
                            QLatin1String("could not determine the source language"),
                            QLatin1String("cannot determine the source language"),
                            QLatin1String("语言对不可用"),
                            QLatin1String("language pair is unavailable"),
                            // 请求参数非法
                            QLatin1String("请求 url 无效"),
                            QLatin1String("invalid request url"),
                            // 用户主动取消
                            QLatin1String("已取消"),
                            QLatin1String("cancelled"),
                            QLatin1String("canceled"),
                            // 内容被引擎拒绝
                            QLatin1String("内容审核"),
                            QLatin1String("content policy"),
                            QLatin1String("content rejected"),
                            QLatin1String("请求被拒绝"),
                            QLatin1String("rejected the request"),
                        });
}

}   // namespace

TranslatorRouter::TranslatorRouter(TranslatorList translators, QStringList priority)
    : m_translators(std::move(translators))
    , m_priority(std::move(priority))
{
}

TranslatorRouter::~TranslatorRouter() = default;

core::ITranslator *TranslatorRouter::findById(const QString &id) const
{
    for (const auto &t : m_translators) {
        if (t && t->id() == id) {
            return t.get();
        }
    }
    return nullptr;
}

QStringList TranslatorRouter::engineIds() const
{
    QStringList ids;
    ids.reserve(static_cast<qsizetype>(m_translators.size()));
    for (const auto &t : m_translators) {
        if (t) {
            ids << t->id();
        }
    }
    return ids;
}

void TranslatorRouter::cancel()
{
    // 只置标志，不碰任何正在使用的 Qt 对象 —— 所以可以从 UI 线程调用。
    for (const auto &t : m_translators) {
        if (t) {
            t->cancel();
        }
    }
}

core::TranslateOutcome TranslatorRouter::translate(const core::TranslateRequest &req)
{
    core::TranslateOutcome out;

    // 每次调用都重新统计：缓存没开时它就是全零（与旧行为一致，旧代码每条返回路径都会重置）。
    m_lastCacheReport = CacheReport{};

    // 取消标志整单只清一次，就在这里 —— "一次 Router::translate() == 一次用户请求"。
    //
    // 以前是各引擎在自己的 translate() 入口清。降级链上这会吞取消：
    // 引擎 A 失败 → 用户此刻点取消 → Router 换引擎 B → B 在入口把标志清掉继续翻，
    // 用户明明点了取消却还要等一次完整往返。清零权收归 Router 后，
    // 引擎只读不写标志，降级边界上到达的取消一定能被下一个引擎看到。
    for (const auto &t : m_translators) {
        if (!t) {
            continue;
        }
        if (auto *ctl = dynamic_cast<translate::ICancelControl *>(t.get())) {
            ctl->resetCancel();
        }
    }

    const QString src = core::LanguageRegistry::normalize(
        req.srcLang.trimmed().isEmpty() ? QStringLiteral("auto") : req.srcLang);

    // 目标语言归一化 + 去重；为空时默认翻成简体中文（v1 UI 的实际默认值）。
    QStringList dsts;
    for (const QString &raw : req.dstLangs) {
        const QString n = core::LanguageRegistry::normalize(raw);
        if (!n.isEmpty() && !dsts.contains(n)) {
            dsts << n;
        }
    }
    if (dsts.isEmpty()) {
        dsts = {QStringLiteral("zh")};
    }

    // 源语言可疑：用户明确选了某种语言，但识别结果与之不符。
    //
    // 这里**不改变行为**（仍按用户选择去翻）——LLM/Web 实测都能自纠，
    // MT（v2）则会按检测值强制走。这里只负责"告诉用户一声"，
    // 免得他觉得"我选了日语怎么翻出来怪怪的"。
    QString sourceWarning;
    if (src != QLatin1String("auto") && !req.paragraphs.isEmpty()) {
        const QString sample = req.paragraphs.first().left(300);
        const i18n::DetectedLanguage detected = i18n::detectLanguage(sample);
        if (!detected.code.isEmpty() && detected.confidence >= 0.7 && detected.code != src) {
            sourceWarning =
                i18n::sf("检测到原文可能是%1，与所选源语言%2不一致；已按所选继续翻译。"
                         "若结果异常，请把源语言改为「%1」或「自动检测」。",
                         "The text looks like %1 but the selected source language is %2. "
                         "Translation continued anyway; if the result looks wrong, switch the source "
                         "language to %1 or auto-detect.",
                         {core::LanguageRegistry::displayName(detected.code),
                          core::LanguageRegistry::displayName(src)});
        }
    }

    // 业务规则：源语言 == 目标语言时不做任何翻译，原样返回。
    //
    // 用户手动选了 zh -> zh，说明他只想"回填原文"（或选错了）。此时调用引擎
    // 只会拿到复读、改写或乱码，没有任何价值，还白花一次请求。
    QHash<QString, QStringList> passthrough;
    if (src != QLatin1String("auto")) {
        for (int i = dsts.size() - 1; i >= 0; --i) {
            if (dsts.at(i) == src) {
                passthrough.insert(dsts.at(i), req.paragraphs);
                dsts.removeAt(i);
            }
        }
    }

    // 所有目标语言都与源语言相同：直接返回原文，不选引擎、不请求网络。
    if (dsts.isEmpty()) {
        out.ok = true;
        out.byLang = passthrough;
        out.passthrough = true;
        out.warning = i18n::s("源语言与目标语言相同，已原样返回（未翻译）。",
                              "Source and target languages are the same; the text was returned "
                              "unchanged without translation.");
        return out;
    }

    // 组件对"支持哪些目标语言"的判定要逐个问，多目标时要求全支持。
    const auto unsupportedDsts = [&src, &dsts](core::ITranslator *t) {
        QStringList bad;
        for (const QString &d : dsts) {
            if (!t->supports(src, d)) {
                bad << d;
            }
        }
        return bad;
    };

    const auto assetsSuffix = [](const core::ResolveResult &rr) {
        if (rr.missingAssets.isEmpty()) {
            return QString();
        }
        return i18n::sf("；缺失资产: %1",
                        "; missing assets: %1",
                        {QStringList(rr.missingAssets.begin(), rr.missingAssets.end())
                             .join(QStringLiteral(", "))});
    };

    // 候选引擎（按 priority 顺序）。这里不再只挑一个：运行期失败还要能往下一个走。
    std::vector<core::ITranslator *> candidates;
    bool forced = false;   // 用户指定了引擎：没有"下一个"，失败原样上报

    const QString wanted = req.preferredEngine.trimmed();
    if (!wanted.isEmpty()) {
        // 强制组件：存在就只用它，不可用也必须给出明确原因，绝不偷偷换成别的引擎。
        core::ITranslator *t = findById(wanted);
        if (t == nullptr) {
            out.error = i18n::sf("指定的引擎 \"%1\" 不存在；已注册引擎: %2",
                                 "The requested engine \"%1\" does not exist; registered engines: %2",
                                 {wanted, engineIds().join(QStringLiteral(", "))});
            return out;
        }

        const core::ResolveResult rr = t->resolve(src, dsts.first());
        if (!t->isReady()) {
            out.error = i18n::sf("指定的引擎 %1 当前不可用：%2%3",
                                 "The requested engine %1 is currently unavailable: %2%3",
                                 {wanted,
                                  rr.reason.isEmpty()
                                      ? i18n::s("组件未就绪", "the engine is not ready")
                                      : rr.reason,
                                  assetsSuffix(rr)});
            return out;
        }

        const QStringList bad = unsupportedDsts(t);
        if (!bad.isEmpty()) {
            // "不支持目标语言"只说对了一半：本地 MT 这里多半是**源语言**读不了
            // （mul-en 词表里没有 ko），说成目标语言会让人以为 en 不可用。
            const core::ResolveResult rr = t->resolve(src, bad.first());
            out.error = rr.reason.isEmpty()
                            ? i18n::sf("指定的引擎 %1 不支持目标语言: %2",
                                       "The requested engine %1 does not support these target languages: %2",
                                       {wanted, bad.join(QStringLiteral(", "))})
                            : i18n::sf("指定的引擎 %1 翻不了这个方向：%2%3",
                                       "The requested engine %1 cannot handle this direction: %2%3",
                                       {wanted, rr.reason, assetsSuffix(rr)});
            return out;
        }

        candidates.push_back(t);
        forced = true;
    } else {
        QStringList reasons;
        for (const QString &id : m_priority) {
            core::ITranslator *t = findById(id);
            if (t == nullptr) {
                // priority 里写了但没注册：这条只是"跳过并继续下一个"，绝不让整单失败
                // （典型场景：前端沿用了旧的 {"mock", ...} 列表，而 mock 已退出生产装配）。
                reasons << i18n::sf("%1: 该引擎未注册", "%1: this engine is not registered", {id});
                continue;
            }
            if (!t->isReady()) {
                const core::ResolveResult rr = t->resolve(src, dsts.first());
                reasons << i18n::sf("%1: %2%3",
                                    "%1: %2%3",
                                    {id,
                                     rr.reason.isEmpty()
                                         ? i18n::s("组件未就绪", "the engine is not ready")
                                         : rr.reason,
                                     assetsSuffix(rr)});
                continue;
            }
            const QStringList bad = unsupportedDsts(t);
            if (!bad.isEmpty()) {
                reasons << i18n::sf("%1: 不支持目标语言 %2",
                                    "%1: does not support target languages %2",
                                    {id, bad.join(QStringLiteral(", "))});
                continue;
            }
            candidates.push_back(t);
        }

        if (candidates.empty()) {
            out.error = i18n::sf("所有翻译引擎均不可用 — %1",
                                 "No translation engine is available — %1",
                                 {reasons.isEmpty()
                                      ? i18n::s("没有已注册的引擎", "no engines are registered")
                                      : reasons.join(i18n::s("；", "; "))});
            return out;
        }
    }

    // 逐个尝试候选引擎：
    //   成功        -> 立刻返回（引擎自带的 warning 会被源语言提示覆盖，与旧行为一致）
    //   可用性失败  -> 记一行原因，继续下一个
    //   语义失败    -> 立刻返回（换引擎也没意义）
    QStringList failures;           // 只收集"可用性类"失败，格式 "id：原因"
    core::TranslateOutcome last;    // 最后一次失败结果，用来保留已拿到的部分译文

    for (core::ITranslator *engine : candidates) {
        core::TranslateOutcome attempt = runEngine(engine, req, src, dsts);

        // 同语言的目标语言原样回填（它们根本没走引擎）。
        for (auto it = passthrough.constBegin(); it != passthrough.constEnd(); ++it) {
            attempt.byLang.insert(it.key(), it.value());
        }

        if (attempt.ok) {
            // 引擎自带的非阻断提示（例如"3 段按原文显示"）必须保住：这里以前是
            // 直接覆盖成源语言提示，等于把后端辛苦报上来的降级信息整条吞掉，
            // 用户只会看到一屏夹杂原文的译文而没有任何说明。
            QStringList notes;
            if (!attempt.warning.trimmed().isEmpty()) {
                notes << attempt.warning;
            }
            if (!sourceWarning.trimmed().isEmpty()) {
                notes << sourceWarning;
            }
            attempt.warning = notes.join(i18n::s("；", "; "));
            return attempt;
        }

        const QString inner = attempt.error.trimmed().isEmpty()
                                  ? i18n::s("未知错误", "unknown error")
                                  : attempt.error;

        // 语义类失败不降级；强制引擎也没有"下一个"——两者都原样上报。
        if (forced || !isAvailabilityFailure(attempt)) {
            attempt.error = QStringLiteral("[%1] %2").arg(engine->id(), inner);
            return attempt;
        }

        qWarning("router: engine %s failed (%s), trying next",
                 qUtf8Printable(engine->id()),
                 qUtf8Printable(inner));
        failures << i18n::sf("%1：%2", "%1: %2", {engine->id(), inner});
        last = attempt;
    }

    // 全部失败：把每个引擎的原因聚合成一行，前端直接展示即可。
    core::TranslateOutcome failed = last;
    failed.ok = false;
    failed.warning.clear();
    failed.error = i18n::sf("所有可用引擎都失败了：%1",
                            "All available engines failed: %1",
                            {failures.join(i18n::s("；", "; "))});
    return failed;
}

core::TranslateOutcome TranslatorRouter::runEngine(core::ITranslator *engine,
                                                   const core::TranslateRequest &req,
                                                   const QString &src,
                                                   const QStringList &dsts)
{
    // 没开缓存：保持原样，一次调用交出去。
    if (!isCacheEnabled()) {
        return engine->translate(req.paragraphs, src, dsts, req.options);
    }

    // 开了缓存：逐段先查，只把未命中的送出去翻译。
    // 关键收益：全命中时一次网络请求都不发（LLM 单次往返约 1.1s，省一次就是省一秒）。
    //
    // 统计在多次尝试之间累加（降级前后一共查了多少段、命中多少段），
    // 这样界面上"命中 3/5"在降级发生后依然说得通。
    const QString cacheTag = engine->cacheTag();
    m_lastCacheReport.used = true;

    core::TranslateOutcome out;
    QStringList errors;

    for (const QString &dst : dsts) {
        QStringList merged;
        merged.reserve(req.paragraphs.size());

        QVector<int> missIndex;
        QStringList missTexts;
        QStringList missKeys;

        for (int i = 0; i < req.paragraphs.size(); ++i) {
            merged << QString();

            const QString text = req.paragraphs.at(i).trimmed();
            if (text.isEmpty()) {
                continue;   // 空段落既不入缓存也不发请求
            }

            const QString key = cache::TranslationCache::makeKey(cacheTag, src, dst, text);
            const std::optional<QString> hit = m_cache->lookup(key);
            // 库里存着"原文照抄"（旧版本写进去的污染行，实测 8000 个 'a'）时不能当命中：
            // 一命中就等于把错译永久化。这种行按未命中处理，重新翻一次（本次也不会再写回去）。
            // 纯数字/符号段不在此列 —— 它们本来就该原样返回。
            const bool echoedHit = hit.has_value() && hasLetters(text)
                                   && isSameTextIgnoringCaseAndSpace(*hit, text);
            if (hit.has_value() && !echoedHit) {
                merged[i] = *hit;
                ++m_lastCacheReport.hits;
            } else {
                if (echoedHit) {
                    warnCachedEchoOnce();
                }
                missIndex.append(i);
                missTexts << text;
                missKeys << key;
                ++m_lastCacheReport.misses;
            }
        }

        if (!missTexts.isEmpty()) {
            const core::TranslateOutcome sub =
                engine->translate(missTexts, src, QStringList{dst}, req.options);
            // 引擎的"非阻断提示"（有段落按原文显示等）要原样带出去，
            // 否则 runEngine 聚合完只剩 ok / byLang，前端拿不到任何降级信号。
            if (!sub.warning.trimmed().isEmpty()) {
                if (!out.warning.trimmed().isEmpty()) {
                    out.warning += i18n::s("；", "; ");
                }
                out.warning += sub.warning;
            }
            if (!sub.ok) {
                // 引擎前缀由调用方统一加（降级聚合时格式才统一）。
                errors << (sub.error.trimmed().isEmpty()
                               ? i18n::s("未知错误", "unknown error")
                               : sub.error);
            }
            // 写缓存用的 tag 必须是"这次实际是谁产出的" —— WebTranslator 的 tag 记录
            // 本次成功的适配器，调用前根本不知道，只能在调用后再问一次。
            // 与上面查缓存用的 tag 不同不影响正确性：下一次请求查缓存时会拿到新的 tag。
            const QString storeTag = engine->cacheTag();
            const QStringList got = sub.byLang.value(dst);
            for (int k = 0; k < missTexts.size(); ++k) {
                const QString translated = k < got.size() ? got.at(k) : QString();
                merged[missIndex.at(k)] = translated;

                // 三道闸，缺一条缓存就会被污染（都实测踩过）：
                //   1. 整段失败（sub.ok == false）时 byLang 里的东西不可信；
                //   2. 空译文等于没翻，存进去下次就再也不发了；
                //   3. 译文 == 原文是"假成功"，存进去等于永久固化一条错译。
                // 例外：纯数字/符号段（"3.14"）照抄就是正确结果，放行入库，
                // 否则每次请求都要为同一串数字再发一次网络/模型请求。
                if (!sub.ok || translated.trimmed().isEmpty()) {
                    continue;
                }
                if (hasLetters(missTexts.at(k))
                    && isSameTextIgnoringCaseAndSpace(translated, missTexts.at(k))) {
                    continue;
                }
                m_cache->store(missKeys.at(k), missTexts.at(k), translated,
                               storeTag, src, dst);
            }
        }

        out.byLang.insert(dst, merged);
    }

    out.ok = errors.isEmpty();
    if (!out.ok) {
        out.error = errors.join(i18n::s("；", "; "));
    }
    return out;
}

QVector<QPair<QString, core::ResolveResult>> TranslatorRouter::status(const QString &src,
                                                                    const QString &dst) const
{
    const QString s = core::LanguageRegistry::normalize(src.trimmed().isEmpty()
                                                            ? QStringLiteral("auto")
                                                            : src);
    const QString d = core::LanguageRegistry::normalize(dst.trimmed().isEmpty()
                                                            ? QStringLiteral("zh")
                                                            : dst);

    QVector<QPair<QString, core::ResolveResult>> rows;
    rows.reserve(static_cast<qsizetype>(m_translators.size()));
    for (const auto &t : m_translators) {
        if (!t) {
            continue;
        }
        core::ResolveResult rr = t->resolve(s, d);
        if (!rr.ready && rr.reason.isEmpty()) {
            rr.reason = i18n::s("组件未就绪", "The engine is not ready");
        }
        if (t->isReady() && !rr.ready) {
            // 组件自认可用但 resolve 说不行。用户看到的应该是 resolve 的理由本身，
            // 「状态矛盾」是给开发者的信息，打进日志就够，别塞进界面。
            warnContradictionOnce(t->id(), rr.reason);
        }
        rows.push_back(qMakePair(t->id(), rr));
    }
    return rows;
}

} // namespace studious::router
