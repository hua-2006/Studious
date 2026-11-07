// 「OCR 词 → 行」的唯一一份规则。
//
// 为什么从 ScreenFlow.cpp 里抽出来：实时翻译每 400~1500 ms 就要并一次行，
// 两处各留一份的话，谁先调了空格阈值、谁后调了垂直重叠判定，同一个画面在
// 截屏页与实时页上就会断成不同的行 —— 用户看到的是"同一句话两种贴法"。
// 规则只有一份，改一处两页同时生效。
//
// 这里只做几何 + 拼接，不碰置信度过滤（那是调用方的策略：截屏页丢 0.5 以下，
// 实时页同理，但阈值该由各自的场景决定）。
#pragma once

#include "platform/Types.h"

#include <QRect>
#include <QString>
#include <QVector>

namespace studious::ui {

// 一行文字。box 是这一行所有词的并集，物理像素、相对来源位图左上角；
// confidence 沿用截屏翻译的老口径：取行内**最小**的词置信度（见 .cpp）。
struct OcrLine {
    QRect box;
    QString text;
    qreal confidence = 0.0;
};

QVector<OcrLine> groupWordsIntoLines(const QVector<studious::platform::OcrWord> &words);

} // namespace studious::ui
