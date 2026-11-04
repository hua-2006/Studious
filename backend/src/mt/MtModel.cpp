#include "mt/MtModel.h"

#include "i18n/L10n.h"
#include "mt/MtTokenizer.h"

#include <onnxruntime_cxx_api.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <cmath>
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

std::unique_ptr<Ort::Session> createSession(Ort::Env &env, const QString &onnxPath, int threads)
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(threads);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

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

} // namespace

struct MtModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "studious-mt"};
    std::unique_ptr<Ort::Session> encoder;
    std::unique_ptr<Ort::Session> decoder;

    MtTokenizer sourceTok;
    MtTokenizer targetTok;

    // 张量名一律从模型里读出来存着，不硬编码猜：量化导出脚本改个名字就崩的坑不踩。
    std::vector<std::string> encInputs;
    std::vector<std::string> encOutputs;
    std::vector<std::string> decInputs;
    std::vector<std::string> decOutputs;
    std::vector<const char *> encInputPtrs;
    std::vector<const char *> encOutputPtrs;
    std::vector<const char *> decInputPtrs;
    std::vector<const char *> decOutputPtrs;

    // config.json 里的生成参数。
    int decoderStartId = 65000;
    int eosId = 0;
    int padId = 65000;
    int maxLength = 512;
    int dModel = 512;

    bool loaded = false;

    bool readConfig(const QString &configPath, QString *error);
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

    // 模型词表：调用方给了就用给的，没给就找 config.json 同目录的 vocab.json。
    QString vocabPath = paths.vocabJson;
    if (vocabPath.isEmpty()) {
        const QString sibling =
            QFileInfo(paths.configJson).absoluteDir().filePath(QStringLiteral("vocab.json"));
        if (QFileInfo::exists(sibling)) {
            vocabPath = sibling;
        }
    }

    const int threads = std::clamp(QThread::idealThreadCount() > 0 ? QThread::idealThreadCount() : 1,
                                   1,
                                   kMaxIntraThreads);
    try {
        m.encoder = createSession(m.env, paths.encoderOnnx, threads);
        m.decoder = createSession(m.env, paths.decoderOnnx, threads);
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

        // 名字和 shape 一起打出来：ONNX 导出脚本换一版就差一个 "_quantized" 后缀，
        // 真出问题时有这行日志能省半小时。
        const auto dump = [](const char *tag, Ort::Session &session, const std::vector<std::string> &names, bool wantInput) {
            for (size_t i = 0; i < names.size(); ++i) {
                qInfo().noquote() << QStringLiteral("[mt] %1 %2[%3] %4 %5")
                                         .arg(QString::fromLatin1(tag),
                                              wantInput ? QStringLiteral("in") : QStringLiteral("out"))
                                         .arg(i)
                                         .arg(QString::fromStdString(names[i]),
                                              shapeText(ioShape(session, wantInput, i)));
            }
        };
        dump("encoder", *m.encoder, m.encInputs, true);
        dump("encoder", *m.encoder, m.encOutputs, false);
        dump("decoder", *m.decoder, m.decInputs, true);
        dump("decoder", *m.decoder, m.decOutputs, false);
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

    m.loaded = true;
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

        // 拷一份出来给每步 decoder 复用：60MB 级的 decoder 每步只跑一次，这点拷贝不值一提。
        const float *hiddenRaw = encOutputs[hiddenIndex].GetTensorData<float>();
        std::vector<float> hiddenData(hiddenRaw,
                                      hiddenRaw + static_cast<size_t>(hiddenInfo.GetElementCount()));

        // ---- decoder：贪心解码。首步 [decoder_start_token_id]，之后每步补 argmax。----
        std::vector<int64_t> decIds{static_cast<int64_t>(m.decoderStartId)};
        QVector<int> generated;

        const int stepCap = std::min(m.maxLength, static_cast<int>(srcLen) * 2 + 32);

        for (int step = 0; step < stepCap; ++step) {
            std::vector<int64_t> decShape{1, static_cast<int64_t>(decIds.size())};
            std::vector<Ort::Value> decValues;
            decValues.reserve(m.decInputs.size());

            for (const std::string &name : m.decInputs) {
                // 顺序要紧：encoder_attention_mask 里也含 "attention_mask"，
                // 必须先把 encoder_hidden_states / encoder_attention_mask 摘出去。
                if (nameContains(name, "encoder_hidden_states")) {
                    decValues.push_back(floatTensor(memory, hiddenData, hiddenShape));
                } else if (nameContains(name, "encoder_attention_mask")) {
                    decValues.push_back(int64Tensor(memory, srcMask, maskShape));
                } else if (nameContains(name, "attention_mask")) {
                    decValues.push_back(int64Tensor(memory, srcMask, maskShape));
                } else if (nameContains(name, "input_ids")) {
                    decValues.push_back(int64Tensor(memory, decIds, decShape));
                } else if (nameContains(name, "past_key_values")) {
                    error = i18n::s("加载的是带 KV cache 的 decoder，Sprint 1 只支持不带 cache 的版本",
                                    "This is a KV-cache decoder; Sprint 1 only supports the non-cached model");
                    return false;
                } else {
                    error = i18n::sf("无法识别的解码器输入张量：%1",
                                     "Unrecognized decoder input tensor: %1",
                                     {QString::fromStdString(name)});
                    return false;
                }
            }

            std::vector<Ort::Value> decOutputs =
                m.decoder->Run(Ort::RunOptions{nullptr},
                               m.decInputPtrs.data(),
                               decValues.data(),
                               decValues.size(),
                               m.decOutputPtrs.data(),
                               m.decOutputPtrs.size());
            if (decOutputs.empty()) {
                error = i18n::s("解码器没有输出张量", "Decoder produced no output tensors");
                return false;
            }

            size_t logitsIndex = 0;
            for (size_t i = 0; i < m.decOutputs.size(); ++i) {
                if (nameContains(m.decOutputs[i], "logits")) {
                    logitsIndex = i;
                    break;
                }
            }

            const auto logitsInfo = decOutputs[logitsIndex].GetTensorTypeAndShapeInfo();
            const std::vector<int64_t> logitsShape = logitsInfo.GetShape();
            if (logitsShape.size() != 3) {
                error = i18n::sf("解码器输出维度不对：%1（期望 3 维 [1, seq, vocab]）",
                                 "Unexpected decoder output shape: %1 (expected 3 dims [1, seq, vocab])",
                                 {shapeText(logitsShape)});
                return false;
            }
            if (logitsInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                error = i18n::sf("解码器输出不是 float32，而是 %1",
                                 "Decoder output is not float32 but %1",
                                 {QString::number(static_cast<int>(logitsInfo.GetElementType()))});
                return false;
            }

            const int64_t seqLen = logitsShape[1];
            const int64_t vocab = logitsShape[2];
            if (seqLen != static_cast<int64_t>(decIds.size()) || vocab <= 0) {
                error = i18n::sf("解码器输出 shape 与输入不匹配：输入长度 %1，输出 %2",
                                 "Decoder output shape does not match its input: input length %1, output %2",
                                 {QString::number(decIds.size()), shapeText(logitsShape)});
                return false;
            }

            const float *logits = decOutputs[logitsIndex].GetTensorData<float>();
            const float *lastRow = logits + static_cast<size_t>(seqLen - 1) * static_cast<size_t>(vocab);

            // STUDIOUS_MT_DEBUG=1 时把首步 top-5 打出来：id 空间对不对，一眼能看出来。
            if (step == 0 && qEnvironmentVariableIsSet("STUDIOUS_MT_DEBUG")) {
                std::vector<int64_t> top(vocab);
                for (int64_t v = 0; v < vocab; ++v) {
                    top[static_cast<size_t>(v)] = v;
                }
                std::partial_sort(top.begin(), top.begin() + 5, top.end(), [lastRow](int64_t a, int64_t b) {
                    return lastRow[a] > lastRow[b];
                });
                QStringList line;
                for (int i = 0; i < 5; ++i) {
                    const int64_t id = top[static_cast<size_t>(i)];
                    line << QStringLiteral("%1=%2(%3)").arg(id).arg(lastRow[id], 0, 'f', 3).arg(QString::fromStdString(m.targetTok.pieceOf(static_cast<int>(id))));
                }
                qInfo().noquote() << "[mt][debug] step0 top5:" << line.join(QStringLiteral("  "));
            }

            int bestId = -1;
            float bestScore = -std::numeric_limits<float>::infinity();
            for (int64_t v = 0; v < vocab; ++v) {
                // pad 与 decoder_start 同 id（65000），config 的 bad_words_ids 也禁它，这里同样排除。
                if (v == m.padId) {
                    continue;
                }
                const float score = lastRow[v];
                if (std::isfinite(score) && score > bestScore) {
                    bestScore = score;
                    bestId = static_cast<int>(v);
                }
            }

            if (bestId < 0) {
                error = i18n::s("解码取 argmax 失败（logits 全是无效值）",
                                "Decode failed: argmax over logits produced nothing");
                return false;
            }
            if (bestId == m.eosId) {
                break;
            }

            generated.append(bestId);
            decIds.push_back(bestId);
        }

        if (generated.isEmpty()) {
            error = i18n::s("解码未产生任何 token（首个预测就是 eos）",
                            "Decoding produced no tokens (first prediction was eos)");
            return false;
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

} // namespace studious::mt
