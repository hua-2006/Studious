#pragma once

#include "contract/ITranslator.h"

namespace studious::translate {

// LLM 翻译组件（v1 为桩实现）。
//
// 后续实现路线：
//   1. 形态：子进程 llama-server（llama.cpp），后端只通过 HTTP 与之通信，
//      因此模型 OOM / 驱动崩溃不会带走主进程，升级 llama.cpp 也只需换二进制。
//   2. 协议：OpenAI 兼容 /v1/chat/completions，一处实现覆盖三种后端——
//      内置 llama-server（127.0.0.1 + 随机端口 + 一次性 token）、用户已装 Ollama（:11434）、远程 API。
//   3. 输出约束：强制 JSON 输出 + 段落数校验 + 解析失败重试 1 次；仍失败则降级。
//   4. galgame 增强：上下文窗口（前 2~4 句原文+译文）、术语表注入、预翻译 + 缓存。
//   5. 资产约定：runtime.llama.vulkan.win-x64（运行时）、model.qwen2.5-3b.q4km（模型包），
//      由 asset/AssetRegistry 解析并验签，缺失时 resolve() 返回清单。
//
// v1 只落地接口与不可用状态，保证 Router 的降级链先跑通。
class LlmTranslator final : public core::ITranslator {
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
