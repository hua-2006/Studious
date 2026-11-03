#pragma once

#include "contract/ITranslator.h"

namespace studious::translate {

// 本地神经机器翻译组件（v1 为桩实现）。
//
// 后续实现路线：
//   1. 推理后端：onnxruntime C API（跨平台预编译二进制最省心），与 llama 一样不引入平台头文件。
//   2. 模型形态：OPUS-MT 按语言对切分的小模型（导出为 ONNX，100~300MB/对），
//      用户选什么语言下什么语言；冷门语言对走 en 中转（A -> en -> B 两次推理）。
//   3. 资产约定：模型包 id 形如 "mt.opus.en-zh"，通过 asset/AssetRegistry 解析与校验，
//      缺失时由 resolve() 返回 missingAssets 供 UI 下载引导。
//   4. 语言检测：本地 MT 不自带检测，需外挂轻量检测器（fastText lid / CLD3）或由前端指定源语言。
//
// v1 只落地接口与不可用状态，保证 Router 的降级链先跑通。
class LocalMtTranslator final : public core::ITranslator {
public:
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
};

} // namespace studious::translate
