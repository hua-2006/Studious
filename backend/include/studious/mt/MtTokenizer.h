#pragma once

#include <QString>
#include <QVector>

#include <memory>
#include <string>

namespace studious::mt {

// SentencePiece 分词器封装。
//
// OPUS-MT 的语言对包里带两个 spm：source.spm（编码侧）与 target.spm（解码侧）。
// 两边 vocab 数量、特殊符号 id 都可能不同，所以这里一个实例只绑定一个 spm 文件，
// 谁用谁各持一份，别想着"共用一个也能跑"。
//
// **模型 id 空间与 spm id 空间不是一回事**：Marian 的模型词表是 vocab.json 里的合并词表
// （本包 65001 项，0=</s>、1=<unk>、65000=<pad>），而单个 spm 只有 32000 项、</s> 在 id 2。
// 所以加载 vocab.json 时，encode() 走"spm 切词 -> vocab.json 映射到模型 id"，
// decode() 走"模型 id -> vocab.json 取 piece -> target spm 反分词"。
//
// 线程安全：Encode/Decode 都是 const，多线程并发调用安全；load() 不是。
class MtTokenizer {
public:
    MtTokenizer();
    ~MtTokenizer();

    MtTokenizer(const MtTokenizer &) = delete;
    MtTokenizer &operator=(const MtTokenizer &) = delete;

    // 只加载 .spm：encode()/decode() 直接用 spm 自己的 id 空间（0..31999）。
    bool load(const QString &spmPath, QString *error = nullptr);

    // .spm + vocab.json：按模型词表映射 id（见上面的说明）。vocabJsonPath 为空时等价于上一个重载。
    bool load(const QString &spmPath, const QString &vocabJsonPath, QString *error = nullptr);

    // 编码成模型 id 序列。**不自动追加 </s>**：加不加、加哪个 id 由调用方决定。
    QVector<int> encode(const QString &text) const;

    // 模型 id -> 文本。跳过控制符（<unk>/<s>/</s>/<pad>）和越界 id，不抛、不崩。
    QString decode(const QVector<int> &ids) const;

    // spm 切出来的**原始 piece**（还没映射到模型词表）。
    // 排查用：某个 piece 在 vocab.json 里不存在时，encode() 会把它落成 <unk>，
    // 只有看到原始 piece 才知道是"spm 切碎了"还是"词表对不上"。
    QStringList encodePieces(const QString &text) const;

    bool isLoaded() const;

    // eos 的 id 由词表自己说了算：加载了 vocab.json 就是 0，只有 spm 时是 spm 的 eos（2）。
    int eosId() const;
    int vocabSize() const;

    // id -> piece 原文，仅用于排查（越界返回空串）。
    std::string pieceOf(int id) const;

    // piece -> id。直接查 vocab.json（模型词表），用于 ">>zho_Hans<<" 这类
    // **不在 spm 里**的特殊 token：它们切不出来，只能按 piece 名换 id 再手工拼进 input_ids。
    // 找不到（含未加载 vocab.json）返回 -1。
    int pieceToId(const QString &piece) const;

private:
    bool loadImpl(const QString &spmPath, const QString &vocabJsonPath, QString *error);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace studious::mt
