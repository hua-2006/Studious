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
    QString decoderOnnx;    // decoder_model_quantized.onnx（不带 KV cache，fallback 用）
    // decoder_model_merged_quantized.onnx（带 KV cache）。留空时自动找 decoderOnnx 同目录的
    // decoder_model_merged_quantized.onnx；没有或加载失败就退回 decoderOnnx（贪心 + 每步重算前缀）。
    QString decoderMergedOnnx;
    QString sourceSpm;
    QString targetSpm;
    QString configJson;     // decoder_start_token_id / eos_token_id / max_length / num_beams
    // 合并词表（模型 id 空间）。留空时自动找 configJson 同目录的 vocab.json；
    // 找不到就退回 spm 自己的 id 空间（只在 spm 本身就是模型词表时才正确）。
    QString vocabJson;
};

// 本地神经机器翻译（OPUS-MT / Marian，ONNX Runtime CPU 推理）。
//
// Sprint 2：
//   * KV cache —— 用 decoder_model_merged_quantized.onnx，首步喂 seq_len=0 的空 past，
//     之后只喂新生成的 1 个 token + 上一步的 present.*；加载失败自动退回非 cache 模型。
//   * beam search —— beam 数取 config.json 的 num_beams（默认 4），可用环境变量
//     STUDIOUS_MT_BEAMS=n 覆盖（n=1 = 贪心，结果与 Sprint 1 逐 token 一致）。
//   * 非 cache 回退路径只支持贪心（num_beams<=1）：没有 past 时每步要重算整段前缀，
//     多 beam 只会线性放大开销、收益不抵成本，故不实现。
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
    //
    // targetLangToken：多语言模型（mt.opus.en-mul）的目标语言 token，形如 "zho_Hans"
    // （带不带 >> << 都认）。非空时把这个 ">>token<<" 的 id **手工拼到 encoder
    // input_ids 最前面**（spm 的源语言模型里根本没有这个 piece，交给 encode() 只会
    // 变成 unk）；attention_mask 与 input_ids 等长，所以同步多一个 1。
    // 单对各模型传默认空串即可，行为与改动前完全一致。
    bool translate(const QString &text, QString &outText, QString &error,
                   const QString &targetLangToken = QString());

    bool isLoaded() const;

    // 诊断用：实际生效的解码配置（自测/日志展示，不参与逻辑）。
    bool hasKvCache() const;
    int beamSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace studious::mt
