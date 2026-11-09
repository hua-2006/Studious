#include "Paragraphs.h"

#include <QRegularExpression>

namespace studious::ui::paragraphs {

QStringList split(const QString &text)
{
    // 规则：空行（可含空白字符）是段落分隔；连续空行算一个；首尾空白去掉；丢弃空段。
    static const QRegularExpression lineBreak(QStringLiteral("\r\n|\n|\r"));

    QStringList paragraphs;
    QStringList current;

    const auto flush = [&paragraphs, &current]() {
        if (current.isEmpty()) {
            return;
        }
        const QString para = current.join(QLatin1Char('\n')).trimmed();
        current.clear();
        if (!para.isEmpty()) {
            paragraphs << para;
        }
    };

    const QStringList lines = text.split(lineBreak);
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            flush();
        } else {
            current << line;
        }
    }
    flush();

    return paragraphs;
}

}   // namespace studious::ui::paragraphs
