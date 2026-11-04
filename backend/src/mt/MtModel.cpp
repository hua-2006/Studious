#include "mt/MtModel.h"

#include "i18n/L10n.h"
#include "mt/MtTokenizer.h"

#include <onnxruntime_cxx_api.h>

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace studious::mt {
namespace {

// 本地 MT 是交互式翻译，抢满核没意义：留余量给 UI 与别的后端任务。
constexpr int kMaxIntraThreads = 4;

// Marian 的静态位置编码上限；超过这个长度的输入只能截断。
constexpr int kMaxPositionEmbeddings = 512;

QString shapeText(const std::vector<int64_t> &shape)
{
    QStringList parts;
    parts.reserve(static_cast<int>(shape.size()));
    for (const int64_t dim : shape) {
        parts << QString::number(dim);
    }
    return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
}

QString typeText(ONNXTensorElementDataType type)
{
    switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
        return QStringLiteral("float32");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
        return QStringLiteral("int64");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
        return QStringLiteral("int32");
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
        return QStringLiteral("bool");
    default:
        return QStringLiteral("type#%1").arg(static_cast<int>(type));
    }
}

bool nameContains(const std::string &name, const char *needle)
{
    return name.find(needle) != std::string::npos;
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
    const Ort::TypeInfo info = wantInput ? session.GetInputTypeInfo(index) : session.GetOutputTypeInfo(index);
    return info.GetTensorTypeAndShapeInfo().GetShape();
}

ONNXTensorElementDataType ioType(Ort::Session &session, bool wantInput, size_t index)
{
    const Ort::TypeInfo info = wantInput ? session.GetInputTypeInfo(index) : session.GetOutputTypeInfo(index);
    return info.GetTensorTypeAndShapeInfo().GetElementType();
}

std::unique_ptr<Ort::Session> createSession(Ort::Env &env, const QString &onnxPath, int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // KV cache 每一步的 past 长度都在变，memory pattern 复用不上还要每次重规划；
    // 关掉它能把 decode 的每步开销压低（只影响内存分配策略，不影响数值结果）。
    options.DisableMemPattern();

#if defined(_WIN32)
    // Windows 下 ORTCHAR_T 是 wchar_t：ONNX 路径走宽字符，
    // 否则中文用户名/中文目录直接打不开模型。
    const std::wstring widePath = QDir::toNativeSeparators(onnxPath).toStdWString();
    return std::make_unique<Ort::Session>(env, widePath.c_str(), options);
#else
    const QByteArray localPath = QDir::toNativeSeparators(onnxPath).toLocal8Bit();
    return std::make_unique<Ort::Session>(env, localPath.constData(), options);
#endif
}

// 把 std::vector<int64_t> 转成 Ort 的张量值（不拷贝数据，缓冲区由调用方持有）。
Ort::Value int64Tensor(Ort::MemoryInfo &memory, std::vector<int64_t> &data, const std::vector<int64_t> &shape)
{
    return Ort::Value::CreateTensor<int64_t>(memory, data.data(), data.size(), shape.data(), shape.size());
}

Ort::Value floatTensor(Ort::MemoryInfo &memory, std::vector<float> &data, const std::vector<int64_t> &shape)
{
    return Ort::Value::CreateTensor<float>(memory, data.data(), data.size(), shape.data(), shape.size());
}

// HF 的 clean_up_tokenization_spaces 等价物：
// 模型偶尔会先吐一个纯 "▁"（空格）再吐 "。"，反分词后就成了"保存进度 。"。
QString cleanupTranslation(const QString &raw)
{
    QString text = raw.trimmed();
    static const QRegularExpression beforeCjkPunct(
        QStringLiteral("[ \\t]+(?=[，。、；：？！）】》」』…—])"));
    static const QRegularExpression afterOpenPunct(
        QStringLiteral("(?<=[（【《「『])[ \\t]+"));
    text.remove(beforeCjkPunct);
    text.remove(afterOpenPunct);
    return text.trimmed();
}

// KV cache 名字解析：optimum 导出的 merged 模型用
//   past_key_values.<layer>.<decoder|encoder>.<key|value>
//   present.<layer>.<decoder|encoder>.<key|value>
// 不硬编码层数/顺序：层数、是不是 encoder KV、顺序全从模型里读出来。
struct KvName {
    bool ok = false;
    int layer = -1;
    bool isDecoder = false;
    bool isKey = false;
};

KvName parseKvName(const std::string &name, const char *prefix)
{
    KvName out;
    const std::string head(prefix);
    if (name.size() <= head.size() || name.compare(0, head.size(), head) != 0) {
        return out;
    }

    const std::string rest = name.substr(head.size());   // "3.decoder.key"
    const size_t dot1 = rest.find('.');
    if (dot1 == std::string::npos || dot1 == 0) {
        return out;
    }
    const std::string layerPart = rest.substr(0, dot1);
    const std::string tail = rest.substr(dot1 + 1);      // "decoder.key"
    const size_t dot2 = tail.find('.');
    if (dot2 == std::string::npos || dot2 == 0) {
        return out;
    }
    const std::string side = tail.substr(0, dot2);
    const std::string kind = tail.substr(dot2 + 1);

    if ((kind != "key" && kind != "value") || (side != "decoder" && side != "encoder")) {
        return out;
    }

    int layer = 0;
    for (const char ch : layerPart) {
        if (ch < '0' || ch > '9') {
            return out;
        }
        layer = layer * 10 + (ch - '0');
    }

    out.ok = true;
    out.layer = layer;
    out.isDecoder = (side == "decoder");
    out.isKey = (kind == "key");
    return out;
}

// 一层 KV cache 张量（float32）。形状统一按 [batch, heads, seq, head_dim]，
// 与 merged 模型 present.* 的输出布局一致。
struct CacheTensor {
    std::vector<float> data;
    int64_t batch = 0;
    int64_t heads = 0;
    int64_t seq = 0;
    int64_t dim = 0;
};

struct Candidate {
    double score = 0.0;
    int parent = 0;
    int token = 0;
};

// top-K（分数降序）。K = 2*num_beams 很小，线性有序插入比建堆更省事。
class TopK {
public:
    explicit TopK(size_t k) : m_k(k) { m_items.reserve(k); }

    void offer(int parent, int token, double score)
    {
        if (m_items.size() >= m_k && score <= m_items.back().score) {
            return;
        }
        const Candidate cand{score, parent, token};
        const auto pos = std::lower_bound(m_items.begin(), m_items.end(), cand,
                                          [](const Candidate &a, const Candidate &b) {
                                              return a.score > b.score;
                                          });
        m_items.insert(pos, cand);
        if (m_items.size() > m_k) {
            m_items.pop_back();
        }
    }

    const std::vector<Candidate> &items() const { return m_items; }

private:
    size_t m_k;
    std::vector<Candidate> m_items;
};

} // namespace

struct MtModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "studious-mt"};
    std::unique_ptr<Ort::Session> encoder;
    // 非 cache 解码器：Sprint 1 的老路径，也是 merged 加载失败时的 fallback。
    std::unique_ptr<Ort::Session> decoder;
    // 带 KV cache 的 merged 解码器（首步 past 长度为 0，之后只喂 1 个新 token）。
    std::unique_ptr<Ort::Session> decoderMerged;

    MtTokenizer sourceTok;
    MtTokenizer targetTok;

    // 张量名一律从模型里读出来存着，不硬编码猜：量化导出脚本改个名字就崩的坑不踩。
    std::vector<std::string> encInputs;
    std::vector<std::string> encOutputs;
    std::vector<std::string> decInputs;
    std::vector<std::string> decOutputs;
    std::vector<std::string> mergedInputs;
    std::vector<std::string> mergedOutputs;
    std::vector<const char *> encInputPtrs;
    std::vector<const char *> encOutputPtrs;
    std::vector<const char *> decInputPtrs;
    std::vector<const char *> decOutputPtrs;
    std::vector<const char *> mergedInputPtrs;
    std::vector<const char *> mergedOutputPtrs;

    // merged 模型的输入按“语义”分类，运行时按分类填张量（顺序照旧按模型给的）。
    enum class InKind {
        Unknown,
        InputIds,
        EncoderMask,
        EncoderHidden,
        DecoderMask,
        UseCacheBranch,
        Past,
    };

    struct PastSlot {
        int layer = -1;
        bool isDecoder = false;
        bool isKey = false;
        int inputIndex = -1;    // merged 输入下标
        int mergedOut = -1;     // merged 输出下标（present.*）
        int legacyOut = -1;     // 非 cache decoder 的同名输出下标
    };

    std::vector<InKind> mergedKinds;
    std::vector<int> mergedSlotOfInput;             // Past 输入 -> pastSlots 下标
    std::vector<std::vector<int64_t>> mergedInShapes;
    std::vector<PastSlot> pastSlots;
    int mergedUseCacheInput = -1;
    bool mergedUseCacheIsBool = true;
    int mergedLogitsOut = -1;
    int decLogitsOut = -1;

    // 空 past 的张量需要一个非空指针（长度 0，数据不会被读）。
    std::vector<float> emptyPlaceholder{0.0f};

    int64_t kvHeads = 0;
    int64_t kvDim = 0;
    // merged 可用：能加载 + 空 past 探针通过（或首步借非 cache 模型后能接上）。
    bool kvReady = false;
    // merged 不接受 seq_len=0 的空 past 时，首步用非 cache 模型跑 1 个 token，
    // 拿它的 present.* 当 cache 起点，从第 2 步开始走 merged。
    bool seedWithLegacy = false;

    // config.json 里的生成参数。
    int decoderStartId = 65000;
    int eosId = 0;
    int padId = 65000;
    int maxLength = 512;
    int dModel = 512;
    int attentionHeads = 8;
    int numBeams = 4;
    double lengthPenalty = 1.0;

    bool loaded = false;

    bool readConfig(const QString &configPath, QString *error);
    bool buildMergedPlan(QString *why);
    bool probeMerged(QString *why);

    // maxSteps > 0：只跑这么多步（load 期探针用）。ranSteps：实际成功执行的步数。
    bool generate(const std::vector<float> &encHidden,
                  int64_t srcLen,
                  QVector<int> &outIds,
                  QString &error,
                  int maxSteps = 0,
                  int *ranSteps = nullptr);
};

namespace {

void makePtrs(const std::vector<std::string> &names, std::vector<const char *> &out)
{
    out.clear();
    out.reserve(names.size());
    for (const std::string &name : names) {
        out.push_back(name.c_str());
    }
}

} // namespace

bool MtModel::Impl::readConfig(const QString &configPath, QString *error)
{
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = i18n::sf("模型配置打不开：%1", "Cannot open model config: %1", {configPath});
        }
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error != nullptr) {
            *error = i18n::sf("模型配置解析失败：%1（%2）",
                              "Failed to parse model config: %1 (%2)",
                              {configPath, parseError.errorString()});
        }
        return false;
    }

    const QJsonObject obj = doc.object();
    decoderStartId = obj.value(QStringLiteral("decoder_start_token_id")).toInt(decoderStartId);
    eosId = obj.value(QStringLiteral("eos_token_id")).toInt(eosId);
    padId = obj.value(QStringLiteral("pad_token_id")).toInt(padId);
    maxLength = obj.value(QStringLiteral("max_length")).toInt(maxLength);
    dModel = obj.value(QStringLiteral("d_model")).toInt(dModel);
    numBeams = obj.value(QStringLiteral("num_beams")).toInt(numBeams);
    lengthPenalty = obj.value(QStringLiteral("length_penalty")).toDouble(lengthPenalty);
    attentionHeads = obj.value(QStringLiteral("decoder_attention_heads")).toInt(attentionHeads);
    if (attentionHeads <= 0) {
        attentionHeads = 8;
    }
    return true;
}

// 从 merged 模型的输入/输出名里推出 KV cache 的接线方式。
// 任何一项对不上（未知输入、缺 present、名字不是 optimum 布局）都判不可用 ——
// 宁可退回能跑的老路径，也不瞎猜 shape 把译文搞坏。
bool MtModel::Impl::buildMergedPlan(QString *why)
{
    mergedKinds.assign(mergedInputs.size(), InKind::Unknown);
    mergedSlotOfInput.assign(mergedInputs.size(), -1);
    mergedInShapes.assign(mergedInputs.size(), {});
    pastSlots.clear();
    mergedUseCacheInput = -1;
    mergedLogitsOut = -1;

    for (size_t i = 0; i < mergedInputs.size(); ++i) {
        mergedInShapes[i] = ioShape(*decoderMerged, true, i);
    }

    for (size_t i = 0; i < mergedInputs.size(); ++i) {
        const std::string &name = mergedInputs[i];

        const KvName kv = parseKvName(name, "past_key_values.");
        if (kv.ok) {
            int slot = -1;
            for (size_t s = 0; s < pastSlots.size(); ++s) {
                if (pastSlots[s].layer == kv.layer && pastSlots[s].isDecoder == kv.isDecoder
                    && pastSlots[s].isKey == kv.isKey) {
                    slot = static_cast<int>(s);
                    break;
                }
            }
            if (slot < 0) {
                PastSlot fresh;
                fresh.layer = kv.layer;
                fresh.isDecoder = kv.isDecoder;
                fresh.isKey = kv.isKey;
                pastSlots.push_back(fresh);
                slot = static_cast<int>(pastSlots.size()) - 1;
            }
            pastSlots[static_cast<size_t>(slot)].inputIndex = static_cast<int>(i);
            mergedKinds[i] = InKind::Past;
            mergedSlotOfInput[i] = slot;
            continue;
        }

        if (name == "use_cache_branch") {
            mergedKinds[i] = InKind::UseCacheBranch;
            mergedUseCacheInput = static_cast<int>(i);
            mergedUseCacheIsBool =
                ioType(*decoderMerged, true, i) == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
            continue;
        }
        if (nameContains(name, "encoder_hidden_states")) {
            mergedKinds[i] = InKind::EncoderHidden;
            continue;
        }
        if (nameContains(name, "encoder_attention_mask")) {
            mergedKinds[i] = InKind::EncoderMask;
            continue;
        }
        if (nameContains(name, "attention_mask")) {
            mergedKinds[i] = InKind::DecoderMask;
            continue;
        }
        if (nameContains(name, "input_ids")) {
            mergedKinds[i] = InKind::InputIds;
            continue;
        }

        if (why != nullptr) {
            *why = i18n::sf("merged decoder 有无法识别的输入：%1",
                            "Unrecognized input on the merged decoder: %1",
                            {QString::fromStdString(name)});
        }
        return false;
    }

    for (size_t i = 0; i < mergedOutputs.size(); ++i) {
        if (mergedLogitsOut < 0 && nameContains(mergedOutputs[i], "logits")) {
            mergedLogitsOut = static_cast<int>(i);
        }
        const KvName pv = parseKvName(mergedOutputs[i], "present.");
        if (!pv.ok) {
            continue;
        }
        for (PastSlot &slot : pastSlots) {
            if (slot.layer == pv.layer && slot.isDecoder == pv.isDecoder && slot.isKey == pv.isKey) {
                slot.mergedOut = static_cast<int>(i);
                break;
            }
        }
    }

    if (mergedLogitsOut < 0 || pastSlots.empty()) {
        if (why != nullptr) {
            *why = i18n::s("merged decoder 没有 logits 或没有 present.* 输出",
                           "The merged decoder has no logits or no present.* outputs");
        }
        return false;
    }

    // 非 cache decoder 的同名输出（首步借它起 cache 时要用）。
    decLogitsOut = -1;
    for (size_t i = 0; i < decOutputs.size(); ++i) {
        if (decLogitsOut < 0 && nameContains(decOutputs[i], "logits")) {
            decLogitsOut = static_cast<int>(i);
        }
        const KvName pv = parseKvName(decOutputs[i], "present.");
        if (!pv.ok) {
            continue;
        }
        for (PastSlot &slot : pastSlots) {
            if (slot.layer == pv.layer && slot.isDecoder == pv.isDecoder && slot.isKey == pv.isKey) {
                slot.legacyOut = static_cast<int>(i);
                break;
            }
        }
    }

    for (const PastSlot &slot : pastSlots) {
        if (slot.inputIndex < 0 || slot.mergedOut < 0) {
            if (why != nullptr) {
                *why = i18n::sf("KV cache 槽位不完整（layer=%1 %2 %3）",
                                "Incomplete KV-cache slot (layer=%1 %2 %3)",
                                {QString::number(slot.layer),
                                 QString::fromLatin1(slot.isDecoder ? "decoder" : "encoder"),
                                 QString::fromLatin1(slot.isKey ? "key" : "value")});
            }
            return false;
        }
    }

    return true;
}

// load 期探针：用一段全 0 的 encoder 输出跑 2 步，验证
//   1) merged 接受 seq_len=0 的空 past；
//   2) 第 2 步能把 present.* 填回 past_key_values.* 并继续往前跑。
// 顺带把 ORT 的图优化/内存分配预热掉（第一个真实句子的耗时会更好看）。
bool MtModel::Impl::probeMerged(QString *why)
{
    const int64_t srcLen = 2;
    const std::vector<float> hidden(static_cast<size_t>(srcLen) * static_cast<size_t>(dModel), 0.0f);

    const auto run = [&](int steps, int *ranSteps, QString *err) {
        QVector<int> ids;
        return generate(hidden, srcLen, ids, *err, steps, ranSteps);
    };

    int ran = 0;
    QString err;
    try {
        (void)run(2, &ran, &err);
    } catch (const Ort::Exception &ex) {
        // 典型报错：past 的 seq 维不接受 0。改走“首步借非 cache 模型”的混合路径。
        if (why != nullptr) {
            *why = QString::fromUtf8(ex.what());
        }
        seedWithLegacy = true;
        if (pastSlots.front().legacyOut < 0) {
            return false;   // 非 cache 模型没有 present.*，混合路径也搭不起来
        }
        QString secondError;
        int ranSecond = 0;
        try {
            (void)run(2, &ranSecond, &secondError);
        } catch (const Ort::Exception &ex2) {
            if (why != nullptr) {
                *why = QString::fromUtf8(ex2.what());
            }
            return false;
        }
        return ranSecond >= 2;
    }

    if (ran < 2) {
        // 步数不够说明 generate 提前退了（例如模型首个预测就是 eos）——
        // 这不算 cache 坏，但也没证明第 2 步能用，稳妥起见按不可用处理。
        if (why != nullptr) {
            *why = err.isEmpty()
                       ? i18n::s("KV cache 探针未跑满 2 步", "KV-cache probe ran fewer than 2 steps")
                       : err;
        }
        return false;
    }
    return true;
}

MtModel::MtModel() : m_impl(std::make_unique<Impl>()) {}

MtModel::~MtModel() = default;

bool MtModel::load(const MtPaths &paths, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    Impl &m = *m_impl;
    m.loaded = false;
    m.kvReady = false;
    m.seedWithLegacy = false;

    const auto requireFile = [error](const QString &path, const char *zh, const char *en) {
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            return true;
        }
        if (error != nullptr) {
            *error = i18n::sf(zh, en, {path});
        }
        return false;
    };

    if (!requireFile(paths.encoderOnnx, "编码器模型不存在：%1", "Encoder model not found: %1")
        || !requireFile(paths.decoderOnnx, "解码器模型不存在：%1", "Decoder model not found: %1")
        || !requireFile(paths.sourceSpm, "源语言分词模型不存在：%1", "Source tokenizer model not found: %1")
        || !requireFile(paths.targetSpm, "目标语言分词模型不存在：%1", "Target tokenizer model not found: %1")
        || !requireFile(paths.configJson, "模型配置文件不存在：%1", "Model config not found: %1")) {
        return false;
    }

    if (!m.readConfig(paths.configJson, error)) {
        return false;
    }

    // beam 数：默认取 config.json，环境变量优先（STUDIOUS_MT_BEAMS=1 即贪心）。
    const QString beamsEnv = qEnvironmentVariable("STUDIOUS_MT_BEAMS").trimmed();
    if (!beamsEnv.isEmpty()) {
        bool ok = false;
        const int requested = beamsEnv.toInt(&ok);
        if (ok && requested > 0) {
            m.numBeams = requested;
        } else {
            qWarning().noquote() << QStringLiteral("[mt] 忽略非法的 STUDIOUS_MT_BEAMS=%1").arg(beamsEnv);
        }
    }
    m.numBeams = std::clamp(m.numBeams, 1, 16);

    // 模型词表：调用方给了就用给的，没给就找 config.json 同目录的 vocab.json。
    QString vocabPath = paths.vocabJson;
    if (vocabPath.isEmpty()) {
        const QString sibling =
            QFileInfo(paths.configJson).absoluteDir().filePath(QStringLiteral("vocab.json"));
        if (QFileInfo::exists(sibling)) {
            vocabPath = sibling;
        }
    }

    // merged（带 KV cache）模型：调用方没给路径就找非 cache 模型的同目录兄弟文件。
    QString mergedPath = paths.decoderMergedOnnx;
    if (mergedPath.isEmpty()) {
        const QString sibling = QFileInfo(paths.decoderOnnx)
                                    .absoluteDir()
                                    .filePath(QStringLiteral("decoder_model_merged_quantized.onnx"));
        if (QFileInfo::exists(sibling)) {
            mergedPath = sibling;
        }
    }
    // STUDIOUS_MT_NO_CACHE=1：强制走非 cache 老路径（A/B 对比与排障用）。
    if (qEnvironmentVariableIsSet("STUDIOUS_MT_NO_CACHE")) {
        mergedPath.clear();
    }
    const bool mergedExists = !mergedPath.isEmpty() && QFileInfo::exists(mergedPath);

    const int threads = std::clamp(QThread::idealThreadCount() > 0 ? QThread::idealThreadCount() : 1,
                                   1,
                                   kMaxIntraThreads);
    try {
        m.encoder = createSession(m.env, paths.encoderOnnx, threads);
        m.decoder = createSession(m.env, paths.decoderOnnx, threads);
        if (mergedExists) {
            m.decoderMerged = createSession(m.env, mergedPath, threads);
        }
    } catch (const Ort::Exception &ex) {
        if (error != nullptr) {
            *error = i18n::sf("ONNX 会话创建失败：%1", "Failed to create ONNX session: %1", {QString::fromUtf8(ex.what())});
        }
        return false;
    }

    if (!m.sourceTok.load(paths.sourceSpm, vocabPath, error)
        || !m.targetTok.load(paths.targetSpm, vocabPath, error)) {
        return false;
    }

    try {
        m.encInputs = collectNames(*m.encoder, true);
        m.encOutputs = collectNames(*m.encoder, false);
        m.decInputs = collectNames(*m.decoder, true);
        m.decOutputs = collectNames(*m.decoder, false);
        if (m.decoderMerged) {
            m.mergedInputs = collectNames(*m.decoderMerged, true);
            m.mergedOutputs = collectNames(*m.decoderMerged, false);
        }

        // 名字 / 类型 / shape 一起打出来：ONNX 导出脚本换一版就差一个 "_quantized" 后缀，
        // 或者把 past 拆成不同命名，真出问题时有这行日志能省半小时。
        const auto dump = [](const char *tag, Ort::Session &session, const std::vector<std::string> &names,
                             bool wantInput) {
            for (size_t i = 0; i < names.size(); ++i) {
                qInfo().noquote() << QStringLiteral("[mt] %1 %2[%3] %4 %5 %6")
                                         .arg(QString::fromLatin1(tag),
                                              wantInput ? QStringLiteral("in") : QStringLiteral("out"))
                                         .arg(i)
                                         .arg(QString::fromStdString(names[i]),
                                              typeText(ioType(session, wantInput, i)),
                                              shapeText(ioShape(session, wantInput, i)));
            }
        };
        dump("encoder", *m.encoder, m.encInputs, true);
        dump("encoder", *m.encoder, m.encOutputs, false);
        dump("decoder", *m.decoder, m.decInputs, true);
        dump("decoder", *m.decoder, m.decOutputs, false);
        if (m.decoderMerged) {
            dump("decoder-merged", *m.decoderMerged, m.mergedInputs, true);
            dump("decoder-merged", *m.decoderMerged, m.mergedOutputs, false);
        }
    } catch (const Ort::Exception &ex) {
        if (error != nullptr) {
            *error = i18n::sf("读取 ONNX 张量信息失败：%1", "Failed to read ONNX tensor info: %1", {QString::fromUtf8(ex.what())});
        }
        return false;
    }

    makePtrs(m.encInputs, m.encInputPtrs);
    makePtrs(m.encOutputs, m.encOutputPtrs);
    makePtrs(m.decInputs, m.decInputPtrs);
    makePtrs(m.decOutputs, m.decOutputPtrs);
    makePtrs(m.mergedInputs, m.mergedInputPtrs);
    makePtrs(m.mergedOutputs, m.mergedOutputPtrs);

    // 非 cache 解码器：logits 输出下标（老路径 + 混合首步都要用）。
    m.decLogitsOut = -1;
    for (size_t i = 0; i < m.decOutputs.size(); ++i) {
        if (nameContains(m.decOutputs[i], "logits")) {
            m.decLogitsOut = static_cast<int>(i);
            break;
        }
    }

    // KV cache 接线 + 探针。任何一步失败都只降级、不失败：Sprint 1 的贪心路径照常能跑。
    if (m.decoderMerged) {
        QString why;
        if (!m.buildMergedPlan(&why)) {
            qWarning().noquote() << QStringLiteral("[mt] KV cache 不可用，退回非 cache 解码：%1").arg(why);
        } else {
            m.kvHeads = m.attentionHeads;
            m.kvDim = m.dModel / std::max(1, m.attentionHeads);
            if (m.dModel % m.attentionHeads != 0) {
                m.kvDim = 64;
            }

            QString probeWhy;
            if (m.probeMerged(&probeWhy)) {
                m.kvReady = true;
                if (m.seedWithLegacy) {
                    qInfo().noquote()
                        << QStringLiteral("[mt] merged 不接受空 past，首步改走非 cache 模型再用 present 起 cache：%1")
                               .arg(probeWhy);
                }
            } else {
                qWarning().noquote()
                    << QStringLiteral("[mt] KV cache 探针失败，退回非 cache 解码：%1").arg(probeWhy);
            }
        }
    }

    qInfo().noquote() << QStringLiteral("[mt] threads=%1 decoder_start=%2 eos=%3 pad=%4 max_length=%5 "
                                        "vocab(src)=%6 vocab(tgt)=%7 eos(src)=%8 vocab.json=%9")
                             .arg(threads)
                             .arg(m.decoderStartId)
                             .arg(m.eosId)
                             .arg(m.padId)
                             .arg(m.maxLength)
                             .arg(m.sourceTok.vocabSize())
                             .arg(m.targetTok.vocabSize())
                             .arg(m.sourceTok.eosId())
                             .arg(vocabPath.isEmpty() ? QStringLiteral("<none>") : vocabPath);
    qInfo().noquote() << QStringLiteral("[mt] kv_cache=%1 beams=%2 length_penalty=%3 kv_slots=%4 merged=%5")
                             .arg(m.kvReady ? QStringLiteral("on") : QStringLiteral("off"))
                             .arg(m.kvReady ? m.numBeams : 1)
                             .arg(m.lengthPenalty)
                             .arg(m.pastSlots.size())
                             .arg(mergedExists ? mergedPath : QStringLiteral("<none>"));

    m.loaded = true;
    return true;
}

bool MtModel::Impl::generate(const std::vector<float> &encHidden,
                             int64_t srcLen,
                             QVector<int> &outIds,
                             QString &error,
                             int maxSteps,
                             int *ranSteps)
{
    outIds.clear();
    if (ranSteps != nullptr) {
        *ranSteps = 0;
    }

    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    const bool cached = kvReady;
    // 没有 KV cache 就不做 beam：每步重算整段前缀，多 beam 纯线性加开销。
    const int beamCount = cached ? std::max(1, numBeams) : 1;
    const int cap = maxSteps > 0
                        ? maxSteps
                        : std::max(1, std::min({maxLength,
                                                kMaxPositionEmbeddings - 1,
                                                static_cast<int>(srcLen) * 4 + 32}));

    const size_t hiddenRow = encHidden.size();
    std::vector<int64_t> srcMaskRow(static_cast<size_t>(srcLen), 1);
    const std::vector<int64_t> srcMaskRowShape{1, srcLen};

    struct BeamPath {
        std::vector<int> tokens;
        double score = 0.0;
        int parent = 0;
    };

    std::vector<BeamPath> beams(1);   // 一条空假设：第 0 步由它分出 beamCount 条
    std::vector<BeamPath> finished;
    std::vector<CacheTensor> cache(pastSlots.size());
    int64_t cacheRows = 0;   // cache 里现在有几行（= 上一步的 beam 数）
    int64_t pastLen = 0;     // decoder 侧 cache 的实际长度（= 已解码的 token 数）
    int executed = 0;

    for (int step = 0; step < cap && !beams.empty(); ++step) {
        const int batch = static_cast<int>(beams.size());

        // ---- 1) cache 按父索引重排（beam 换序 / beam 收缩时必须做）----
        std::vector<int> parents(static_cast<size_t>(batch));
        for (int i = 0; i < batch; ++i) {
            parents[i] = beams[i].parent;
        }

        if (cached && cacheRows == 0) {
            // 首步：past 是 seq_len=0 的空张量，heads/head_dim 先按 config 推。
            for (CacheTensor &t : cache) {
                t.batch = batch;
                t.heads = kvHeads;
                t.seq = 0;
                t.dim = kvDim;
                t.data.clear();
            }
            cacheRows = batch;
        } else if (cached) {
            bool identity = batch == static_cast<int>(cacheRows);
            for (int i = 0; identity && i < batch; ++i) {
                identity = parents[static_cast<size_t>(i)] == i;
            }
            if (!identity) {
                for (CacheTensor &t : cache) {
                    if (t.batch <= 0) {
                        continue;
                    }
                    const size_t rowElems = static_cast<size_t>(t.heads * t.seq * t.dim);
                    if (rowElems == 0) {
                        t.batch = batch;
                        continue;
                    }
                    std::vector<float> gathered(rowElems * static_cast<size_t>(batch));
                    for (int i = 0; i < batch; ++i) {
                        const int p = std::clamp(parents[static_cast<size_t>(i)], 0,
                                                 static_cast<int>(t.batch) - 1);
                        std::memcpy(gathered.data() + static_cast<size_t>(i) * rowElems,
                                    t.data.data() + static_cast<size_t>(p) * rowElems,
                                    rowElems * sizeof(float));
                    }
                    t.data.swap(gathered);
                    t.batch = batch;
                }
                cacheRows = batch;
            }
        }

        // ---- 2) 组装这一 step 的输入 ----
        std::vector<int64_t> idsData(static_cast<size_t>(batch));
        for (int i = 0; i < batch; ++i) {
            idsData[static_cast<size_t>(i)] = static_cast<int64_t>(
                step == 0 ? decoderStartId : beams[static_cast<size_t>(i)].tokens.back());
        }
        const std::vector<int64_t> idsShape{batch, 1};

        std::vector<float> encBatch(hiddenRow * static_cast<size_t>(batch));
        for (int i = 0; i < batch; ++i) {
            std::copy(encHidden.begin(), encHidden.end(),
                      encBatch.begin() + static_cast<size_t>(i) * hiddenRow);
        }
        const std::vector<int64_t> encBatchShape{batch, srcLen, dModel};

        std::vector<int64_t> maskBatch(static_cast<size_t>(batch) * static_cast<size_t>(srcLen), 1);
        const std::vector<int64_t> maskBatchShape{batch, srcLen};

        // merged 不接受空 past 时，首步借非 cache 模型跑（batch 恒为 1）。
        const bool legacyStep = !cached || (step == 0 && seedWithLegacy);

        std::vector<Ort::Value> outputs;
        const float *logits = nullptr;
        int64_t seqOut = 0;
        int64_t vocab = 0;

        if (legacyStep) {
            if (batch != 1) {
                error = i18n::s("非 cache 解码只支持单条假设（num_beams=1）",
                                "The non-cached decoder only supports a single hypothesis (num_beams=1)");
                return false;
            }
            std::vector<int64_t> full{static_cast<int64_t>(decoderStartId)};
            full.insert(full.end(), beams.front().tokens.begin(), beams.front().tokens.end());
            const std::vector<int64_t> fullShape{1, static_cast<int64_t>(full.size())};

            std::vector<Ort::Value> values;
            values.reserve(decInputs.size());
            for (const std::string &name : decInputs) {
                // 顺序要紧：encoder_attention_mask 里也含 "attention_mask"，
                // 必须先把 encoder_hidden_states / encoder_attention_mask 摘出去。
                if (nameContains(name, "encoder_hidden_states")) {
                    values.push_back(floatTensor(memory, encBatch, encBatchShape));
                } else if (nameContains(name, "encoder_attention_mask")) {
                    values.push_back(int64Tensor(memory, srcMaskRow, srcMaskRowShape));
                } else if (nameContains(name, "attention_mask")) {
                    values.push_back(int64Tensor(memory, srcMaskRow, srcMaskRowShape));
                } else if (nameContains(name, "input_ids")) {
                    values.push_back(int64Tensor(memory, full, fullShape));
                } else {
                    error = i18n::sf("无法识别的解码器输入张量：%1",
                                     "Unrecognized decoder input tensor: %1",
                                     {QString::fromStdString(name)});
                    return false;
                }
            }

            outputs = decoder->Run(Ort::RunOptions{nullptr},
                                   decInputPtrs.data(),
                                   values.data(),
                                   values.size(),
                                   decOutputPtrs.data(),
                                   decOutputPtrs.size());
            if (outputs.empty() || decLogitsOut < 0) {
                error = i18n::s("解码器没有 logits 输出", "Decoder produced no logits output");
                return false;
            }

            const auto info = outputs[static_cast<size_t>(decLogitsOut)].GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> shape = info.GetShape();
            if (shape.size() != 3 || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                error = i18n::sf("解码器 logits 形状/类型不对：%1", "Unexpected decoder logits: %1",
                                 {shapeText(shape)});
                return false;
            }
            logits = outputs[static_cast<size_t>(decLogitsOut)].GetTensorData<float>();
            seqOut = shape[1];
            vocab = shape[2];

            // 首步借非 cache 模型起 cache：把它的 present.* 收进来。
            if (cached) {
                for (size_t i = 0; i < pastSlots.size(); ++i) {
                    const int outIdx = pastSlots[i].legacyOut;
                    if (outIdx < 0) {
                        continue;
                    }
                    const auto pv = outputs[static_cast<size_t>(outIdx)].GetTensorTypeAndShapeInfo();
                    const std::vector<int64_t> pvShape = pv.GetShape();
                    if (pvShape.size() != 4) {
                        error = i18n::sf("present 张量维度不对：%1", "Unexpected present tensor: %1",
                                         {shapeText(pvShape)});
                        return false;
                    }
                    CacheTensor &t = cache[i];
                    t.batch = pvShape[0];
                    t.heads = pvShape[1];
                    t.seq = pvShape[2];
                    t.dim = pvShape[3];
                    const float *src = outputs[static_cast<size_t>(outIdx)].GetTensorData<float>();
                    t.data.assign(src, src + static_cast<size_t>(pv.GetElementCount()));
                    if (pastSlots[i].isDecoder && pastSlots[i].isKey) {
                        pastLen = t.seq;
                    }
                }
                cacheRows = batch;
            }
        } else {
            std::vector<int64_t> decMask;
            std::vector<int64_t> decMaskShape;
            // use_cache_branch 只有一个，用固定缓冲区：vector 扩容会让 Ort::Value 拿到的指针失效。
            std::array<bool, 1> useCacheBool{};
            std::array<int64_t, 1> useCacheInt{};

            const auto cacheTensorValue = [&](CacheTensor &t) {
                const std::vector<int64_t> shape{t.batch, t.heads, t.seq, t.dim};
                float *ptr = t.data.empty() ? emptyPlaceholder.data() : t.data.data();
                return Ort::Value::CreateTensor<float>(memory, ptr, t.data.size(), shape.data(),
                                                      shape.size());
            };

            const bool useCacheFlag = pastLen > 0;

            std::vector<Ort::Value> values;
            values.reserve(mergedInputs.size());
            for (size_t i = 0; i < mergedInputs.size(); ++i) {
                switch (mergedKinds[i]) {
                case InKind::InputIds:
                    values.push_back(int64Tensor(memory, idsData, idsShape));
                    break;
                case InKind::EncoderMask:
                    values.push_back(int64Tensor(memory, maskBatch, maskBatchShape));
                    break;
                case InKind::EncoderHidden:
                    values.push_back(floatTensor(memory, encBatch, encBatchShape));
                    break;
                case InKind::DecoderMask: {
                    if (decMask.empty()) {
                        // 解码器自注意力 mask：历史 cache + 当前这 1 个 token。
                        decMask.assign(static_cast<size_t>(batch) * static_cast<size_t>(pastLen + 1), 1);
                        decMaskShape = {batch, pastLen + 1};
                    }
                    values.push_back(int64Tensor(memory, decMask, decMaskShape));
                    break;
                }
                case InKind::UseCacheBranch: {
                    const std::vector<int64_t> shape = mergedInShapes[i];
                    if (mergedUseCacheIsBool) {
                        useCacheBool[0] = useCacheFlag;
                        Ort::Value v = Ort::Value::CreateTensor<bool>(memory, useCacheBool.data(),
                                                                      useCacheBool.size(),
                                                                      shape.data(), shape.size());
                        values.push_back(std::move(v));
                    } else {
                        useCacheInt[0] = useCacheFlag ? 1 : 0;
                        Ort::Value v = Ort::Value::CreateTensor<int64_t>(memory, useCacheInt.data(),
                                                                         useCacheInt.size(),
                                                                         shape.data(), shape.size());
                        values.push_back(std::move(v));
                    }
                    break;
                }
                case InKind::Past:
                    values.push_back(cacheTensorValue(cache[static_cast<size_t>(mergedSlotOfInput[i])]));
                    break;
                case InKind::Unknown:
                default:
                    error = i18n::s("merged 解码器输入计划不完整", "Merged decoder input plan is incomplete");
                    return false;
                }
            }

            try {
                outputs = decoderMerged->Run(Ort::RunOptions{nullptr},
                                             mergedInputPtrs.data(),
                                             values.data(),
                                             values.size(),
                                             mergedOutputPtrs.data(),
                                             mergedOutputPtrs.size());
            } catch (const Ort::Exception &ex) {
                // 带上 step/batch/past 长度再抛：光看 ORT 的节点名根本不知道是哪一步炸的。
                qWarning().noquote()
                    << QStringLiteral("[mt] merged decoder Run 失败：step=%1 batch=%2 use_cache=%3 past=%4：%5")
                           .arg(step)
                           .arg(batch)
                           .arg(useCacheFlag ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(pastLen)
                           .arg(QString::fromUtf8(ex.what()));
                throw;
            }

            if (outputs.empty() || mergedLogitsOut < 0) {
                error = i18n::s("merged 解码器没有 logits 输出", "Merged decoder produced no logits");
                return false;
            }

            const auto info = outputs[static_cast<size_t>(mergedLogitsOut)].GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> shape = info.GetShape();
            if (shape.size() != 3 || info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                error = i18n::sf("merged 解码器 logits 形状/类型不对：%1",
                                 "Unexpected merged decoder logits: %1",
                                 {shapeText(shape)});
                return false;
            }
            logits = outputs[static_cast<size_t>(mergedLogitsOut)].GetTensorData<float>();
            seqOut = shape[1];
            vocab = shape[2];

            // present.* -> 下一步的 past_key_values.*
            for (size_t i = 0; i < pastSlots.size(); ++i) {
                const int outIdx = pastSlots[i].mergedOut;
                if (outIdx < 0) {
                    continue;
                }
                const auto pv = outputs[static_cast<size_t>(outIdx)].GetTensorTypeAndShapeInfo();
                const std::vector<int64_t> pvShape = pv.GetShape();
                if (pvShape.size() != 4) {
                    error = i18n::sf("present 张量维度不对：%1", "Unexpected present tensor: %1",
                                     {shapeText(pvShape)});
                    return false;
                }
                CacheTensor &t = cache[i];
                const size_t elementCount = static_cast<size_t>(pv.GetElementCount());
                if (elementCount == 0) {
                    // cache 分支里模型对 encoder KV 可能只回一个空张量（表示“沿用旧的”），
                    // 这种情况保留上一步的 cache，别把它覆盖成 0 长度。
                    continue;
                }
                t.batch = pvShape[0];
                t.heads = pvShape[1];
                t.seq = pvShape[2];
                t.dim = pvShape[3];
                const float *src = outputs[static_cast<size_t>(outIdx)].GetTensorData<float>();
                t.data.assign(src, src + elementCount);
                if (pastSlots[i].isDecoder && pastSlots[i].isKey) {
                    pastLen = t.seq;
                }
            }
            cacheRows = batch;
        }

        ++executed;
        if (ranSteps != nullptr) {
            *ranSteps = executed;
        }

        if (vocab <= 0 || seqOut <= 0) {
            error = i18n::sf("解码器输出 shape 异常：%1", "Unexpected decoder output shape: %1",
                             {shapeText({batch, seqOut, vocab})});
            return false;
        }

        // ---- 3) 每个 beam 取最后一行的 logits -> log_softmax -> top-K ----
        TopK top(static_cast<size_t>(2 * beamCount));
        for (int b = 0; b < batch; ++b) {
            const size_t rowOffset = (static_cast<size_t>(b) * static_cast<size_t>(seqOut)
                                      + static_cast<size_t>(seqOut - 1))
                                     * static_cast<size_t>(vocab);
            const float *row = logits + rowOffset;

            // STUDIOUS_MT_DEBUG=1 时把首步 top-5 打出来：id 空间对不对，一眼能看出来。
            if (step == 0 && b == 0 && qEnvironmentVariableIsSet("STUDIOUS_MT_DEBUG")) {
                std::vector<int64_t> topIds(static_cast<size_t>(vocab));
                for (int64_t v = 0; v < vocab; ++v) {
                    topIds[static_cast<size_t>(v)] = v;
                }
                std::partial_sort(topIds.begin(), topIds.begin() + 5, topIds.end(),
                                  [row](int64_t x, int64_t y) { return row[x] > row[y]; });
                QStringList line;
                for (int i = 0; i < 5; ++i) {
                    const int64_t id = topIds[static_cast<size_t>(i)];
                    line << QStringLiteral("%1=%2(%3)")
                                .arg(id)
                                .arg(row[id], 0, 'f', 3)
                                .arg(QString::fromStdString(targetTok.pieceOf(static_cast<int>(id))));
                }
                qInfo().noquote() << "[mt][debug] step0 top5:" << line.join(QStringLiteral("  "));
            }

            float maxLogit = -std::numeric_limits<float>::infinity();
            for (int64_t v = 0; v < vocab; ++v) {
                const float s = row[v];
                if (std::isfinite(s) && s > maxLogit) {
                    maxLogit = s;
                }
            }
            if (!std::isfinite(maxLogit)) {
                error = i18n::s("解码取 argmax 失败（logits 全是无效值）",
                                "Decode failed: argmax over logits produced nothing");
                return false;
            }

            double expSum = 0.0;
            for (int64_t v = 0; v < vocab; ++v) {
                if (std::isfinite(row[v])) {
                    expSum += std::exp(static_cast<double>(row[v]) - static_cast<double>(maxLogit));
                }
            }
            const double logZ = static_cast<double>(maxLogit) + std::log(std::max(expSum, 1e-30));
            const double base = beams[static_cast<size_t>(b)].score;

            for (int64_t v = 0; v < vocab; ++v) {
                // pad 与 decoder_start 同 id（65000），config 的 bad_words_ids 也禁它，这里同样排除。
                if (v == padId) {
                    continue;
                }
                const float s = row[v];
                if (!std::isfinite(s)) {
                    continue;
                }
                top.offer(b, static_cast<int>(v), base + (static_cast<double>(s) - logZ));
            }
        }

        // ---- 4) 收束 / 展开 ----
        std::vector<BeamPath> next;
        next.reserve(static_cast<size_t>(beamCount));

        const auto &candidates = top.items();
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate &cand = candidates[i];
            const BeamPath &parent = beams[static_cast<size_t>(cand.parent)];

            if (cand.token == eosId) {
                finished.push_back(BeamPath{parent.tokens, cand.score, cand.parent});
                if (beamCount == 1) {
                    break;   // 贪心语义：argmax 就是 eos，立刻收工
                }
                continue;
            }
            if (static_cast<int>(next.size()) >= beamCount) {
                break;
            }
            BeamPath nb;
            nb.tokens = parent.tokens;
            nb.tokens.push_back(cand.token);
            nb.score = cand.score;
            nb.parent = cand.parent;
            next.push_back(std::move(nb));
        }

        beams = std::move(next);
        if (beams.empty()) {
            break;   // 所有假设都收束了
        }
        if (static_cast<int>(finished.size()) >= beamCount) {
            break;   // 启发式早停：已经攒够 beamCount 条完整假设
        }
    }

    // ---- 5) 选最优：按 length_penalty 归一化累计 log-prob ----
    const auto normalized = [this](const BeamPath &b) {
        const double len = std::max(1.0, static_cast<double>(b.tokens.size()));
        return b.score / std::pow(len, lengthPenalty);
    };

    const BeamPath *best = nullptr;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (const BeamPath &b : finished) {
        const double s = normalized(b);
        if (s > bestScore) {
            bestScore = s;
            best = &b;
        }
    }
    if (best == nullptr) {
        for (const BeamPath &b : beams) {
            const double s = normalized(b);
            if (s > bestScore) {
                bestScore = s;
                best = &b;
            }
        }
    }

    if (best == nullptr || best->tokens.empty()) {
        error = i18n::s("解码未产生任何 token（首个预测就是 eos）",
                        "Decoding produced no tokens (first prediction was eos)");
        return false;
    }

    outIds.reserve(static_cast<int>(best->tokens.size()));
    for (const int id : best->tokens) {
        outIds.append(id);
    }
    return true;
}

bool MtModel::translate(const QString &text, QString &outText, QString &error)
{
    outText.clear();
    error.clear();

    Impl &m = *m_impl;
    if (!m.loaded) {
        error = i18n::s("本地 MT 模型尚未加载", "Local MT model is not loaded");
        return false;
    }

    QVector<int> srcIds = m.sourceTok.encode(text);
    if (srcIds.isEmpty()) {
        error = i18n::s("输入句子分词后为空", "Tokenizer produced no tokens for the input");
        return false;
    }

    // 编码侧补 </s>：Marian 的 encoder 必须看见句尾符。id 由**模型词表**决定
    // （合并词表里 </s>=0），不是 spm 自己的 eos（那是 2，在合并词表里是逗号）。
    const int srcEos = m.sourceTok.eosId();
    if (srcIds.size() + 1 > std::min(m.maxLength, kMaxPositionEmbeddings)) {
        srcIds.resize(std::min(m.maxLength, kMaxPositionEmbeddings) - 1);
    }
    srcIds.append(srcEos);

    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeDefault);

    // 只在 STUDIOUS_MT_DEBUG 下用来拆 encoder / decoder 耗时。
    QElapsedTimer timer;
    timer.start();

    const int64_t srcLen = srcIds.size();
    std::vector<int64_t> srcData;
    srcData.reserve(static_cast<size_t>(srcLen));
    for (const int id : srcIds) {
        srcData.push_back(id);
    }
    std::vector<int64_t> srcMask(static_cast<size_t>(srcLen), 1);
    std::vector<int64_t> srcShape{1, srcLen};
    std::vector<int64_t> maskShape{1, srcLen};

    try {
        // ---- encoder ----
        std::vector<Ort::Value> encValues;
        encValues.reserve(m.encInputs.size());
        for (const std::string &name : m.encInputs) {
            if (nameContains(name, "attention_mask")) {
                encValues.push_back(int64Tensor(memory, srcMask, maskShape));
            } else if (nameContains(name, "input_ids")) {
                encValues.push_back(int64Tensor(memory, srcData, srcShape));
            } else {
                error = i18n::sf("无法识别的编码器输入张量：%1",
                                 "Unrecognized encoder input tensor: %1",
                                 {QString::fromStdString(name)});
                return false;
            }
        }

        std::vector<Ort::Value> encOutputs =
            m.encoder->Run(Ort::RunOptions{nullptr},
                           m.encInputPtrs.data(),
                           encValues.data(),
                           encValues.size(),
                           m.encOutputPtrs.data(),
                           m.encOutputPtrs.size());
        if (encOutputs.empty()) {
            error = i18n::s("编码器没有输出张量", "Encoder produced no output tensors");
            return false;
        }

        size_t hiddenIndex = 0;
        for (size_t i = 0; i < m.encOutputs.size(); ++i) {
            if (nameContains(m.encOutputs[i], "last_hidden_state")
                || nameContains(m.encOutputs[i], "hidden_states")) {
                hiddenIndex = i;
                break;
            }
        }

        const auto hiddenInfo = encOutputs[hiddenIndex].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> hiddenShape = hiddenInfo.GetShape();
        if (hiddenShape.size() != 3) {
            error = i18n::sf("编码器输出维度不对：%1（期望 3 维）",
                             "Unexpected encoder output shape: %1 (expected 3 dims)",
                             {shapeText(hiddenShape)});
            return false;
        }
        if (hiddenInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            error = i18n::sf("编码器输出不是 float32，而是 %1",
                             "Encoder output is not float32 but %1",
                             {QString::number(static_cast<int>(hiddenInfo.GetElementType()))});
            return false;
        }
        // 最后一维必须是 d_model，否则是拿错了输出（比如取到 pooler_output）。
        if (m.dModel > 0 && hiddenShape[2] != m.dModel) {
            error = i18n::sf("编码器输出最后一维是 %1，config 里的 d_model 是 %2",
                             "Encoder output last dim is %1 but config d_model is %2",
                             {QString::number(hiddenShape[2]), QString::number(m.dModel)});
            return false;
        }

        // 拷一份出来：解码期要按 beam 展开成 [B, src, d_model]，原张量随 encOutputs 一起释放。
        const float *hiddenRaw = encOutputs[hiddenIndex].GetTensorData<float>();
        std::vector<float> hiddenData(hiddenRaw,
                                      hiddenRaw + static_cast<size_t>(hiddenInfo.GetElementCount()));

        // ---- decoder：KV cache + beam search（无 cache 时退回贪心）----
        QVector<int> generated;
        const qint64 encMs = qEnvironmentVariableIsSet("STUDIOUS_MT_DEBUG") ? timer.elapsed() : 0;
        if (!m.generate(hiddenData, srcLen, generated, error)) {
            return false;
        }
        if (qEnvironmentVariableIsSet("STUDIOUS_MT_DEBUG")) {
            const qint64 decodeMs = timer.elapsed();
            qInfo().noquote()
                << QStringLiteral("[mt][debug] encoder=%1 ms decode=%2 ms tokens=%3 (%4 ms/token)")
                       .arg(encMs)
                       .arg(decodeMs)
                       .arg(generated.size())
                       .arg(generated.isEmpty()
                                ? 0.0
                                : static_cast<double>(decodeMs) / static_cast<double>(generated.size()),
                            0,
                            'f',
                            1);
        }

        if (qEnvironmentVariableIsSet("STUDIOUS_MT_DEBUG")) {
            QStringList ids;
            for (int i = 0; i < generated.size() && i < 24; ++i) {
                ids << QString::number(generated.at(i));
            }
            qInfo().noquote() << QStringLiteral("[mt][debug] generated %1 ids: %2")
                                     .arg(generated.size())
                                     .arg(ids.join(QLatin1Char(',')));
        }

        outText = cleanupTranslation(m.targetTok.decode(generated));
        if (outText.isEmpty()) {
            error = i18n::s("译文经目标分词器还原后为空", "Translation is empty after detokenization");
            return false;
        }
        return true;
    } catch (const Ort::Exception &ex) {
        error = i18n::sf("ONNX 推理失败：%1", "ONNX inference failed: %1", {QString::fromUtf8(ex.what())});
        return false;
    }
}

bool MtModel::isLoaded() const
{
    return m_impl->loaded;
}

bool MtModel::hasKvCache() const
{
    return m_impl->kvReady;
}

int MtModel::beamSize() const
{
    return m_impl->kvReady ? m_impl->numBeams : 1;
}

} // namespace studious::mt
