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

// 同一行里横向间隙过大时再切开：导航栏、工具条、面包屑这种「一行里很多个
// 互不相干的标签」，并成一行会把整条栏当成一句话送给模型 —— 翻回来是一整句，
// 贴回去又是一个横跨全宽的长块，必然压到相邻标签上。断点判据是「间隙 > 1.6 倍
// 字高」（0.9 倍是实测线：西文词距一般 0.3~0.5 倍字高，导航栏那种独立标签
// 因为框里带了图标/角标，高度被抬高，间隙也能到 1 倍以上），另加 20px 下限，
// 免得小字号把正常的词间距也当成断点。
constexpr int kSegmentGapFloor = 20;
constexpr qreal kSegmentGapFactor = 0.9;

QVector<Row> splitRow(const Row &row)
{
    QVector<studious::platform::OcrWord> ws = row.words;
    std::sort(ws.begin(), ws.end(),
              [](const studious::platform::OcrWord &a, const studious::platform::OcrWord &b) {
                  return a.box.x() < b.box.x();
              });

    QVector<Row> segments;
    Row current;
    // 间隙要跟**单个词**的高度比，不能跟并出来的行高比：行高是整段并集，
    // 越并越大，阈值跟着虚胖，导航栏那种间距就再也断不开了。
    const studious::platform::OcrWord *previous = nullptr;
    for (int i = 0; i < ws.size(); ++i) {
        const studious::platform::OcrWord &w = ws.at(i);
        if (previous) {
            const int gap = w.box.left() - previous->box.right();
            const int height = qMax(1, qMax(previous->box.height(), w.box.height()));
            const int limit = qMax(kSegmentGapFloor, qRound(height * kSegmentGapFactor));
            if (gap > limit) {
                segments.append(current);
                current = Row();
            }
        }
        current.box = current.words.isEmpty() ? w.box : current.box.united(w.box);
        current.words.append(w);
        previous = &w;
    }
    if (!current.words.isEmpty())
        segments.append(current);
    return segments;
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
        for (const Row &segment : splitRow(row)) {
        OcrLine line;
        line.box = segment.box;
        line.text = lineText(segment);
        // 置信度取**最小**而不是均值：一行里只要有一个词认得很虚，这行就不可信，
        // 均值会把一个 0.2 的噪声词藏在一串 0.99 里。截屏翻译一直这么算，
        // 实时这边照抄 —— 两页的 confidence 才能直接对比（QML 用不用它是另一回事）。
        qreal confidence = 1.0;
        for (const studious::platform::OcrWord &w : segment.words)
            confidence = qMin(confidence, w.confidence);
        line.confidence = segment.words.isEmpty() ? 0.0 : confidence;
        lines.append(line);
        }
    }
    return lines;
}

} // namespace studious::ui
