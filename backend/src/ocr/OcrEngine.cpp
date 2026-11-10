#include "ocr/OcrEngine.h"

#include "i18n/L10n.h"

#include <onnxruntime_cxx_api.h>

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace studious::ocr {
namespace {

// 资产清单（backend/asset/manifest.json）里 OCR 三条的文件名，顺序与 OcrEngine 的期望一致。
constexpr const char *kDetFile = "det.onnx";
constexpr const char *kRecFile = "rec.onnx";
constexpr const char *kDictFile = "ppocr_keys_v1.txt";

// DBNet 主干有 5 次 stride=2 下采样，输入边长必须是 32 的倍数，否则特征图对不齐。
constexpr int kSizeMultiple = 32;

// 识别网络输入：默认高 48、宽按比例、上限 320（PP-OCRv4 rec 的标准输入 3×48×320）。
// 宽高比更极端的框改成"宽 320 + 高度反算"，右侧 / 下侧补 0。
// 模型若在导出时把 H/W 烧死了（recShape 里是正数），以模型声明为准 —— 在
// recognizeCrop() 里解析，这两个常量只做模型没声明时的兜底。
constexpr int kRecHeight = 48;
constexpr int kRecWidth = 320;

// 检测输入 / 概率图的像素上限不再写死：档位（Tuning::maxPixels）给 4M/8M/12M，
// tuningFor() 里再夹到 1M~16M。上限本身的意义没变 —— extractComponents() 会按 w*h 分配
// uint8 的 seen 加同规模的 int 栈，一张 8000×8000 的截图就是 64 MB + 256 MB，直接把进程吃掉。
// 「精确」档走 detectTiled()：按原生分辨率分块，同样像素量只是分成若干次小推理。

// 自动线程数上限：OCR 是交互式路径（截完图等结果），把核占满只会拖慢 UI。
constexpr int kMaxAutoThreads = 8;

// 检测归一化：ImageNet 均值 / 方差，与 PaddleOCR 的 NormalizeImage(scale=1/255) 一致。
constexpr std::array<float, 3> kDetMean{0.485f, 0.456f, 0.406f};
constexpr std::array<float, 3> kDetStd{0.229f, 0.224f, 0.225f};

// 识别归一化：mean=0.5 std=0.5，等价于 (px/255 - 0.5) / 0.5。
constexpr float kRecMean = 0.5f;
constexpr float kRecStd = 0.5f;

// 连通域过滤：面积 / 最小边都太小的块当噪声扔掉（都在概率图坐标系里，单位=概率图像素）。
constexpr int kMinComponentArea = 4;
constexpr int kMinComponentSide = 2;

// ---- 视觉引擎档位 ----------------------------------------------------------
// 档位只调"送进网络的分辨率 + 后处理尺度"，不换 det/rec 模型：换模型要动资产清单、
// 下载与验签，用户升级不了程序就换不了档；调参数则连重启都不用。
struct Tuning {
    int maxSideLen = 960;      // 检测输入最长边
    qint64 maxPixels = 4'000'000;  // 检测输入 / 概率图像素上限
    int recMaxWidth = 320;     // 识别宽度上限（模型宽度轴是动态时才生效）
    float sliceFactor = 6.0f;  // 超宽行切片宽度 = 字高 × 该系数
    float detThreshold = 0.3f;
    float boxThreshold = 0.5f;
    float unclipRatio = 1.6f;
    bool tile = false;         // 大图分块检测（每块按原生分辨率过一遍网络）
};

Tuning tuningFor(const OcrOptions &opts)
{
    Tuning t;
    switch (opts.preset <= 0 ? 0 : (opts.preset >= 2 ? 2 : 1)) {
    case 0:   // 快速
        t.maxSideLen = 960;
        t.maxPixels = 4'000'000;
        t.recMaxWidth = 320;
        t.sliceFactor = 6.0f;
        break;
    case 2:   // 精确
        t.maxSideLen = 1600;
        t.maxPixels = 12'000'000;
        t.recMaxWidth = 640;
        t.sliceFactor = 12.0f;
        t.detThreshold = 0.25f;   // 放宽召回：小字概率峰值本来就低
        t.boxThreshold = 0.45f;
        t.unclipRatio = 1.7f;
        t.tile = true;
        break;
    default:  // 标准
        t.maxSideLen = 1280;
        t.maxPixels = 8'000'000;
        t.recMaxWidth = 480;
        t.sliceFactor = 8.0f;
        break;
    }

    // 调用方显式给了就尊重（0 = 没给）。
    if (opts.maxSideLen > 0) {
        t.maxSideLen = opts.maxSideLen;
    }
    if (opts.detPixels > 0) {
        t.maxPixels = opts.detPixels;
    }
    if (opts.detThreshold > 0.0f) {
        t.detThreshold = opts.detThreshold;
    }
    if (opts.boxThreshold > 0.0f) {
        t.boxThreshold = opts.boxThreshold;
    }
    if (opts.unclipRatio > 0.0f) {
        t.unclipRatio = opts.unclipRatio;
    }
    // 硬夹取：extractComponents 按 w*h 分配 seen(uint8) + 栈(int)，16M 已经是 80 MB 的量级。
    t.maxPixels = std::clamp<qint64>(t.maxPixels, 1'000'000, 16'000'000);
    t.recMaxWidth = std::clamp(t.recMaxWidth, kRecWidth, 1280);
    t.sliceFactor = std::clamp(t.sliceFactor, 4.0f, 16.0f);
    return t;
}

// 深色底判断：缩到 64×64 求平均亮度。整图逐像素太贵，而"是不是深色主题"是全局属性，
// 64×64 足够（下拉菜单、弹窗这类局部深色块也照样能判出来）。
bool looksDark(const QImage &img)
{
    const QImage small = img.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_Grayscale8);
    qint64 sum = 0;
    int n = 0;
    for (int y = 0; y < small.height(); ++y) {
        const uchar *line = small.constScanLine(y);
        for (int x = 0; x < small.width(); ++x) {
            sum += line[x];
            ++n;
        }
    }
    return n > 0 && (sum / n) < 96;   // 96/255：明显暗于中灰
}

bool debugEnabled()
{
    return qEnvironmentVariableIsSet("STUDIOUS_OCR_DEBUG");
}

QString shapeText(const std::vector<int64_t> &shape)
{
    QStringList parts;
    parts.reserve(static_cast<int>(shape.size()));
    for (const int64_t dim : shape) {
        parts << QString::number(dim);
    }
    return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
}

// 0 = 自动（min(8, 核数)）；正数原样用（上限 32，防止把核数写成几百把机器拖死）。
int resolveThreads(int requested)
{
    if (requested > 0) {
        return std::clamp(requested, 1, 32);
    }
    const int cores = QThread::idealThreadCount();
    return std::clamp(cores > 0 ? cores : 1, 1, kMaxAutoThreads);
}

std::vector<std::string> collectNames(Ort::Session &session, bool wantInputs)
{
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t count = wantInputs ? session.GetInputCount() : session.GetOutputCount();

    std::vector<std::string> names;
    names.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Ort::AllocatedStringPtr name = wantInputs ? session.GetInputNameAllocated(i, allocator)
                                                  : session.GetOutputNameAllocated(i, allocator);
        names.emplace_back(name.get());
    }
    return names;
}

std::vector<int64_t> ioShape(Ort::Session &session, bool wantInput, size_t index)
{
    const Ort::TypeInfo info =
        wantInput ? session.GetInputTypeInfo(index) : session.GetOutputTypeInfo(index);
    return info.GetTensorTypeAndShapeInfo().GetShape();
}

void makePtrs(const std::vector<std::string> &names, std::vector<const char *> &out)
{
    out.clear();
    out.reserve(names.size());
    for (const std::string &name : names) {
        out.push_back(name.c_str());
    }
}

std::unique_ptr<Ort::Session> createSession(Ort::Env &env, const QString &onnxPath, int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    // 整张图是线性的（Conv → BN → ReLU 一路走到底），跨算子并行只会互相等待。
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#if defined(_WIN32)
    // Windows 下 ORTCHAR_T 是 wchar_t：路径走宽字符，中文用户名 / 中文目录才能打开。
    const std::wstring widePath = QDir::toNativeSeparators(onnxPath).toStdWString();
    return std::make_unique<Ort::Session>(env, widePath.c_str(), options);
#else
    const QByteArray localPath = QDir::toNativeSeparators(onnxPath).toLocal8Bit();
    return std::make_unique<Ort::Session>(env, localPath.constData(), options);
#endif
}

int roundToMultiple(int value, int multiple)
{
    if (value <= 0) {
        return multiple;
    }
    const int rounded =
        static_cast<int>(std::lround(static_cast<double>(value) / static_cast<double>(multiple)))
        * multiple;
    return std::max(multiple, rounded);
}

// 缩放到 (w,h) + CHW(float) + 按 (px/255 - mean) / std 归一化。
std::vector<float> toChwFloat(const QImage &src, int w, int h, const std::array<float, 3> &mean,
                              const std::array<float, 3> &stdv)
{
    const QImage rgb = src.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB888);
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<float> out(plane * 3, 0.0f);

    float *rp = out.data();
    float *gp = rp + plane;
    float *bp = rp + 2 * plane;

    for (int y = 0; y < h; ++y) {
        const uchar *line = rgb.constScanLine(y);
        const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            const size_t dst = rowOffset + static_cast<size_t>(x);
            // PaddleOCR 的权重是在 **BGR** 上训的（它先 cv2 解码再 `[:, :, ::-1]`）。
            // 这里 QImage 给的是 RGB，通道顺序必须换过来，否则颜色权重对不上，
            // 检测出来的多边形会碎、识别会串字（实测「always hear about my friends」
            // 会变成「awayshearaboutmfresli」）。
            const float b = static_cast<float>(line[x * 3 + 0]) / 255.0f;
            const float g = static_cast<float>(line[x * 3 + 1]) / 255.0f;
            const float r = static_cast<float>(line[x * 3 + 2]) / 255.0f;
            rp[dst] = (b - mean[0]) / stdv[0];
            gp[dst] = (g - mean[1]) / stdv[1];
            bp[dst] = (r - mean[2]) / stdv[2];
        }
    }
    return out;
}

// 判断一个字符是不是 CJK（中日韩）——切片拼接时中文之间不该插空格。
bool isCjk(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF)
           || (u >= 0xFF00 && u <= 0xFFEF) || (u >= 0x3040 && u <= 0x30FF);
}

// 把切片识别结果拼到累计串上，并去掉重叠部分。
//
// 切片之间刻意留了重叠（防止把字母切两半），代价是相邻两片会把同一段字各认一遍：
// 不做处理时一行会变成「they say their ine is e is such low that th they can' t …」。
// 这里按"累计串的后缀 == 下一片的前缀"找最长重叠（最多 24 字符），从重叠处接上。
void appendSlice(QString *acc, const QString &next, bool wordGap)
{
    if (next.isEmpty()) {
        return;
    }
    if (acc->isEmpty()) {
        *acc = next;
        return;
    }

    const QChar last = acc->at(acc->size() - 1);
    const QChar first = next.at(0);

    // 词边界：两片之间本来就有空白，直接补一个空格接上。
    // **不要在这里做"后缀==前缀"去重** —— 那会把 "make" + "ends" 这种
    // 尾首相同时被当成重叠吃掉一个字母，拼出 "makends"。
    if (wordGap) {
        if (!last.isSpace() && !first.isSpace() && !isCjk(last) && !isCjk(first)) {
            acc->append(QLatin1Char(' '));
        }
        acc->append(next);
        return;
    }

    // 硬切（超长单词 / CJK 密排）：没有留白可依，按"后缀 == 前缀"剥掉重复段。
    const int maxL = std::min(std::min(static_cast<int>(acc->size()), static_cast<int>(next.size())), 24);
    for (int len = maxL; len >= 2; --len) {
        if (acc->right(len) == next.left(len)) {
            acc->append(next.mid(len));
            return;
        }
    }
    acc->append(next);
}

// 把超宽的一行切成"字与字之间"的几段，切点必须落在空白列上。
//
// 为什么不用带重叠的等距切片：重叠区的两片各自识别结果不完全一致
// （「work」可能一边认成「worl」、一边认成「orks」），按字符串去重根本对不上，
// 拼出来就是「he worl orks so hard」。按空白列切就没有重叠区，也不用猜。
QVector<QRect> sliceAtGaps(const QImage &img, const QRect &box, int sliceW, QVector<int> *gapAfter)
{
    QVector<QRect> out;
    if (gapAfter) {
        gapAfter->clear();
    }

    // 先算每一列有没有墨迹（第 0 行以外也算，避免细笔画漏检）。
    const int columns = box.width();
    std::array<int, 256> hist{};
    for (int y = box.top(); y < box.bottom(); y += 2) {
        const uchar *line = img.constScanLine(y);
        for (int x = box.left(); x < box.right(); x += 2) {
            const int l = static_cast<int>(0.299 * line[x * 3] + 0.587 * line[x * 3 + 1]
                                           + 0.114 * line[x * 3 + 2]);
            ++hist[static_cast<size_t>(std::clamp(l, 0, 255))];
        }
    }
    const int bg = static_cast<int>(std::distance(hist.begin(),
                                                  std::max_element(hist.begin(), hist.end())));

    std::vector<int> ink(static_cast<size_t>(columns), 0);
    for (int y = box.top(); y < box.bottom(); ++y) {
        const uchar *line = img.constScanLine(y);
        for (int x = 0; x < columns; ++x) {
            const int px = (box.left() + x) * 3;
            const int l = static_cast<int>(0.299 * line[px] + 0.587 * line[px + 1]
                                           + 0.114 * line[px + 2]);
            if (std::abs(l - bg) > 48) {
                ++ink[static_cast<size_t>(x)];
            }
        }
    }

    // 按"墨迹连续段"切出词（中文字符之间通常也有一两列空白，切出来就是单字，没问题）。
    struct Span { int begin; int end; };
    std::vector<Span> words;
    int x = 0;
    while (x < columns) {
        while (x < columns && ink[static_cast<size_t>(x)] == 0) {
            ++x;
        }
        if (x >= columns) {
            break;
        }
        const int begin = x;
        while (x < columns && ink[static_cast<size_t>(x)] > 0) {
            ++x;
        }
        words.push_back(Span{begin, x});
    }
    // 把"字母缝"并进同一个词：词间距明显大于字母缝。阈值取字高的 12%
    // （22px 正文 ≈ 2.6px，Notepad 的词间距是 8~10px），避免把 "anothe r" 切成两段。
    if (words.size() > 1) {
        const int mergeGap = std::max(2, static_cast<int>(std::lround(box.height() * 0.12)));
        std::vector<Span> mergedWords;
        mergedWords.push_back(words.front());
        for (size_t k = 1; k < words.size(); ++k) {
            const int gap = words[k].begin - mergedWords.back().end;
            if (gap < mergeGap) {
                mergedWords.back().end = words[k].end;
            } else {
                mergedWords.push_back(words[k]);
            }
        }
        words.swap(mergedWords);
    }

    if (words.empty()) {
        out.append(box);
        return out;
    }

    // 按词边界把若干词装进一片（宽度 <= sliceW ≈ 6 倍字高）。
    //
    // 为什么不"一个词一片"：rec 对极小裁剪（"I"、"a"、句号）会乱猜，
    // 实测置信度掉到 0.15、还会多吐字符（"1 1 always"）。整片识别最准；
    // 组与组之间由我们补空格，片内部偶尔会粘词（中文行模型不吐空格），
    // 这是已知限制，用 CTC 列对齐可以再修（见 TODO）。
    size_t i = 0;
    while (i < words.size()) {
        const int startCol = words[i].begin;
        size_t last = i;
        while (last + 1 < words.size() && words[last + 1].end - startCol <= sliceW) {
            ++last;
        }
        const int endCol = words[last].end;
        if (endCol - startCol > sliceW) {
            int cursor = startCol;
            while (cursor < endCol) {
                const int w = std::min(sliceW, endCol - cursor);
                out.append(QRect(box.left() + cursor, box.top(), w, box.height()));
                if (gapAfter) {
                    gapAfter->append(0);
                }
                cursor += w;
            }
        } else {
            out.append(QRect(box.left() + startCol, box.top(), endCol - startCol, box.height()));
            const int gap = (last + 1 < words.size()) ? (words[last + 1].begin - endCol) : 0;
            if (gapAfter) {
                gapAfter->append(gap);
            }
        }
        i = last + 1;
    }    return out;
}
// 概率图里的一个连通域（坐标系 = 概率图，不是原图）。
struct Component {
    int minX = 0;
    int minY = 0;
    int maxX = -1;
    int maxY = -1;
    int area = 0;
    double scoreSum = 0.0;
};

// 概率图 -> 二值 -> 8 邻域连通域。逐像素 BFS，显式栈，不递归（大图递归会爆栈）。
// 8 邻域与 PaddleOCR 用的 cv2.connectedComponents 默认 connectivity=8 一致。
std::vector<Component> extractComponents(const float *prob, int w, int h, float threshold)
{
    std::vector<Component> comps;
    std::vector<uint8_t> seen(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    std::vector<int> stack;
    stack.reserve(1024);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t seed = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            if (seen[seed] != 0 || !(prob[seed] > threshold)) {
                continue;
            }

            Component c;
            c.minX = c.maxX = x;
            c.minY = c.maxY = y;
            seen[seed] = 1;
            stack.push_back(static_cast<int>(seed));

            while (!stack.empty()) {
                const int idx = stack.back();
                stack.pop_back();
                const int cx = idx % w;
                const int cy = idx / w;

                ++c.area;
                c.scoreSum += static_cast<double>(prob[idx]);
                c.minX = std::min(c.minX, cx);
                c.maxX = std::max(c.maxX, cx);
                c.minY = std::min(c.minY, cy);
                c.maxY = std::max(c.maxY, cy);

                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = cy + dy;
                    if (ny < 0 || ny >= h) {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = cx + dx;
                        if (nx < 0 || nx >= w) {
                            continue;
                        }
                        const size_t n = static_cast<size_t>(ny) * static_cast<size_t>(w)
                                         + static_cast<size_t>(nx);
                        if (seen[n] != 0 || !(prob[n] > threshold)) {
                            continue;
                        }
                        seen[n] = 1;
                        stack.push_back(static_cast<int>(n));
                    }
                }
            }
            comps.push_back(c);
        }
    }
    return comps;
}

// DBNet 的 unclip：pyclipper 的 offset 距离 = area * ratio / perimeter。
// 对轴对齐矩形来说，perimeter = 2*(w+h)，四边各外扩 distance —— 与 pyclipper 结果一致。
QRectF unclipRect(const QRect &r, float ratio)
{
    const double w = static_cast<double>(r.width());
    const double h = static_cast<double>(r.height());
    const double perimeter = 2.0 * (w + h);
    if (perimeter <= 0.0) {
        return QRectF(r);
    }
    const double distance = (w * h * static_cast<double>(ratio)) / perimeter;
    return QRectF(r.x() - distance, r.y() - distance, w + 2.0 * distance, h + 2.0 * distance);
}

struct DetBox {
    QRect rect;        // 原图像素
    qreal score = 0.0; // 连通域概率均值
};

// 阅读顺序：先按行（y），行内再按 x。
//
// 纯按 (top, left) 排序在标题 + 副标题混排时会串行：大字号标题的 top 比旁边小字高，
// 却应该是同一行的开头。这里先按 top 粗排，再把垂直中心落在首框 ±半行高以内的并成一行，
// 行内按 left 排 —— 屏幕文字（对话、菜单、按钮）这个启发式够用，不做版面分析。
void sortReadingOrder(std::vector<DetBox> &boxes)
{
    if (boxes.size() < 2) {
        return;
    }

    std::stable_sort(boxes.begin(), boxes.end(), [](const DetBox &a, const DetBox &b) {
        if (a.rect.top() != b.rect.top()) {
            return a.rect.top() < b.rect.top();
        }
        return a.rect.left() < b.rect.left();
    });

    std::vector<DetBox> ordered;
    ordered.reserve(boxes.size());

    size_t i = 0;
    while (i < boxes.size()) {
        const QRect head = boxes[i].rect;
        const int center = head.center().y();
        const int tolerance = std::max(4, head.height() / 2);

        size_t j = i;
        std::vector<DetBox> line;
        while (j < boxes.size()
               && std::abs(boxes[j].rect.center().y() - center) <= tolerance) {
            line.push_back(boxes[j]);
            ++j;
        }
        std::stable_sort(line.begin(), line.end(), [](const DetBox &a, const DetBox &b) {
            if (a.rect.left() != b.rect.left()) {
                return a.rect.left() < b.rect.left();
            }
            return a.rect.top() < b.rect.top();
        });
        for (const DetBox &b : line) {
            ordered.push_back(b);
        }
        i = j;
    }

    boxes.swap(ordered);
}

// argmax 位置的概率：先找到 maxLogit，再算 1/sum(exp(x - max))，省掉一次 argmax。
float softmaxAt(const float *row, int count, float maxLogit)
{
    double sum = 0.0;
    for (int c = 0; c < count; ++c) {
        const float v = row[c];
        if (std::isfinite(v)) {
            sum += std::exp(static_cast<double>(v) - static_cast<double>(maxLogit));
        }
    }
    const float p = static_cast<float>(1.0 / std::max(sum, 1e-12));
    return std::isfinite(p) ? p : 0.0f;
}

// 资产是「一条资产一个目录」装的（<models>/ocr.det.ppocrv4/det.onnx），而引擎只想收一个目录。
// 两种布局都认：文件直接放在 dir 下，或躺在 dir 的某个子目录里。都找不到时返回直连路径，
// 让 missingFiles() 照常报出「缺 det.onnx」这种能看懂的话。
QString resolveAssetFile(const QString &dir, const QString &fileName)
{
    const QString direct = QDir(dir).filePath(fileName);
    if (QFileInfo(direct).isFile())
        return direct;

    const QFileInfoList subdirs =
        QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &sub : subdirs) {
        const QString candidate = QDir(sub.absoluteFilePath()).filePath(fileName);
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return direct;
}

} // namespace

struct OcrEngine::Session {
    explicit Session(const QString &dir)
        : modelsDir(dir),
          detPath(resolveAssetFile(dir, QString::fromLatin1(kDetFile))),
          recPath(resolveAssetFile(dir, QString::fromLatin1(kRecFile))),
          dictPath(resolveAssetFile(dir, QString::fromLatin1(kDictFile)))
    {
    }

    QString modelsDir;
    QString detPath;
    QString recPath;
    QString dictPath;

    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "studious-ocr"};
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;

    // 张量名从模型里读出来，不硬编码：导出脚本换一版名字就变。
    std::vector<std::string> detIn;
    std::vector<std::string> detOut;
    std::vector<std::string> recIn;
    std::vector<std::string> recOut;
    std::vector<const char *> detInPtrs;
    std::vector<const char *> detOutPtrs;
    std::vector<const char *> recInPtrs;
    std::vector<const char *> recOutPtrs;

    std::vector<int64_t> detShape;   // 检测输入的形状（静态导出时能直接读到 H/W）
    std::vector<int64_t> recShape;   // 识别输入的形状（静态导出时一般为 [1,3,48,320]）
    int detOutIndex = 0;             // 概率图输出下标

    std::vector<QString> dict;       // ppocr_keys_v1.txt：每行一个字符，不含 blank

    // ---- 并发保护 ----
    // 界面线程会调 isAvailable()（threads=min(8,核数)），工作线程会调 recognize()（opts.threads）。
    // 以前线程数一不同就 det.reset()/rec.reset()：正在 Run() 的 Ort::Session 被析构 = 野指针，
    // 崩在 onnxruntime 内部。现在：
    //   * mutex 把 load() / detect() / recognizeCrop() 串成一条线（调用方负责持有）；
    //   * 会话只建一次，线程数取**首次** load 的值，之后不同只 warning；
    //   * ready 是会话已就绪的无锁快照，给 isAvailable() 的快路径看。
    mutable std::mutex mutex;
    std::atomic<bool> ready{false};
    // lastError 单独一把锁：读错误信息不该排在整轮识别后面。
    mutable std::mutex errorMutex;

    bool loaded = false;
    int threads = 0;                 // 建会话时用的线程数（首次 load 固定，之后不再变）
    // 本轮识别用的档位参数。recognize() 在拿锁之后按 OcrOptions 算好放进来，
    // detect / recognizeBox / recognizeCrop 直接读 —— 同一实例同时只有一轮识别（mutex 串行化）。
    Tuning tuning;
    int warnedThreads = -1;          // 已经 warning 过的"不同线程数"请求，避免每帧刷日志
    // 字典与模型类别数不匹配只提示一次。以前是函数级 static bool —— 前台截屏与实时翻译
    // 各有一个 OcrEngine/Session（各持各的锁），两个工作线程会并发写同一个 bool。
    // 放成员 + 原子交换，跨实例也不再有数据竞争。
    std::atomic<bool> warnedClassMismatch{false};
    bool loadFailed = false;         // 文件齐但加载失败：不再反复重试
    QString loadError;               // 最近一次加载失败的原因
    QString lastError;               // 最近一次操作（isAvailable / recognize）的错误

    // 三件套的现场体检。返回"不在 / 不是普通文件"的文件名；
    // 普通文件但 0 字节的写进 *emptyFiles（可空）——这两类修法不同，文案也要分开。
    QStringList missingFilesNow(QStringList *emptyFiles = nullptr) const;
    // 调用方必须持有 mutex。
    bool load(int wantThreads);
    bool loadDict(QString *error);
    void setLastError(const QString &text);

    bool detect(const QImage &img, const OcrOptions &opts, std::vector<DetBox> &boxes,
                QString *error);
    // 单块检测（不进分块逻辑）。detect() 按档位决定是直接调它还是走 detectTiled()。
    bool detectOnce(const QImage &img, const OcrOptions &opts, std::vector<DetBox> &boxes,
                    QString *error);
    // 大图分块：每块按原生分辨率过检测网络，再把框平移回原图坐标并去重。
    bool detectTiled(const QImage &img, const OcrOptions &opts, std::vector<DetBox> &boxes,
                     QString *error);
    bool recognizeBox(const QImage &img, const QRect &box, QString *text, qreal *confidence,
                      QString *error);
    // 单片的识别（prepare → run → CTC 解码）。recognizeBox 负责按宽度切片后逐片调它。
    bool recognizeCrop(const QImage &img, const QRect &cropBox, QString *text, qreal *confidence,
                       QString *error);
};

QStringList OcrEngine::Session::missingFilesNow(QStringList *emptyFiles) const
{
    if (emptyFiles != nullptr) {
        emptyFiles->clear();
    }

    // 以前三件套只看 QFileInfo::exists()：一个 0 字节的 det.onnx 会被判成"就绪"，
    // 界面亮绿灯，等真正识别时才炸在 onnxruntime 里（报的是"加载失败"，看不出是文件没下完）。
    // 现在统一按"普通文件且非空"判，并且把「不在」和「在但是空的」分成两份报：
    // 前者去资产页下载，后者得重新下载 / 查磁盘。
    QStringList missing;
    const auto check = [emptyFiles, &missing](const QString &path, const char *name) {
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            missing << QString::fromLatin1(name);   // 不在，或者被同名的目录占了位
            return;
        }
        if (info.size() <= 0 && emptyFiles != nullptr) {
            *emptyFiles << QString::fromLatin1(name);
        }
    };

    check(detPath, kDetFile);
    check(recPath, kRecFile);
    check(dictPath, kDictFile);
    return missing;
}

// 写 lastError 一律走这里：读它的可能是另一个线程（界面查错误原因），
// 单独一把锁，不让它排在整轮识别后面。
void OcrEngine::Session::setLastError(const QString &text)
{
    std::lock_guard<std::mutex> lock(errorMutex);
    lastError = text;
}

bool OcrEngine::Session::loadDict(QString *error)
{
    QFile file(dictPath);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = i18n::sf("OCR 字典打不开：%1", "Cannot open the OCR dictionary: %1", {dictPath});
        return false;
    }

    QString content = QString::fromUtf8(file.readAll());
    if (!content.isEmpty() && content.at(0) == QChar(0xFEFF)) {
        content.remove(0, 1);   // UTF-8 BOM
    }

    QStringList lines = content.split(QLatin1Char('\n'));
    // 文件末尾的换行会多切出一个空串，去掉它；中间的空行保留（否则后面所有 id 全部错位）。
    if (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }

    dict.clear();
    dict.reserve(lines.size());
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        dict.push_back(line);
    }

    if (dict.empty()) {
        *error = i18n::sf("OCR 字典是空的：%1", "The OCR dictionary is empty: %1", {dictPath});
        return false;
    }
    return true;
}

bool OcrEngine::Session::load(int wantThreads)
{
    // 前置条件：调用方持有 mutex（见 OcrEngine::isAvailable / recognize）。
    // 1) 文件检查放最前面：缺文件是最常见的情形（资产还没下），别为它建会话。
    QStringList emptyFiles;
    const QStringList missing = missingFilesNow(&emptyFiles);
    if (!missing.isEmpty() || !emptyFiles.isEmpty()) {
        det.reset();
        rec.reset();
        loaded = false;
        ready = false;
        loadFailed = false;   // 文件又缺了：等补装之后再试
        // 「没下载」和「下下来了但内容是空的」修法不同，文案必须分开 ——
        // 后者说成"缺失"会让用户反复点下载，点完还是 0 字节。
        loadError = missing.isEmpty()
                        ? i18n::sf("OCR 模型文件是空的：%1",
                                   "The OCR model file is empty (0 bytes): %1",
                                   {emptyFiles.join(QStringLiteral("、"))})
                        : i18n::sf("缺少 OCR 模型：%1（目录 %2）",
                                   "Missing OCR models: %1 (directory %2)",
                                   {missing.join(QStringLiteral("、")), modelsDir});
        return false;
    }

    if (loaded) {
        // 线程数只在建会话时生效，这里**不重建**：
        // 重建要先析构正在被另一线程 Run() 的 Ort::Session（UAF），
        // 而且一次重建就是几百毫秒。差异只记一条 warning。
        if (threads != wantThreads && warnedThreads != wantThreads) {
            warnedThreads = wantThreads;
            qWarning().noquote()
                << QStringLiteral("[ocr] 请求线程数 %1 与建会话时的 %2 不同："
                                  "会话只建一次（线程数在首次加载时固定），本次请求被忽略")
                       .arg(wantThreads)
                       .arg(threads);
        }
        return true;
    }
    // 文件齐、会话却建不起来：不再重试 —— 线程数不再是重试的理由，
    // 反复建只会在每次 isAvailable() 上白烧几百毫秒。
    if (loadFailed) {
        return false;
    }

    loadFailed = false;
    loadError.clear();
    ready = false;
    det.reset();
    rec.reset();
    // 首次加载即定型：之后所有请求都用这个线程数。
    threads = wantThreads;

    try {
        det = createSession(env, detPath, threads);
        rec = createSession(env, recPath, threads);

        detIn = collectNames(*det, true);
        detOut = collectNames(*det, false);
        recIn = collectNames(*rec, true);
        recOut = collectNames(*rec, false);
    } catch (const Ort::Exception &ex) {
        loadFailed = true;
        loadError = i18n::sf("OCR 模型加载失败：%1", "Failed to load the OCR models: %1",
                             {QString::fromUtf8(ex.what())});
        det.reset();
        rec.reset();
        return false;
    } catch (const std::exception &ex) {
        loadFailed = true;
        loadError = i18n::sf("OCR 模型加载失败：%1", "Failed to load the OCR models: %1",
                             {QString::fromUtf8(ex.what())});
        det.reset();
        rec.reset();
        return false;
    }

    if (detIn.size() != 1 || recIn.size() != 1) {
        loadFailed = true;
        loadError = i18n::sf("OCR 模型输入个数不对（det=%1 rec=%2，期望各 1 个）",
                             "Unexpected OCR model inputs (det=%1 rec=%2, expected 1 each)",
                             {static_cast<qint64>(detIn.size()), static_cast<qint64>(recIn.size())});
        det.reset();
        rec.reset();
        return false;
    }
    if (detOut.empty() || recOut.empty()) {
        loadFailed = true;
        loadError = i18n::s("OCR 模型没有输出张量", "The OCR models have no output tensors");
        det.reset();
        rec.reset();
        return false;
    }

    detOutIndex = 0;
    for (size_t i = 0; i < detOut.size(); ++i) {
        if (detOut[i].find("prob") != std::string::npos
            || detOut[i].find("sigmoid") != std::string::npos) {
            detOutIndex = static_cast<int>(i);
            break;
        }
    }

    QString dictError;
    if (!loadDict(&dictError)) {
        loadFailed = true;
        loadError = dictError;
        det.reset();
        rec.reset();
        return false;
    }

    makePtrs(detIn, detInPtrs);
    makePtrs(detOut, detOutPtrs);
    makePtrs(recIn, recInPtrs);
    makePtrs(recOut, recOutPtrs);

    // 形状读取要包在 try 里：GetInputTypeInfo 对非张量输入会抛，别让异常漏出 load()。
    try {
        detShape = ioShape(*det, true, 0);
        recShape = ioShape(*rec, true, 0);
    } catch (const Ort::Exception &) {
        detShape.clear();   // 读不到形状就当全动态，按 maxSideLen 算
        recShape.clear();
    }

    loaded = true;
    ready = true;

    qInfo().noquote() << QStringLiteral("[ocr] 已加载：det=%1 %2 rec=%3 %4 字典=%5 行 threads=%6")
                             .arg(QString::fromStdString(detIn.front()),
                                  shapeText(detShape),
                                  QString::fromStdString(recIn.front()),
                                  shapeText(recShape))
                             .arg(dict.size())
                             .arg(threads);
    return true;
}

bool OcrEngine::Session::detect(const QImage &img, const OcrOptions &opts,
                                std::vector<DetBox> &boxes, QString *error)
{
    boxes.clear();
    if (img.isNull()) {
        *error = i18n::s("OCR 检测：图像是空的", "OCR detection: the image is empty");
        return false;
    }

    // 「精确」档才分块：把大图切成若干块，每块按原生分辨率过网络，等于绕过 kMaxDetPixels
    // 的降采样。快速/标准档保持一次过网络（分块要多跑 N 次前向，实时路径受不了）。
    const int longest = std::max(img.width(), img.height());
    if (tuning.tile && longest > tuning.maxSideLen) {
        return detectTiled(img, opts, boxes, error);
    }
    return detectOnce(img, opts, boxes, error);
}

bool OcrEngine::Session::detectTiled(const QImage &img, const OcrOptions &opts,
                                     std::vector<DetBox> &boxes, QString *error)
{
    const int imgW = img.width();
    const int imgH = img.height();
    // 块边长取档位的 maxSideLen（对齐 32）：这样 detectOnce 里 scale==1，是真正的原生分辨率。
    const int side = std::max(kSizeMultiple * 8, roundToMultiple(tuning.maxSideLen, kSizeMultiple));
    // 重叠区必须够宽：一条横跨块边界的文字，两边各只有半条，任何一个框都框不全。
    const int overlap = std::max(64, side / 5);
    const int step = std::max(kSizeMultiple, side - overlap);

    auto offsets = [](int total, int window, int stepLen) {
        std::vector<int> out;
        if (total <= window) {
            out.push_back(0);
            return out;
        }
        int pos = 0;
        while (pos + window < total) {
            out.push_back(pos);
            pos += stepLen;
        }
        out.push_back(total - window);   // 最后一块贴右边，不足的宽度由 copy 裁掉
        return out;
    };

    const std::vector<int> xs = offsets(imgW, side, step);
    const std::vector<int> ys = offsets(imgH, side, step);

    std::vector<DetBox> all;
    for (const int y : ys) {
        for (const int x : xs) {
            const QRect tileRect(x, y, std::min(side, imgW - x), std::min(side, imgH - y));
            if (tileRect.width() < kSizeMultiple || tileRect.height() < kSizeMultiple) {
                continue;
            }
            std::vector<DetBox> part;
            QString tileError;
            if (!detectOnce(img.copy(tileRect), opts, part, &tileError)) {
                *error = tileError;
                return false;
            }
            for (DetBox &box : part) {
                box.rect.translate(tileRect.topLeft());
                all.push_back(box);
            }
        }
    }

    // 去重：重叠区里的同一条文字会被相邻两块各框一次。判定用「交集占小框的比例」，
    // 因为它对半截框更敏感（IoU 在半截框上会偏小，容易漏掉真正的重复）。
    auto area = [](const QRect &r) {
        return static_cast<double>(std::max(0, r.width())) * std::max(0, r.height());
    };
    std::vector<DetBox> kept;
    kept.reserve(all.size());
    for (const DetBox &cand : all) {
        bool duplicate = false;
        for (DetBox &exist : kept) {
            const QRect inter = exist.rect.intersected(cand.rect);
            if (inter.isEmpty()) {
                continue;
            }
            const double ia = area(inter);
            const double ea = area(exist.rect);
            const double ca = area(cand.rect);
            const double iou = ia / std::max(1.0, ea + ca - ia);
            const double cover = ia / std::max(1.0, std::min(ea, ca));
            if (iou > 0.45 || cover > 0.7) {
                // 保留更可信的那个；分数相同就留面积大的（半截框面积小）
                if (cand.score > exist.score
                    || (cand.score == exist.score && ca > ea)) {
                    exist = cand;
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            kept.push_back(cand);
        }
    }

    sortReadingOrder(kept);
    boxes = std::move(kept);
    return true;
}

bool OcrEngine::Session::detectOnce(const QImage &img, const OcrOptions &opts,
                                    std::vector<DetBox> &boxes, QString *error)
{
    const int imgW = img.width();
    const int imgH = img.height();

    // 输入尺寸：模型把 960×960 烧进图里（不少导出脚本这么干）就必须照它的尺寸喂；
    // 动态维度才按 maxSideLen 缩放，并就近取整到 32 的倍数。
    int detW = 0;
    int detH = 0;
    const bool fixedSize = detShape.size() == 4 && detShape[2] > 0 && detShape[3] > 0;
    if (fixedSize) {
        detH = static_cast<int>(detShape[2]);
        detW = static_cast<int>(detShape[3]);
    } else {
        const int maxSide = tuning.maxSideLen;
        const int longest = std::max(imgW, imgH);
        double scale = 1.0;
        if (longest > maxSide) {
            scale = static_cast<double>(maxSide) / static_cast<double>(longest);
        }
        detW = roundToMultiple(static_cast<int>(std::lround(imgW * scale)), kSizeMultiple);
        detH = roundToMultiple(static_cast<int>(std::lround(imgH * scale)), kSizeMultiple);
    }
    if (detW <= 0 || detH <= 0) {
        *error = i18n::s("OCR 检测：输入尺寸算出来是 0", "OCR detection: the input size resolved to 0");
        return false;
    }

    // 尺寸上限：下面的连通域会按 detW*detH 分配 seen（uint8）和等规模的栈，
    // 一张超大图能把内存吃光。动态尺寸就按比例缩回去；模型烧死的尺寸改不了，直接报错。
    if (static_cast<qint64>(detW) * static_cast<qint64>(detH) > tuning.maxPixels) {
        if (fixedSize) {
            *error = i18n::sf("OCR 检测输入过大：模型固定要求 %1×%2（%3 像素），超过上限 %4。\n"
                              "请先裁剪截图再识别。",
                              "The OCR detection input is too large: the model requires %1×%2 "
                              "(%3 pixels), above the limit of %4.\n"
                              "Crop the screenshot before running OCR.",
                              {QString::number(detW), QString::number(detH),
                               QString::number(static_cast<qint64>(detW) * detH),
                               QString::number(tuning.maxPixels)});
            return false;
        }
        const double shrink = std::sqrt(static_cast<double>(tuning.maxPixels)
                                        / (static_cast<double>(detW) * static_cast<double>(detH)));
        detW = std::max(kSizeMultiple,
                        roundToMultiple(static_cast<int>(std::lround(detW * shrink)), kSizeMultiple));
        detH = std::max(kSizeMultiple,
                        roundToMultiple(static_cast<int>(std::lround(detH * shrink)), kSizeMultiple));
        // roundToMultiple 可能往上取（例如 31 -> 32），再压一次，保证硬上限。
        while (static_cast<qint64>(detW) * static_cast<qint64>(detH) > tuning.maxPixels
               && (detW > kSizeMultiple || detH > kSizeMultiple)) {
            if (detW >= detH && detW > kSizeMultiple) {
                detW -= kSizeMultiple;
            } else if (detH > kSizeMultiple) {
                detH -= kSizeMultiple;
            } else {
                break;
            }
        }
    }

    std::vector<float> chw = toChwFloat(img, detW, detH, kDetMean, kDetStd);
    const std::vector<int64_t> inShape{1, 3, detH, detW};

    // 阈值全部来自档位（tuningFor() 里按 preset 定，调用方显式覆盖时才走 opts）。
    const float boxThreshold = tuning.boxThreshold;
    const float detThreshold = tuning.detThreshold;
    const float unclipRatio = tuning.unclipRatio;

    try {
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(memory, chw.data(), chw.size(),
                                                         inShape.data(), inShape.size()));

        std::vector<Ort::Value> outputs = det->Run(Ort::RunOptions{nullptr}, detInPtrs.data(),
                                                  inputs.data(), inputs.size(), detOutPtrs.data(),
                                                  detOutPtrs.size());
        if (outputs.empty() || detOutIndex < 0
            || static_cast<size_t>(detOutIndex) >= outputs.size()) {
            *error = i18n::s("OCR 检测模型没有概率图输出",
                             "The OCR detection model produced no probability map");
            return false;
        }

        Ort::Value &out = outputs[static_cast<size_t>(detOutIndex)];
        const auto info = out.GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> shape = info.GetShape();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            *error = i18n::sf("OCR 检测输出不是 float32：%1",
                              "OCR detection output is not float32: %1", {shapeText(shape)});
            return false;
        }

        int probW = 0;
        int probH = 0;
        if (shape.size() == 4) {
            probH = static_cast<int>(shape[2]);
            probW = static_cast<int>(shape[3]);
        } else if (shape.size() == 3) {
            probH = static_cast<int>(shape[1]);
            probW = static_cast<int>(shape[2]);
        } else if (shape.size() == 2) {
            probH = static_cast<int>(shape[0]);
            probW = static_cast<int>(shape[1]);
        } else {
            *error = i18n::sf("OCR 检测输出维度不对：%1", "Unexpected OCR detection shape: %1",
                              {shapeText(shape)});
            return false;
        }
        if (probW <= 0 || probH <= 0) {
            *error = i18n::sf("OCR 检测输出尺寸非法：%1", "Invalid OCR detection size: %1",
                              {shapeText(shape)});
            return false;
        }

        const float *prob = out.GetTensorData<float>();
        // 概率图尺寸同样要设上限：模型若把输出上采样到比输入还大（少见但合法），
        // extractComponents 的 seen/栈就按这张图的 w*h 分配，一样能把内存吃光。
        if (static_cast<qint64>(probW) * static_cast<qint64>(probH) > tuning.maxPixels) {
            *error = i18n::sf("OCR 检测概率图过大：%1×%2（%3 像素），超过上限 %4。",
                              "The OCR probability map is too large: %1×%2 (%3 pixels), above the "
                              "limit of %4.",
                              {QString::number(probW), QString::number(probH),
                               QString::number(static_cast<qint64>(probW) * probH),
                               QString::number(tuning.maxPixels)});
            return false;
        }
        const std::vector<Component> comps = extractComponents(prob, probW, probH, boxThreshold);

        // 概率图 -> 原图：模型输出可能比输入小（下采样后没上采样回来），用比例换算而不是假设 1:1。
        const double sx = static_cast<double>(imgW) / static_cast<double>(probW);
        const double sy = static_cast<double>(imgH) / static_cast<double>(probH);
        const QRect imageRect(0, 0, imgW, imgH);

        std::vector<DetBox> found;
        found.reserve(comps.size());
        for (const Component &c : comps) {
            if (c.area < kMinComponentArea) {
                continue;
            }
            const double mean = c.scoreSum / static_cast<double>(c.area);
            // 先按平均分过滤（detThreshold），再谈几何：噪声块往往面积不小但分数很低。
            if (mean < static_cast<double>(detThreshold)) {
                continue;
            }

            const QRect maskRect(c.minX, c.minY, c.maxX - c.minX + 1, c.maxY - c.minY + 1);
            if (std::min(maskRect.width(), maskRect.height()) < kMinComponentSide) {
                continue;
            }

            const QRectF grown = unclipRect(maskRect, unclipRatio);
            const QRectF mapped(grown.x() * sx, grown.y() * sy, grown.width() * sx,
                                grown.height() * sy);
            const QRect box = mapped.toAlignedRect().intersected(imageRect);
            if (box.width() < kMinComponentSide || box.height() < kMinComponentSide) {
                continue;
            }

            DetBox item;
            item.rect = box;
            item.score = mean;
            found.push_back(item);
        }

        sortReadingOrder(found);
        boxes = std::move(found);

        if (debugEnabled()) {
            qInfo().noquote() << QStringLiteral("[ocr] det %1x%2 -> %3x%4 阈值(box=%5 det=%6) 连通域=%7 保留=%8")
                                     .arg(imgW)
                                     .arg(imgH)
                                     .arg(detW)
                                     .arg(detH)
                                     .arg(boxThreshold, 0, 'f', 2)
                                     .arg(detThreshold, 0, 'f', 2)
                                     .arg(comps.size())
                                     .arg(boxes.size());
        }
        return true;
    } catch (const Ort::Exception &ex) {
        *error = i18n::sf("OCR 检测推理失败：%1", "OCR detection inference failed: %1",
                          {QString::fromUtf8(ex.what())});
        return false;
    }
}

bool OcrEngine::Session::recognizeBox(const QImage &img, const QRect &box, QString *text,
                                      qreal *confidence, QString *error)
{
    text->clear();
    *confidence = 0.0;

    // 一行很长的文字（截图里是常态：整段正文一行 800~1200 px、字高 30 px）塞进
    // 320×48 的 rec 输入会被横向压掉三四倍，字母直接糊成一团。PaddleOCR 的做法是
    // 保持高 48、宽按比例裁到 320；对超宽行必须**按空白列切片**再拼回来。
    const int sliceW = std::max(64, static_cast<int>(std::lround(box.height() * tuning.sliceFactor)));
    QVector<int> gapAfter;
    const QVector<QRect> slices = sliceAtGaps(img, box, sliceW, &gapAfter);
    if (slices.size() <= 1) {
        return recognizeCrop(img, box, text, confidence, error);
    }

    QString merged;
    qreal minConfidence = 1.0;
    int counted = 0;
    for (int i = 0; i < slices.size(); ++i) {
        const QRect &slice = slices.at(i);
        if (slice.width() < 4) {
            continue;
        }
        QString part;
        qreal conf = 0.0;
        if (!recognizeCrop(img, slice, &part, &conf, error)) {
            return false;
        }
        if (part.isEmpty()) {
            continue;
        }
        // 空白宽到像词间距（> 字高的 1/8）才补空格；字母缝里落下就不补。
        const int gap = gapAfter.value(i, 0);
        const bool wordGap = gap > 0;   // 分组边界就是词边界：有间隙就补一个空格
        appendSlice(&merged, part, wordGap);
        minConfidence = std::min(minConfidence, conf);
        ++counted;
    }

    if (counted == 0) {
        return true;
    }
    *text = merged.simplified();
    *confidence = minConfidence;
    return true;
}

bool OcrEngine::Session::recognizeCrop(const QImage &img, const QRect &box, QString *text,
                                       qreal *confidence, QString *error)
{
    text->clear();
    *confidence = 0.0;

    const QImage crop = img.copy(box);
    if (crop.isNull() || crop.width() < 1 || crop.height() < 1) {
        return true;   // 空裁剪不算失败，只是这一段没字
    }

    // 输入尺寸以模型声明为准：recShape = [N,C,H,W]，静态导出时 H/W 是正数
    // （PP-OCRv4 是 [1,3,48,320]）。只有模型把 H/W 留成动态轴（-1）时才回落到常量。
    // 上限 4096 只是防呆：形状读出来是天文数字时宁可按默认走，也不要分配一块离谱的 buffer。
    int recH = kRecHeight;
    int recW = kRecWidth;
    if (recShape.size() == 4 && recShape[2] > 0 && recShape[3] > 0 && recShape[2] <= 4096
        && recShape[3] <= 4096) {
        recH = static_cast<int>(recShape[2]);
        recW = static_cast<int>(recShape[3]);
    } else if (recShape.size() == 4) {
        // 宽度是动态轴（本机的 rec.onnx 就是 [-1,3,48,-1]）：上限交给档位 —— 宽 320 会把
        // 一整行压到看不清，标准档 480 / 精确档 640 才留得住窄笔画的字间空隙。
        recH = recShape[2] > 0 && recShape[2] <= 4096 ? static_cast<int>(recShape[2]) : kRecHeight;
        recW = tuning.recMaxWidth;
    }

    // 高**保持模型的固定值**（默认 48）：PP-OCR 的 rec 是「高度固定、宽度按比例裁到上限」
    // 训出来的，宽度超过上限时横向压缩（训练里有这类增广），纵向永远不缩。
    // 早先我们按宽高比反算高度（320/ratio），等于把整行字缩小成十几个像素，
    // 实测「they say their income is so low...」会被识成「woud pitfn teln」。
    const double ratio =
        static_cast<double>(crop.width()) / static_cast<double>(std::max(1, crop.height()));
    const int drawH = recH;
    const int drawW = std::clamp(static_cast<int>(std::lround(recH * ratio)), 1, recW);

    const QImage scaled = crop.scaled(drawW, drawH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);

    const size_t plane = static_cast<size_t>(recW) * static_cast<size_t>(recH);
    std::vector<float> chw(plane * 3, 0.0f);
    for (int y = 0; y < drawH; ++y) {
        const uchar *line = scaled.constScanLine(y);
        const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(recW);
        for (int x = 0; x < drawW; ++x) {
            const size_t dst = rowOffset + static_cast<size_t>(x);
            for (int c = 0; c < 3; ++c) {
                // 同上：rec 也要 BGR（c=0 取蓝通道，即 RGB888 里的第 3 个字节）。
                const float v = static_cast<float>(line[x * 3 + (2 - c)]) / 255.0f;
                chw[static_cast<size_t>(c) * plane + dst] = (v - kRecMean) / kRecStd;
            }
        }
    }

    const std::vector<int64_t> inShape{1, 3, recH, recW};

    try {
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> inputs;
        inputs.push_back(Ort::Value::CreateTensor<float>(memory, chw.data(), chw.size(),
                                                         inShape.data(), inShape.size()));

        std::vector<Ort::Value> outputs = rec->Run(Ort::RunOptions{nullptr}, recInPtrs.data(),
                                                  inputs.data(), inputs.size(), recOutPtrs.data(),
                                                  recOutPtrs.size());
        if (outputs.empty()) {
            *error = i18n::s("OCR 识别模型没有输出", "The OCR recognition model produced no output");
            return false;
        }

        const auto info = outputs.front().GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> shape = info.GetShape();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            *error = i18n::sf("OCR 识别输出不是 float32：%1",
                              "OCR recognition output is not float32: %1", {shapeText(shape)});
            return false;
        }
        if (shape.size() != 3) {
            *error = i18n::sf("OCR 识别输出维度不对：%1（期望 3 维 [1,T,C]）",
                              "Unexpected OCR recognition shape: %1 (expected 3 dims [1,T,C])",
                              {shapeText(shape)});
            return false;
        }

        const int steps = static_cast<int>(shape[1]);
        const int classes = static_cast<int>(shape[2]);
        if (steps <= 0 || classes <= 0) {
            *error = i18n::sf("OCR 识别输出尺寸非法：%1", "Invalid OCR recognition size: %1",
                              {shapeText(shape)});
            return false;
        }

        const int dictSize = static_cast<int>(dict.size());
        // 类别数 = 1(blank) + 字典 + 1(空格)：PP-OCR 的模型都是这个布局
        // （v4 中文表 6623 行 → 6625 类；v5 多语种表 18384 行 → 18386 类）。多出的一类就是空格。
        const bool hasSpace = classes > dictSize + 1;
        if (classes != dictSize + 1 && classes != dictSize + 2) {
            // 字典与模型不配套：能跑，但映射会错位。只提示一次，不中断识别。
            if (!warnedClassMismatch.exchange(true, std::memory_order_relaxed)) {
                qWarning().noquote()
                    << QStringLiteral("[ocr] 字典与模型类别数不匹配：字典 %1 行，模型 %2 类。"
                                      "请确认 rec.onnx 与 ppocr_keys_v1.txt 来自同一套 PP-OCR 资产（v4 与 v5 不能混搭）")
                           .arg(dictSize)
                           .arg(classes);
            }
        }

        const float *logits = outputs.front().GetTensorData<float>();

        // 输出的到底是 logits 还是已经 softmax 过的概率，取决于导出脚本：
        // PP-OCR 官方的 rec 导出带 softmax（整行加起来 = 1），有些导出不带。
        // 直接对已经是概率的行走一遍 softmax 会得到 1/类别数 ≈ 0.0002 的"置信度"，
        // 看上去像模型坏了。这里按第一行的行和判断一次，两种都支持。
        bool rowIsProbability = false;
        {
            double rowSum = 0.0;
            bool allFinite = true;
            for (int c = 0; c < classes; ++c) {
                const float v = logits[c];
                if (!std::isfinite(v)) {
                    allFinite = false;
                    break;
                }
                rowSum += static_cast<double>(v);
            }
            rowIsProbability = allFinite && std::abs(rowSum - 1.0) < 0.05;
        }

        // CTC 贪心解码：逐时间步 argmax → 去掉 blank(0) → 去掉连续重复。
        // 注意 prev 连 blank 一起更新，这样 "a blank a" 会输出两个 a（不是 "aa" 折叠成一个）。
        QString result;
        double probSum = 0.0;
        int kept = 0;
        int prev = -1;
        for (int t = 0; t < steps; ++t) {
            const float *row = logits + static_cast<size_t>(t) * static_cast<size_t>(classes);
            int best = 0;
            float bestLogit = row[0];
            for (int c = 1; c < classes; ++c) {
                if (row[c] > bestLogit) {
                    bestLogit = row[c];
                    best = c;
                }
            }
            if (best != 0 && best != prev) {
                if (best >= 1 && best <= dictSize) {
                    result += dict[static_cast<size_t>(best - 1)];
                } else if (hasSpace && best == dictSize + 1) {
                    result += QLatin1Char(' ');
                }
                // 超出字典的类别（字典不配套时）直接丢弃，不猜字。
                probSum += rowIsProbability ? static_cast<double>(bestLogit)
                                            : static_cast<double>(softmaxAt(row, classes, bestLogit));
                ++kept;
            }
            prev = best;
        }

        *text = result;
        *confidence = kept > 0 ? probSum / static_cast<double>(kept) : 0.0;
        return true;
    } catch (const Ort::Exception &ex) {
        *error = i18n::sf("OCR 识别推理失败：%1", "OCR recognition inference failed: %1",
                          {QString::fromUtf8(ex.what())});
        return false;
    }
}

OcrEngine::OcrEngine(const QString &modelsDir) : m_impl(std::make_unique<Session>(modelsDir)) {}

OcrEngine::~OcrEngine() = default;

bool OcrEngine::isAvailable() const
{
    Session &s = *m_impl;

    // 快路径：会话已就绪就看一眼原子标志，不碰锁 —— 识别可能正占着锁跑几百毫秒，
    // 界面按钮的可用性检查不该卡在这上面。
    if (s.ready.load(std::memory_order_acquire)) {
        s.setLastError(QString());
        return true;
    }

    // 首次加载仍然由这里触发（界面在识别之前就要知道能不能用）。
    // 用 try_lock：拿不到锁说明工作线程正在加载 / 正在识别，别插队也别阻塞 GUI，
    // 报当前状态即可（下一次刷新就会看到 true）。
    // 注意：这里**永远不做"重建"**，线程数与建会话时不同也不重建（见 Session::load）。
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return s.ready.load(std::memory_order_acquire);
    }

    const bool ok = s.load(resolveThreads(0));
    s.setLastError(ok ? QString() : s.loadError);
    return ok;
}

QStringList OcrEngine::missingFiles() const
{
    // 只碰路径字符串，不碰会话：不需要加锁。
    return m_impl->missingFilesNow();
}

QString OcrEngine::lastError() const
{
    Session &s = *m_impl;
    std::lock_guard<std::mutex> lock(s.errorMutex);
    return s.lastError;
}

QVector<studious::platform::OcrWord> OcrEngine::recognize(const QImage &img, const OcrOptions &opts)
{
    Session &s = *m_impl;
    s.setLastError(QString());

    QVector<studious::platform::OcrWord> words;

    // 空图 / 1 像素图：没有可检测的内容，返回空且不报错（调用方要区分"没字"和"坏了"）。
    if (img.isNull() || img.width() < 2 || img.height() < 2) {
        return words;
    }

    // 串行化：det/rec 会话、字典、loadError 全是共享可变状态。
    // 两个识别（或界面线程的 isAvailable 撞上首次加载）同时进来会撕裂状态；
    // 更要命的是旧代码会在"线程数不同"时 reset() 掉正在 Run() 的 Ort::Session（UAF）。
    // 锁住整轮识别，同一时刻只有一个推理在跑。
    std::lock_guard<std::mutex> lock(s.mutex);

    // 档位参数每轮现算：detect / recognizeBox / recognizeCrop 都读 s.tuning，
    // 而整轮识别都在锁里，所以不用担心被别的识别线程改掉。
    s.tuning = tuningFor(opts);

    const int threads = resolveThreads(opts.threads);
    if (!s.load(threads)) {
        s.setLastError(s.loadError);
        return words;
    }

    QElapsedTimer timer;
    timer.start();

    // 一次转换，检测与识别共用：Format_RGB888 的 constScanLine 步长 = 3*w。
    QImage rgb = img.convertToFormat(QImage::Format_RGB888);

    // 深色底反色：深色主题下截出来是白字黑底，而 det/rec 都是拿浅底黑字训练的。
    // 自动判定只看整图平均亮度（深色窗口占多数时才判深色），用户也可以强制开/关。
    bool invertForOcr = opts.invert == 1;
    if (opts.invert == 0) {
        invertForOcr = looksDark(rgb);
    }
    if (invertForOcr) {
        rgb.invertPixels(QImage::InvertRgb);
    }

    std::vector<DetBox> boxes;
    QString error;
    if (!s.detect(rgb, opts, boxes, &error)) {
        s.setLastError(error);
        return words;
    }
    const qint64 detMs = timer.elapsed();

    if (boxes.empty()) {
        if (debugEnabled()) {
            qInfo().noquote() << QStringLiteral("[ocr] 没检测到文本框（%1 ms）").arg(detMs);
        }
        return words;   // 整图无框：空 list，lastError 保持空
    }

    words.reserve(static_cast<int>(boxes.size()));
    for (const DetBox &box : boxes) {
        QString text;
        qreal confidence = 0.0;
        if (!s.recognizeBox(rgb, box.rect, &text, &confidence, &error)) {
            s.setLastError(error);
            return {};   // 模型坏了：与其返回半截结果，不如整体报错
        }
        if (text.isEmpty()) {
            continue;   // 检出来但解不出字的框（横线、图标）直接丢
        }

        studious::platform::OcrWord word;
        word.text = text;
        word.box = box.rect;
        word.confidence = confidence;
        words.push_back(word);
    }

    if (debugEnabled()) {
        qInfo().noquote() << QStringLiteral("[ocr] det=%1 ms 总耗时=%2 ms 框=%3 文本=%4 threads=%5 "
                                            "档位=%6 反色=%7 最长边=%8 像素上限=%9 分块=%10")
                                 .arg(detMs)
                                 .arg(timer.elapsed())
                                 .arg(boxes.size())
                                 .arg(words.size())
                                 .arg(s.threads)
                                 .arg(opts.preset)
                                 .arg(invertForOcr ? 1 : 0)
                                 .arg(s.tuning.maxSideLen)
                                 .arg(s.tuning.maxPixels)
                                 .arg(s.tuning.tile ? 1 : 0);
    }
    return words;
}

} // namespace studious::ocr
