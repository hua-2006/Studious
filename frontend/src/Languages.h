#pragma once

// 语言的「列表 ↔ 存储字符串」与「按引擎归一」的唯一实现。
//
// 为什么单独抽出来：目标语言在设置里是以逗号分隔的字符串存的
// （settings/lang/dst = "zh,ja"），而 AppBackend / MiniSession / ScreenFlow /
// LiveSession 四个入口以前各自 split 一遍，还各自实现了一遍
// 「本地 MT 只支持单目标语言就截成第一个」。任何一处漏改，就会出现
// 「界面显示三个标签、发单只带一个」这种只有用户能发现的错位。
//
// 约定：
//   · 存进设置的字符串只做 trim + 去空 + 去重 + 保序，不在这里判引擎能力。
//   · 真正发单前调 normalizeTargets()：空列表兜底成 {zh}，本地 MT 只留第一个。

#include <QString>
#include <QStringList>

namespace studious::ui::langs {

// "zh,ja" → {"zh", "ja"}；空串 → 空列表。
QStringList split(const QString &joined);
// {"zh","ja"} → "zh,ja"。
QString join(const QStringList &langs);

// 发单前归一：空 → {"zh"}；local-mt 只留第一个（它按语言对装模型）。
QStringList normalizeTargets(const QStringList &targets, const QString &engineUiId);

// 源语言：「自动」只有一种编码 —— "auto"。
//
// 以前 MiniSession 在内存里用空串表示「自动」、落盘又写 "auto"，
// 其余四个入口一律用 "auto"，两边各判各的。这种漏转不会崩，
// 只会静默翻错语言（"自动检测"被当成"没指定"），最难查。
// 现在统一走这两个函数：存盘、发单、界面比较都用 "auto"。
bool isAuto(const QString &code);
QString normalizeSource(const QString &code);

}   // namespace studious::ui::langs
