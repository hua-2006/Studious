#pragma once

#include "contract/ITranslator.h"

#include <QString>

#include <map>
#include <memory>

namespace studious::mt {
class MtModel;
}

namespace studious::translate {

// 本地神经机器翻译组件（OPUS-MT / Marian，ONNX Runtime CPU 推理）。
//
// 资产约定：<modelsRoot>/mt.opus.<src>-<dst>/ 下放
//   encoder_model_quantized.onnx / decoder_model_quantized.onnx
//   source.spm / target.spm / vocab.json / config.json
//
// 语言对策略：
//   1. 有直连对（如 en-zh、zh-en）就用直连，质量最好；
//   2. 没有直连但有 A-en 和 en-B → 走英文中转（两次推理）；
//   3. 都没有 → supports() 返回 false，Router 不会选中，UI 通过 resolve() 提示下载。
class LocalMtTranslator final : public core::ITranslator {
public:
    // modelsRoot 为空时用 %APPDATA%/studious/assets/models（与资产系统一致）。
    explicit LocalMtTranslator(const QString &modelsRoot = QString());
    ~LocalMtTranslator() override;

    QString id() const override;
    bool isReady() const override;
    core::ResolveResult resolve(const QString &src, const QString &dst) const override;
    QStringList supportedTargetLangs() const override;
    bool supports(const QString &src, const QString &dst) const override;
    core::TranslateOutcome translate(const QStringList &paragraphs,
                                     const QString &srcLang,
                                     const QStringList &dstLangs,
                                     const QJsonObject &options) override;
    void cancel() override;

    QString cacheTag() const override;

    // 当前已安装的语言对（"en-zh" 形式），供 UI 展示。
    QStringList installedPairs() const;

    // src→dst 是否要走英文中转（直连模型可用时返回 false）。
    // 只是把 translate() 内部的选择结果提前暴露出来，供 UI/自测展示。
    bool pivotsThroughEnglish(const QString &src, const QString &dst) const;

private:
    QString pairDir(const QString &src, const QString &dst) const;
    bool pairAvailable(const QString &src, const QString &dst) const;
    mt::MtModel *modelFor(const QString &src, const QString &dst);
    bool translateOne(const QString &text,
                      const QString &src,
                      const QString &dst,
                      QString &outText,
                      QString &error);

    QString m_modelsRoot;
    // 注意：不能用 QHash —— Qt 容器要求元素可拷贝，std::unique_ptr 不能拷贝（实测编译不过）。
    std::map<QString, std::unique_ptr<mt::MtModel>> m_models;   // key: "en-zh"
    bool m_cancelRequested = false;
};

} // namespace studious::translate
