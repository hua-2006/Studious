#include "contract/LanguageRegistry.h"

#include <QHash>
#include <QSet>

namespace studious::core {

namespace {

// 别名表：key 一律小写、下划线已替换为连字符。
const QHash<QString, QString> &aliasTable()
{
    static const QHash<QString, QString> table = {
        // 自动检测
        {QStringLiteral("auto"),    QStringLiteral("auto")},
        {QStringLiteral("detect"),  QStringLiteral("auto")},

        // 简体中文：zh-cn / zh-hans 等多种写法归一到 zh
        {QStringLiteral("zh"),      QStringLiteral("zh")},
        {QStringLiteral("zh-cn"),   QStringLiteral("zh")},
        {QStringLiteral("zh-hans"), QStringLiteral("zh")},
        {QStringLiteral("zh-sg"),   QStringLiteral("zh")},
        {QStringLiteral("zh-my"),   QStringLiteral("zh")},
        {QStringLiteral("cn"),      QStringLiteral("zh")},
        {QStringLiteral("chs"),     QStringLiteral("zh")},

        // 繁体中文
        {QStringLiteral("zh-tw"),   QStringLiteral("zh-TW")},
        {QStringLiteral("zh-hant"), QStringLiteral("zh-TW")},
        {QStringLiteral("zh-hk"),   QStringLiteral("zh-TW")},
        {QStringLiteral("zh-mo"),   QStringLiteral("zh-TW")},
        {QStringLiteral("cht"),     QStringLiteral("zh-TW")},

        {QStringLiteral("en"),      QStringLiteral("en")},
        {QStringLiteral("en-us"),   QStringLiteral("en")},
        {QStringLiteral("en-gb"),   QStringLiteral("en")},
        {QStringLiteral("eng"),     QStringLiteral("en")},

        // jp 是常见误写，必须吃掉
        {QStringLiteral("ja"),      QStringLiteral("ja")},
        {QStringLiteral("ja-jp"),   QStringLiteral("ja")},
        {QStringLiteral("jp"),      QStringLiteral("ja")},
        {QStringLiteral("jpn"),     QStringLiteral("ja")},

        // kr 是常见误写，必须吃掉
        {QStringLiteral("ko"),      QStringLiteral("ko")},
        {QStringLiteral("ko-kr"),   QStringLiteral("ko")},
        {QStringLiteral("kr"),      QStringLiteral("ko")},
        {QStringLiteral("kor"),     QStringLiteral("ko")},

        {QStringLiteral("fr"),      QStringLiteral("fr")},
        {QStringLiteral("fr-fr"),   QStringLiteral("fr")},
        {QStringLiteral("fra"),     QStringLiteral("fr")},
        {QStringLiteral("fre"),     QStringLiteral("fr")},

        {QStringLiteral("de"),      QStringLiteral("de")},
        {QStringLiteral("de-de"),   QStringLiteral("de")},
        {QStringLiteral("deu"),     QStringLiteral("de")},
        {QStringLiteral("ger"),     QStringLiteral("de")},

        {QStringLiteral("es"),      QStringLiteral("es")},
        {QStringLiteral("es-es"),   QStringLiteral("es")},
        {QStringLiteral("spa"),     QStringLiteral("es")},

        {QStringLiteral("ru"),      QStringLiteral("ru")},
        {QStringLiteral("ru-ru"),   QStringLiteral("ru")},
        {QStringLiteral("rus"),     QStringLiteral("ru")},

        // 葡语：默认按欧洲葡语，pt-br 单独归一
        {QStringLiteral("pt"),      QStringLiteral("pt")},
        {QStringLiteral("pt-pt"),   QStringLiteral("pt")},
        {QStringLiteral("por"),     QStringLiteral("pt")},
        {QStringLiteral("pt-br"),   QStringLiteral("pt-BR")},
        {QStringLiteral("ptbr"),    QStringLiteral("pt-BR")},

        {QStringLiteral("it"),      QStringLiteral("it")},
        {QStringLiteral("it-it"),   QStringLiteral("it")},
        {QStringLiteral("ita"),     QStringLiteral("it")},
    };
    return table;
}

// 主语言子标签兜底表：en-xyz 之类未列出的区域变体归到主语言。
const QHash<QString, QString> &primaryTable()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("zh"), QStringLiteral("zh")},
        {QStringLiteral("en"), QStringLiteral("en")},
        {QStringLiteral("ja"), QStringLiteral("ja")},
        {QStringLiteral("ko"), QStringLiteral("ko")},
        {QStringLiteral("fr"), QStringLiteral("fr")},
        {QStringLiteral("de"), QStringLiteral("de")},
        {QStringLiteral("es"), QStringLiteral("es")},
        {QStringLiteral("ru"), QStringLiteral("ru")},
        {QStringLiteral("pt"), QStringLiteral("pt")},
        {QStringLiteral("it"), QStringLiteral("it")},
    };
    return table;
}

const QHash<QString, QString> &displayTable()
{
    static const QHash<QString, QString> table = {
        {QStringLiteral("auto"),  QStringLiteral("自动检测")},
        {QStringLiteral("zh"),    QStringLiteral("中文(简体)")},
        {QStringLiteral("zh-TW"), QStringLiteral("中文(繁体)")},
        {QStringLiteral("en"),    QStringLiteral("英语")},
        {QStringLiteral("ja"),    QStringLiteral("日语")},
        {QStringLiteral("ko"),    QStringLiteral("韩语")},
        {QStringLiteral("fr"),    QStringLiteral("法语")},
        {QStringLiteral("de"),    QStringLiteral("德语")},
        {QStringLiteral("es"),    QStringLiteral("西班牙语")},
        {QStringLiteral("ru"),    QStringLiteral("俄语")},
        {QStringLiteral("pt"),    QStringLiteral("葡萄牙语")},
        {QStringLiteral("pt-BR"), QStringLiteral("葡萄牙语(巴西)")},
        {QStringLiteral("it"),    QStringLiteral("意大利语")},
    };
    return table;
}

} // namespace

QString LanguageRegistry::normalize(const QString &code)
{
    const QString raw = code.trimmed();
    if (raw.isEmpty()) {
        return QString();
    }

    QString key = raw.toLower();
    key.replace(QLatin1Char('_'), QLatin1Char('-'));

    // 1) 别名表直接命中
    const auto &aliases = aliasTable();
    const auto it = aliases.constFind(key);
    if (it != aliases.constEnd()) {
        return it.value();
    }

    // 2) zh 前缀特殊处理：带 hant / hk / mo / tw 的变体一律视为繁体
    if (key.startsWith(QLatin1String("zh-"))) {
        if (key.contains(QLatin1String("hant")) || key.contains(QLatin1String("tw"))
            || key.contains(QLatin1String("hk")) || key.contains(QLatin1String("mo"))) {
            return QStringLiteral("zh-TW");
        }
        return QStringLiteral("zh");
    }

    // 3) 主语言子标签兜底（en-xyz -> en）
    const int dash = key.indexOf(QLatin1Char('-'));
    if (dash > 0) {
        const auto &primaries = primaryTable();
        const auto pit = primaries.constFind(key.left(dash));
        if (pit != primaries.constEnd()) {
            return pit.value();
        }
    }

    // 4) 未知：返回小写原值，交给调用方判断
    return key;
}

QString LanguageRegistry::displayName(const QString &internal)
{
    const QString n = normalize(internal);
    const auto &table = displayTable();
    const auto it = table.constFind(n);
    if (it != table.constEnd()) {
        return it.value();
    }
    return internal;
}

QString LanguageRegistry::toGoogle(const QString &internal)
{
    const QString n = normalize(internal);
    if (n == QLatin1String("zh")) {
        return QStringLiteral("zh-CN");
    }
    if (n == QLatin1String("zh-TW")) {
        return QStringLiteral("zh-TW");
    }
    if (n == QLatin1String("pt")) {
        return QStringLiteral("pt-PT");
    }
    if (n == QLatin1String("pt-BR")) {
        return QStringLiteral("pt-BR");
    }
    if (n == QLatin1String("auto")) {
        return QStringLiteral("auto");
    }
    if (isKnown(n)) {
        return n;
    }
    return n.isEmpty() ? QStringLiteral("auto") : n;
}

QString LanguageRegistry::toMyMemory(const QString &internal)
{
    const QString n = normalize(internal);
    if (n == QLatin1String("zh")) {
        return QStringLiteral("zh-CN");
    }
    if (n == QLatin1String("zh-TW")) {
        return QStringLiteral("zh-TW");
    }
    if (n == QLatin1String("pt")) {
        return QStringLiteral("pt-PT");
    }
    if (n == QLatin1String("pt-BR")) {
        return QStringLiteral("pt-BR");
    }
    if (n == QLatin1String("auto")) {
        return QStringLiteral("auto");
    }
    return n;
}

QStringList LanguageRegistry::allInternal()
{
    // 顺序即 UI 下拉框顺序：auto 在最前，随后按使用频率排。
    static const QStringList all = {
        QStringLiteral("auto"),
        QStringLiteral("zh"),
        QStringLiteral("zh-TW"),
        QStringLiteral("en"),
        QStringLiteral("ja"),
        QStringLiteral("ko"),
        QStringLiteral("fr"),
        QStringLiteral("de"),
        QStringLiteral("es"),
        QStringLiteral("ru"),
        QStringLiteral("pt"),
        QStringLiteral("pt-BR"),
        QStringLiteral("it"),
    };
    return all;
}

bool LanguageRegistry::isKnown(const QString &internal)
{
    const QString n = normalize(internal);
    if (n.isEmpty() || n == QLatin1String("auto")) {
        return false;
    }
    return allInternal().contains(n);
}

} // namespace studious::core
