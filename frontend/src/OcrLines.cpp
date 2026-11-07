#include "OcrLines.h"

#include <algorithm>
#include <utility>

namespace studious::ui {
namespace {

// 行内的排序与拼接是两步：groupWordsIntoLines() 只保证"哪些词属于同一行"，
// 词的先后由 lineText() 按 x 重排一次 —— 归行是按 y 扫的，同一行里 x 可能是乱的。
struct Row {
    QRect box;
    QVector<studious::platform::OcrWord> words;
};

// 行内按 x 排序，词间距超过字高三分之一才补空格。
// OCR 的分片本身已经补过空格，这里兜住跨分片的情况（一段话被切成两个框）；
// 阈值取 1/3 字高是实测值：西文的词间距一般不超过它，汉字的字间距也够塞下。
QString lineText(const Row &row)
{
    QVector<studious::platform::OcrWord> ws = row.words;
    std::sort(ws.begin(), ws.end(),
              [](const studious::platform::OcrWord &a, const studious::platform::OcrWord &b) {
                  return a.box.x() < b.box.x();
              });
    QString text;
    int prevRight = 0;
    int prevHeight = 0;
    for (int i = 0; i < ws.size(); ++i) {
        const studious::platform::OcrWord &w = ws.at(i);
        if (i > 0) {
            const int gap = w.box.left() - prevRight;
            const int height = qMax(1, qMax(prevHeight, w.box.height()));
            if (gap > height / 3)
                text += QLatin1Char(' ');
        }
        text += w.text.trimmed();
        prevRight = w.box.right();
        prevHeight = w.box.height();
    }
    return text;
}

} // namespace

QVector<OcrLine> groupWordsIntoLines(const QVector<studious::platform::OcrWord> &words)
{
    // 先按 y 扫：同一条水平带上的词才会落进同一个候选行。
    QVector<studious::platform::OcrWord> sorted = words;
    std::sort(sorted.begin(), sorted.end(),
              [](const studious::platform::OcrWord &a, const studious::platform::OcrWord &b) {
                  if (a.box.center().y() != b.box.center().y())
                      return a.box.center().y() < b.box.center().y();
                  return a.box.x() < b.box.x();
              });

    // 归行判据是「垂直重叠超过较矮者一半」而不是「中心点在同一带」：
    // 上下标、不同字号的混排（标题旁贴着的小字）中心点会差得远，
    // 但跟相邻行确实共处一行，用重叠比例才既不断行也不串行。
    QVector<Row> rows;
    for (const studious::platform::OcrWord &w : std::as_const(sorted)) {
        int target = -1;
        for (int i = 0; i < rows.size(); ++i) {
            const QRect box = rows.at(i).box;
            const int overlap = qMin(box.bottom(), w.box.bottom()) - qMax(box.top(), w.box.top());
            const int shorter = qMin(box.height(), w.box.height());
            if (shorter > 0 && overlap * 2 >= shorter) {
                target = i;
                break;
            }
        }
        if (target < 0) {
            Row row;
            row.box = w.box;
            row.words.append(w);
            rows.append(row);
        } else {
            rows[target].box = rows.at(target).box.united(w.box);
            rows[target].words.append(w);
        }
    }

    QVector<OcrLine> lines;
    lines.reserve(rows.size());
    for (const Row &row : std::as_const(rows)) {
        OcrLine line;
        line.box = row.box;
        line.text = lineText(row);
        // 置信度取**最小**而不是均值：一行里只要有一个词认得很虚，这行就不可信，
        // 均值会把一个 0.2 的噪声词藏在一串 0.99 里。截屏翻译一直这么算，
        // 实时这边照抄 —— 两页的 confidence 才能直接对比（QML 用不用它是另一回事）。
        qreal confidence = 1.0;
        for (const studious::platform::OcrWord &w : row.words)
            confidence = qMin(confidence, w.confidence);
        line.confidence = row.words.isEmpty() ? 0.0 : confidence;
        lines.append(line);
    }
    return lines;
}

} // namespace studious::ui
