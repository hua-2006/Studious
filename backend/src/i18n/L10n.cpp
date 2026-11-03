#include "i18n/L10n.h"

#include <QLocale>

#include <atomic>

namespace studious::i18n {
namespace {

// -1 = 没有显式覆盖，跟随系统；0 = 强制中文；1 = 强制英文。
// 用 atomic 是因为下载/翻译会跑在工作线程，文案取值必须线程安全。
std::atomic<int> g_override{-1};

Language detectSystemLanguage()
{
    return QLocale::system().language() == QLocale::Chinese
               ? Language::Chinese
               : Language::English;
}

} // namespace

Language current()
{
    const int override = g_override.load(std::memory_order_relaxed);
    if (override < 0) {
        return detectSystemLanguage();
    }
    return override == 0 ? Language::Chinese : Language::English;
}

void setLanguage(Language lang)
{
    g_override.store(lang == Language::Chinese ? 0 : 1, std::memory_order_relaxed);
}

void resetToSystemLanguage()
{
    g_override.store(-1, std::memory_order_relaxed);
}

QString sf(const char *zh, const char *en, const QVariantList &args)
{
    const QString pattern = s(zh, en);
    if (args.isEmpty()) {
        return pattern;
    }

    // 单遍替换 %1..%9：插进去的文本不会再被扫描，
    // 避免参数里出现 "%2" / "%20"（百分号编码的 URL 极常见）时被二次替换。
    QString out;
    out.reserve(pattern.size() + 32);
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('%') && i + 1 < pattern.size()) {
            const QChar digit = pattern.at(i + 1);
            if (digit >= QLatin1Char('1') && digit <= QLatin1Char('9')) {
                const int index = digit.digitValue() - 1;
                if (index < args.size()) {
                    out += args.at(index).toString();
                    ++i;   // 跳过占位符的序号位
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

} // namespace studious::i18n
