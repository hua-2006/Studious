#pragma once

#include <QString>

#include <memory>

namespace studious::mt {

// 一组 OPUS-MT 语言对资产的路径。
//
// 约定来自资产包 mt.opus.en-zh：两个 ONNX（encoder / decoder）+ 两个 spm + config.json。
// 路径由调用方（AssetRegistry / 自测）解析，MtModel 自己不去猜目录。
struct MtPaths {
    QString encoderOnnx;    // encoder_model_quantized.onnx
    QString decoderOnnx;    // decoder_model_quantized.onnx（MVP 用不带 KV cache 的那份）
    QString sourceSpm;
    QString targetSpm;
    QString configJson;     // decoder_start_token_id / eos_token_id / max_length
    // 合并词表（模型 id 空间）。留空时自动找 configJson 同目录的 vocab.json；
    // 找不到就退回 spm 自己的 id 空间（只在 spm 本身就是模型词表时才正确）。
    QString vocabJson;
};

// 本地神经机器翻译（OPUS-MT / Marian，ONNX Runtime CPU 推理）。
//
// Sprint 1 范围：单语言对、单句、贪心解码。没有 beam search（config 里 num_beams=4 先无视），
// 没有 KV cache（每步重算整段前缀），没有批处理。够跑通闭环，性能优化留给 Sprint 2。
class MtModel {
public:
    MtModel();
    ~MtModel();

    MtModel(const MtModel &) = delete;
    MtModel &operator=(const MtModel &) = delete;

    // 加载两个 ONNX 会话 + 两个分词器 + config。失败时 error 写中文文案（走 i18n）。
    bool load(const MtPaths &paths, QString *error = nullptr);

    // 单句翻译。text 是源语言（当前只有 en）一句，outText 是译文。
    // 失败返回 false 并写 error —— 错误不吞，调用方自己决定怎么显示。
    bool translate(const QString &text, QString &outText, QString &error);

    bool isLoaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace studious::mt
