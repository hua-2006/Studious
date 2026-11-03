#include "router/TranslatorRouter.h"

#include "contract/LanguageRegistry.h"

#include <utility>

namespace studious::router {

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
        return QStringLiteral("；缺失资产: %1").arg(QStringList(rr.missingAssets.begin(),
                                                               rr.missingAssets.end())
                                                    .join(QStringLiteral(", ")));
    };

    core::ITranslator *chosen = nullptr;

    const QString wanted = req.preferredEngine.trimmed();
    if (!wanted.isEmpty()) {
        // 强制组件：存在就只用它，不可用也必须给出明确原因，绝不偷偷换成别的引擎。
        core::ITranslator *t = findById(wanted);
        if (t == nullptr) {
            out.error = QStringLiteral("指定的引擎 \"%1\" 不存在；已注册引擎: %2")
                            .arg(wanted, engineIds().join(QStringLiteral(", ")));
            return out;
        }

        const core::ResolveResult rr = t->resolve(src, dsts.first());
        if (!t->isReady()) {
            out.error = QStringLiteral("指定的引擎 %1 当前不可用：%2%3")
                            .arg(wanted,
                                 rr.reason.isEmpty() ? QStringLiteral("组件未就绪") : rr.reason,
                                 assetsSuffix(rr));
            return out;
        }

        const QStringList bad = unsupportedDsts(t);
        if (!bad.isEmpty()) {
            out.error = QStringLiteral("指定的引擎 %1 不支持目标语言: %2")
                            .arg(wanted, bad.join(QStringLiteral(", ")));
            return out;
        }

        chosen = t;
    } else {
        QStringList reasons;
        for (const QString &id : m_priority) {
            core::ITranslator *t = findById(id);
            if (t == nullptr) {
                reasons << QStringLiteral("%1: 该引擎未注册").arg(id);
                continue;
            }
            if (!t->isReady()) {
                const core::ResolveResult rr = t->resolve(src, dsts.first());
                reasons << QStringLiteral("%1: %2%3")
                               .arg(id,
                                    rr.reason.isEmpty() ? QStringLiteral("组件未就绪") : rr.reason,
                                    assetsSuffix(rr));
                continue;
            }
            const QStringList bad = unsupportedDsts(t);
            if (!bad.isEmpty()) {
                reasons << QStringLiteral("%1: 不支持目标语言 %2")
                               .arg(id, bad.join(QStringLiteral(", ")));
                continue;
            }
            chosen = t;
            break;
        }

        if (chosen == nullptr) {
            out.error = QStringLiteral("所有翻译引擎均不可用 — %1")
                            .arg(reasons.isEmpty() ? QStringLiteral("没有已注册的引擎")
                                                   : reasons.join(QStringLiteral("；")));
            return out;
        }
    }

    const QString engineId = chosen->id();
    out = chosen->translate(req.paragraphs, src, dsts, req.options);
    if (!out.ok) {
        const QString inner = out.error.isEmpty() ? QStringLiteral("未知错误") : out.error;
        out.error = QStringLiteral("[%1] %2").arg(engineId, inner);
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
            rr.reason = QStringLiteral("组件未就绪");
        }
        if (t->isReady() && !rr.ready) {
            // 组件自认可用但 resolve 说不行：以 resolve 为准，同时把矛盾暴露出来。
            rr.reason = QStringLiteral("组件状态与 resolve 结果不一致：") + rr.reason;
        }
        rows.push_back(qMakePair(t->id(), rr));
    }
    return rows;
}

} // namespace studious::router
