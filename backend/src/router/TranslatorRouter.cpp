#include "router/TranslatorRouter.h"

#include "cache/TranslationCache.h"
#include "contract/LanguageRegistry.h"
#include "i18n/L10n.h"
#include "i18n/LanguageDetector.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>

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

core::TranslateOutcome TranslatorRouter::translate(const core::TranslateRequest &req)
{
    core::TranslateOutcome out;

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
        m_lastCacheReport = CacheReport{};
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

    core::ITranslator *chosen = nullptr;

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
            out.error = i18n::sf("指定的引擎 %1 不支持目标语言: %2",
                                 "The requested engine %1 does not support these target languages: %2",
                                 {wanted, bad.join(QStringLiteral(", "))});
            return out;
        }

        chosen = t;
    } else {
        QStringList reasons;
        for (const QString &id : m_priority) {
            core::ITranslator *t = findById(id);
            if (t == nullptr) {
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
            chosen = t;
            break;
        }

        if (chosen == nullptr) {
            out.error = i18n::sf("所有翻译引擎均不可用 — %1",
                                 "No translation engine is available — %1",
                                 {reasons.isEmpty()
                                      ? i18n::s("没有已注册的引擎", "no engines are registered")
                                      : reasons.join(i18n::s("；", "; "))});
            return out;
        }
    }

    const QString engineId = chosen->id();

    // 没开缓存：保持原样，一次调用交出去。
    if (!isCacheEnabled()) {
        out = chosen->translate(req.paragraphs, src, dsts, req.options);
        // 同语言的目标语言原样回填（它们根本没走引擎）。
        for (auto it = passthrough.constBegin(); it != passthrough.constEnd(); ++it) {
            out.byLang.insert(it.key(), it.value());
        }
        if (!out.ok) {
            const QString inner =
                out.error.isEmpty() ? i18n::s("未知错误", "unknown error") : out.error;
            out.error = QStringLiteral("[%1] %2").arg(engineId, inner);
        } else {
            out.warning = sourceWarning;
        }
        m_lastCacheReport = CacheReport{};
        return out;
    }

    // 开了缓存：逐段先查，只把未命中的送出去翻译。
    // 关键收益：全命中时一次网络请求都不发（LLM 单次往返约 1.1s，省一次就是省一秒）。
    const QString cacheTag = chosen->cacheTag();
    m_lastCacheReport = CacheReport{};
    m_lastCacheReport.used = true;

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
            if (const std::optional<QString> hit = m_cache->lookup(key)) {
                merged[i] = *hit;
                ++m_lastCacheReport.hits;
            } else {
                missIndex.append(i);
                missTexts << text;
                missKeys << key;
                ++m_lastCacheReport.misses;
            }
        }

        if (!missTexts.isEmpty()) {
            const core::TranslateOutcome sub =
                chosen->translate(missTexts, src, QStringList{dst}, req.options);
            if (!sub.ok) {
                const QString inner =
                    sub.error.isEmpty() ? i18n::s("未知错误", "unknown error") : sub.error;
                errors << QStringLiteral("[%1] %2").arg(engineId, inner);
            }
            const QStringList got = sub.byLang.value(dst);
            for (int k = 0; k < missTexts.size(); ++k) {
                const QString translated = k < got.size() ? got.at(k) : QString();
                merged[missIndex.at(k)] = translated;
                if (!translated.trimmed().isEmpty()) {
                    m_cache->store(missKeys.at(k), missTexts.at(k), translated,
                                   cacheTag, src, dst);
                }
            }
        }

        out.byLang.insert(dst, merged);
    }

    for (auto it = passthrough.constBegin(); it != passthrough.constEnd(); ++it) {
        out.byLang.insert(it.key(), it.value());
    }

    out.ok = errors.isEmpty();
    if (out.ok && !sourceWarning.isEmpty()) {
        out.warning = sourceWarning;
    }
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
