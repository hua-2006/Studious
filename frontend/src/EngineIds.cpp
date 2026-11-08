#include "EngineIds.h"

#include <QCoreApplication>

namespace studious::ui::engine {

QStringList known()
{
    return {QStringLiteral("web"), QStringLiteral("llm-cloud"),
            QStringLiteral("llm-local"), QStringLiteral("local-mt")};
}

QStringList defaultPriority()
{
    return {QStringLiteral("llm-local"), QStringLiteral("local-mt"),
            QStringLiteral("llm-cloud"), QStringLiteral("web")};
}

QString toUi(const QString &raw)
{
    const QString id = raw.trimmed().toLower();
    if (id.isEmpty() || id == QLatin1String("auto")) {
        return QString();
    }
    if (id == QLatin1String("web")) {
        return QStringLiteral("web");
    }
    // 老设置里云端与本地共用 router id "llm"，靠 useLocalEngine 分流；
    // 走到这里（只剩 id、没有那个布尔）按云端算。
    if (id == QLatin1String("llm-cloud") || id == QLatin1String("llm")) {
        return QStringLiteral("llm-cloud");
    }
    if (id == QLatin1String("llm-local") || id == QLatin1String("llamacpp")
        || id == QLatin1String("local-llm")) {
        return QStringLiteral("llm-local");
    }
    if (id == QLatin1String("local-mt") || id == QLatin1String("localmt")) {
        return QStringLiteral("local-mt");
    }
    return QString();
}

QString toRouter(const QString &uiId)
{
    const QString ui = toUi(uiId);
    if (ui.isEmpty()) {
        return QString();
    }
    if (ui == QLatin1String("llm-cloud")) {
        return QStringLiteral("llm");
    }
    if (ui == QLatin1String("llm-local")) {
        return QString();   // 不进 Router，由 LocalEngine 单独承担
    }
    return ui;
}

QStringList sanitizePriority(const QStringList &input)
{
    const QStringList members = known();
    QStringList out;
    for (const QString &raw : input) {
        const QString ui = toUi(raw);
        if (ui.isEmpty() || out.contains(ui) || !members.contains(ui)) {
            continue;
        }
        out.append(ui);
    }
    for (const QString &id : defaultPriority()) {
        if (!out.contains(id)) {
            out.append(id);
        }
    }
    return out;
}

QStringList toRouterPriority(const QStringList &uiOrder)
{
    QStringList out;
    for (const QString &uiId : uiOrder) {
        const QString id = toRouter(uiId);
        if (id.isEmpty() || out.contains(id)) {
            continue;
        }
        out.append(id);
    }
    return out;
}

QString displayName(const QString &uiId)
{
    const QString ui = toUi(uiId);
    if (ui.isEmpty()) {
        return QCoreApplication::translate("EngineIds", "自动");
    }
    if (ui == QLatin1String("web")) {
        return QCoreApplication::translate("EngineIds", "Web 在线");
    }
    if (ui == QLatin1String("llm-cloud")) {
        return QCoreApplication::translate("EngineIds", "云端 LLM");
    }
    if (ui == QLatin1String("llm-local")) {
        return QCoreApplication::translate("EngineIds", "本地 LLM");
    }
    return QCoreApplication::translate("EngineIds", "本地 MT");
}

}   // namespace studious::ui::engine
