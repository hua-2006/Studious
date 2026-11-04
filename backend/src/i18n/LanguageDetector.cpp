#include "i18n/LanguageDetector.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace studious::i18n {
namespace {

// 拉丁语系的特征停用词（小写、空格分隔）。
// 只求"区分度"，不求完整词典：每个语言 25~40 个高频词足够判句级文本。
const char *const kLatinStopsEn[] = {
    "the", "and", "is", "are", "was", "were", "you", "your", "for", "not",
    "with", "this", "that", "have", "has", "from", "they", "will", "would", "there",
    "what", "when", "which", "about", "can", "just", "please", "save", "game", nullptr};

const char *const kLatinStopsFr[] = {
    "le", "la", "les", "des", "une", "est", "et", "vous", "nous", "pour",
    "avec", "dans", "que", "qui", "pas", "sur", "plus", "au", "aux", "ce",
    "cette", "être", "avoir", "je", "il", "elle", "bonjour", "merci", nullptr};

const char *const kLatinStopsDe[] = {
    "der", "die", "das", "und", "ist", "nicht", "ein", "eine", "mit", "für",
    "auf", "sie", "wir", "ich", "auch", "den", "dem", "des", "zu", "von",
    "werden", "wird", "sind", "haben", "danke", "bitte", nullptr};

const char *const kLatinStopsEs[] = {
    "el", "la", "los", "las", "es", "y", "que", "para", "con", "por",
    "una", "un", "del", "no", "se", "su", "más", "como", "pero", "este",
    "esta", "hola", "gracias", "usted", "muy", nullptr};

const char *const kLatinStopsPt[] = {
    "o", "a", "os", "as", "de", "é", "e", "que", "para", "com",
    "uma", "um", "do", "da", "não", "se", "seu", "mais", "como", "mas",
    "este", "esta", "olá", "obrigado", "você", nullptr};

const char *const kLatinStopsIt[] = {
    "il", "la", "gli", "le", "di", "che", "per", "con", "non", "un",
    "una", "del", "della", "sono", "sei", "questo", "questa", "più", "come", "ma",
    "ciao", "grazie", "anche", "molto", nullptr};

struct LatinProfile {
    const char *code;
    const char *const *stops;
};

const LatinProfile kLatinProfiles[] = {
    {"en", kLatinStopsEn},
    {"fr", kLatinStopsFr},
    {"de", kLatinStopsDe},
    {"es", kLatinStopsEs},
    {"pt", kLatinStopsPt},
    {"it", kLatinStopsIt},
};

// 简体/繁体判别用的特征字（各挑最常见的几十个）。
const QString kTraditionalOnly = QStringLiteral(
    "們這個說國學會對時個來後發裡麼樣點為與經濟問題東西還開關實現應該機會認為");
const QString kSimplifiedOnly = QStringLiteral(
    "们这个说国学会对时个来后发里么样点为与经济问题东西还开关实现应该机会认为");

// 从字符串数组构造停用词集合（只做一次）。
QSet<QString> buildStops(const char *const *list)
{
    QSet<QString> set;
    for (int i = 0; list[i] != nullptr; ++i) {
        set.insert(QString::fromUtf8(list[i]));
    }
    return set;
}

const QSet<QString> &stopsFor(int index)
{
    static QVector<QSet<QString>> cache;
    if (cache.isEmpty()) {
        for (const LatinProfile &profile : kLatinProfiles) {
            cache.append(buildStops(profile.stops));
        }
    }
    return cache.at(index);
}

bool isLatinLetter(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
           || (u >= 0x00C0 && u <= 0x024F);   // 带变音符号的拉丁字母
}

} // namespace

DetectedLanguage detectLanguage(const QString &text)
{
    DetectedLanguage result;

    int han = 0;
    int kana = 0;
    int hangul = 0;
    int cyrillic = 0;
    int arabic = 0;
    int thai = 0;
    int devanagari = 0;
    int latin = 0;
    int traditionalHits = 0;
    int simplifiedHits = 0;

    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u >= 0x4E00 && u <= 0x9FFF) {
            ++han;
            if (kTraditionalOnly.contains(ch)) {
                ++traditionalHits;
            }
            if (kSimplifiedOnly.contains(ch)) {
                ++simplifiedHits;
            }
        } else if (u >= 0x3040 && u <= 0x30FF) {
            ++kana;
        } else if (u >= 0xAC00 && u <= 0xD7AF) {
            ++hangul;
        } else if (u >= 0x0400 && u <= 0x04FF) {
            ++cyrillic;
        } else if (u >= 0x0600 && u <= 0x06FF) {
            ++arabic;
        } else if (u >= 0x0E00 && u <= 0x0E7F) {
            ++thai;
        } else if (u >= 0x0900 && u <= 0x097F) {
            ++devanagari;
        } else if (isLatinLetter(ch)) {
            ++latin;
        }
    }

    const int cjkTotal = han + kana + hangul;
    const int letters = cjkTotal + cyrillic + arabic + thai + devanagari + latin;
    if (letters == 0) {
        return result;   // 纯符号/数字：无语言可言
    }

    // ---- 文字系统优先：这些判据几乎不会错 ----
    if (kana > 0) {
        result.code = QStringLiteral("ja");
        result.confidence = 0.95;
        return result;
    }
    if (hangul > 0) {
        result.code = QStringLiteral("ko");
        result.confidence = 0.95;
        return result;
    }
    if (cyrillic * 100 / letters >= 60) {
        result.code = QStringLiteral("ru");
        result.confidence = 0.85;
        return result;
    }
    if (arabic * 100 / letters >= 60) {
        result.code = QStringLiteral("ar");
        result.confidence = 0.85;
        return result;
    }
    if (thai * 100 / letters >= 60) {
        result.code = QStringLiteral("th");
        result.confidence = 0.85;
        return result;
    }
    if (devanagari * 100 / letters >= 60) {
        result.code = QStringLiteral("hi");
        result.confidence = 0.85;
        return result;
    }
    if (han * 100 / letters >= 60) {
        // 汉字为主：用特征字区分简繁。样本太少时保持 zh、置信度下调。
        if (traditionalHits > simplifiedHits) {
            result.code = QStringLiteral("zh-TW");
            result.confidence = 0.8;
        } else {
            result.code = QStringLiteral("zh");
            result.confidence = (traditionalHits + simplifiedHits) >= 2 ? 0.85 : 0.7;
        }
        return result;
    }

    // ---- 拉丁语系：停用词打分 ----
    if (latin * 100 / letters >= 60) {
        // 按词切分（只取纯拉丁字母序列，避免标点干扰）
        static const QRegularExpression wordRe(QStringLiteral("[A-Za-z\\x{00C0}-\\x{024F}]+"));
        const QString lower = text.toLower();
        QStringList words;
        auto it = wordRe.globalMatch(lower);
        while (it.hasNext()) {
            const QString w = it.next().captured(0);
            if (w.size() >= 1) {
                words << w;
            }
        }

        if (words.isEmpty()) {
            return result;
        }

        int bestScore = 0;
        int bestIndex = -1;
        for (int i = 0; i < static_cast<int>(std::size(kLatinProfiles)); ++i) {
            const QSet<QString> &stops = stopsFor(i);
            int score = 0;
            for (const QString &w : words) {
                if (stops.contains(w)) {
                    ++score;
                }
            }
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }

        // 没有命中任何停用词时，按英文字母表推断（多数界面文本是英文）。
        if (bestScore == 0) {
            result.code = QStringLiteral("en");
            result.confidence = words.size() >= 6 ? 0.45 : 0.3;
            return result;
        }

        result.code = QString::fromUtf8(kLatinProfiles[bestIndex].code);

        // 置信度 = 命中词占全部词的比例，但给一个较宽的上限：
        // 短句停用词占比天然低，不应被判成"不可信"。
        const double ratio = static_cast<double>(bestScore) / static_cast<double>(words.size());
        double confidence = 0.5 + ratio * 0.4;
        if (words.size() >= 4 && bestScore >= 2) {
            confidence = qMax(confidence, 0.75);
        }
        result.confidence = qMin(0.95, confidence);
        return result;
    }

    // 混合文字（例如中英混排）：以占比最大的为准，置信度打折。
    if (han > 0 && latin > 0) {
        result.code = han >= latin ? QStringLiteral("zh") : QStringLiteral("en");
        result.confidence = 0.5;
        return result;
    }

    return result;
}

} // namespace studious::i18n
