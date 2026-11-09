#pragma once

// 「输入文本 → 段落列表」的唯一实现。
//
// 为什么单独抽出来：段落数是**翻译缓存键**的一部分（TranslationCache::makeKey 把
// 单段原文 + 语言对 + 引擎标签算进去），所以「这段输入算 3 段还是 2 段」直接决定
// 命中还是重翻。以前 TranslateJob / QuickTranslate / MiniSession 各有一份切分，
// 规则还不一样（一份认 \r、一份不认；一份把连续空行当一个分隔、一份按正则）。
// 同一段文本在主页算 3 段、在小窗算 2 段，缓存就对不上，用户改个窗口重翻就变慢。
//
// 规则（与历来 TranslateJob::splitParagraphs 完全一致，不许"顺手改进"）：
//   · \r\n、\r、\n 都算换行；
//   · 空行（可含空白字符）是段落分隔，连续空行算一个分隔；
//   · 段内单换行保留为 '\n'；
//   · 每段两端 trim，丢掉空段。

#include <QString>
#include <QStringList>

namespace studious::ui::paragraphs {

QStringList split(const QString &text);

}   // namespace studious::ui::paragraphs
