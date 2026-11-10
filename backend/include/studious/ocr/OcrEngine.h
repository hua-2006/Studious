#pragma once

// 本地 OCR（PP-OCRv4，ONNX Runtime CPU 推理）。
//
// 为什么放在 backend 而不是 frontend：
//   onnxruntime 的头文件目录与静态库只挂在 backend target 上（见 backend/CMakeLists.txt），
//   frontend 拿不到；backend 本来就零 GUI（Qt Core / Gui 的数据类型，不碰 Widgets），
//   OCR 与 backend/src/mt 同级最省事。
//
// 阶段：
//   1) 检测（det.onnx，DBNet）：缩放整图 → 概率图 → 二值化 → 连通域外接框 → 外扩 → 阅读顺序排序；
//   2) 识别（rec.onnx，CRNN + CTC）：逐框裁剪 → 高 48 定宽 320 → CTC 贪心解码 → 字典映射。
//
// 不做 CTRBox 角度分类（PP-OCR 的第三个模型，用来把 0°/90°/180°/270° 归正）：
//   本项目的 OCR 输入是屏幕截图里**轴对齐的界面文字**（按钮、对话、菜单），
//   竖排/旋转文本不是目标场景；砍掉它省一个模型文件、一次推理与一整段旋转后处理。
//   真需要竖排时，正确做法是加 third_party 的 cls.onnx 并在这里按角度旋转裁剪图，
//   而不是在这里猜。

#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

// OCR 结果结构以平台层的定义为准（frontend/src/platform/Types.h），这里不另造一份：
// 平台层 / AppBackend 拿到的就是同一个 studious::platform::OcrWord。
#include "platform/Types.h"

namespace studious::ocr {

struct OcrOptions {
    QStringList languages;      // "zh" / "en"，空 = 中英混合

    // 以下三个阈值留 0 = 按档位（preset）取默认值。以前这里是写死的 0.3 / 0.5 / 1.6，
    // 结果档位改不动它们：调用方拿到的永远是默认拷贝，分不清"没设"和"设成默认"。
    int maxSideLen = 0;         // 送进检测网络前的最长边；0 = 按档位
    float detThreshold = 0.0f;  // DBNet 概率图阈值（连通域平均分的下限）
    float boxThreshold = 0.0f;  // 文本框二值化阈值（概率图 -> 0/1 掩码）
    float unclipRatio = 0.0f;   // 文本框外扩比例（DBNet 的 unclip）
    int threads = 0;            // 0 = 自动（min(8, 核数)）

    // ---- 视觉引擎档位 ----
    // 三档只改"送进网络的分辨率 + 后处理尺度 + 深色反色"，不换模型资产：
    //   0 快速：4M 像素、宽 320、切片 6×字高 —— 实时翻译/连续截屏用。
    //   1 标准：8M 像素、宽 480、切片 8×字高 —— 默认，截图翻译用。
    //   2 精确：12M 像素 + 大图分块按原生分辨率检测、宽 640、切片 12×字高 —— 小字/4K/长行用。
    int preset = 1;

    // 深色底反色：深色主题下是白字黑底，PP-OCR 主要拿浅底黑字训练，不反色会明显掉识别率。
    // 0 = 自动（按整图中位亮度判断）、1 = 始终反色、2 = 从不。
    int invert = 0;

    // 检测像素上限的显式覆盖（0 = 按档位）。档位里的值再走引擎内 1M~16M 的硬夹取。
    qint64 detPixels = 0;
};

class OcrEngine {
public:
    // modelsDir 下应有：det.onnx / rec.onnx / ppocr_keys_v1.txt（字典）。
    // 构造不加载模型（构造不该失败、也不该阻塞）：第一次 isAvailable() / recognize() 时才建会话。
    explicit OcrEngine(const QString &modelsDir);
    ~OcrEngine();

    OcrEngine(const OcrEngine &) = delete;
    OcrEngine &operator=(const OcrEngine &) = delete;

    // 三个文件都在 **且** 两个 ONNX 会话真的建起来了。缺任何一个都返回 false。
    // 缺文件时不会去建会话（省一次几百毫秒的尝试），文件补装后下次调用会自动重试。
    bool isAvailable() const;

    // 缺哪些文件（只给文件名，例如 "det.onnx"；目录在 lastError() 里）。
    // 这里只报"不在 / 不是普通文件"的；文件在但 0 字节属于"坏的"而不是"缺的"，
    // 由 lastError() 报「OCR 模型文件是空的：%1」（见 missingFilesNow 的分工）。
    // 齐全时返回空 list —— 此时是否可用看 isAvailable()（会话可能仍然建不起来）。
    QStringList missingFiles() const;

    // 最近一次操作的错误，给人看的中文（已本地化）。没有错误时为空。
    QString lastError() const;

    // img 是物理像素位图；返回的 box 坐标是**相对于 img 左上角**的物理像素。
    // 整图没有检测到文本框时返回空 list，且不设置 lastError()（"没字"不是错误）。
    // 模型缺失 / 推理失败时同样返回空 list，但 lastError() 有原因 —— 调用方据此区分。
    QVector<studious::platform::OcrWord> recognize(const QImage &img, const OcrOptions &opts);

private:
    struct Session;   // pimpl：头文件里不出现任何 onnxruntime 类型
    std::unique_ptr<Session> m_impl;
};

} // namespace studious::ocr
