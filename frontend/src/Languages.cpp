#include "Languages.h"

#include "EngineIds.h"

namespace studious::ui::langs {

QStringList split(const QString &joined)
{
    QStringList out;
    const QStringList parts = joined.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString code = raw.trimmed();
        if (!code.isEmpty() && !out.contains(code)) {
            out.append(code);
        }
    }
    return out;
}

QString join(const QStringList &langs)
{
    return langs.join(QLatin1Char(','));
}

QStringList normalizeTargets(const QStringList &targets, const QString &engineUiId)
{
    // 「自动检测」只对源语言有意义：目标语言填 auto 等于没指定。
    QStringList out;
    for (const QString &code : targets) {
        if (!isAuto(code) && !out.contains(code))
            out.append(code);
    }
    if (out.isEmpty()) {
        out.append(QStringLiteral("zh"));
    }
    // 本地 MT 按语言对装模型：多目标会让路由整单判不支持，界面还留着用不上的标签。
    if (engine::toUi(engineUiId) == QLatin1String("local-mt") && out.size() > 1) {
        out = {out.first()};
    }
    return out;
}

QStringList normalizeTargetsAgainstSource(const QStringList &targets, const QString &source)
{
    const QString src = normalizeSource(source);
    QStringList out = targets;
    if (!isAuto(src)) {
        out.removeAll(src);
    }
    if (out.isEmpty()) {
        // 剔完一个不剩：挑一个和源语言不同的常识目标语言，别把界面留在空选择上。
        const QStringList fallbacks = {QStringLiteral("zh"), QStringLiteral("en")};
        for (const QString &fallback : fallbacks) {
            if (isAuto(src) || fallback != src) {
                out.append(fallback);
                break;
            }
        }
        if (out.isEmpty())
            out.append(QStringLiteral("en"));
    }
    return out;
}

bool isAuto(const QString &code)
{
    const QString trimmed = code.trimmed();
    return trimmed.isEmpty()
        || trimmed.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0;
}

QString normalizeSource(const QString &code)
{
    // 不在这里校验语言码是否注册：CLI 传了没见过的码时，应该让后端给出
    // 「不支持这个源语言」的明确错误，而不是被我们悄悄改成自动检测。
    return isAuto(code) ? QStringLiteral("auto") : code.trimmed();
}

}   // namespace studious::ui::langs
