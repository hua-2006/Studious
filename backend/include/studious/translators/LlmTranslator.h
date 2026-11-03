#pragma once

#include "config/LlmConfig.h"
#include "contract/ITranslator.h"
#include "net/IHttpClient.h"

#include <QString>

namespace studious::translate {

// LLM 翻译组件：直接对接 OpenAI 兼容的 /v1/chat/completions（云端网关 / Ollama / 商业 API 皆可）。
//
// 设计要点（v1 已落地，后续增强见文件末尾）：
//   1. 逐段落请求（方案 A）：每段一次 HTTP，失败可精确定位到段落，不必为批量格式做容错。
//      代价是 N 段 = N 次往返，后续可用 JSON 批量 + 段落数校验来合并。
//   2. 只取 choices[0].message.content：推理模型还会回 reasoning_content（思考过程），
//      那是给调试看的，绝不能当译文输出。
//   3. 强约束 system prompt + <<< >>> 边界，避免模型把原文当问题回答（实测弱提示会变闲聊）。
//   4. 错误一律翻译成中文可读原因，前端直接展示，不需要认识 HTTP 状态码。
//
// 后续增强路线：
//   - 批量：多段合并成一次请求（JSON 数组 + 段落数校验 + 解析失败重试 1 次）。
//   - 上下文：galgame 场景注入前 2~4 句原文/译文，改善对话连贯性。
//   - 术语表：注入专有名词表，保证人名/技能名一致。
//   - 缓存：原文 hash -> 译文，命中即免请求。
class LlmTranslator final : public core::ITranslator {
public:
    // 空配置版本：isReady() == false，供 Router 的降级链探测用。
    LlmTranslator();

    LlmTranslator(config::LlmConfig cfg, net::IHttpClient &http);

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

private:
    // 拼 baseUrl + /chat/completions（容忍 baseUrl 末尾带斜杠）。
    QString chatCompletionsUrl() const;

    // system prompt：用户填了就用用户的，空则用内置默认。
    QString effectiveSystemPrompt() const;

    // 拼 user 消息：用 <<< >>> 标出原文边界，避免模型把原文当提问。
    QString buildUserPrompt(const QString &text, const QString &dstLang) const;

    // 单段翻译。失败返回 false 并写 outError（中文可读原因）。
    bool translateOne(const QString &text,
                      const QString &dstLang,
                      const QString &endpoint,
                      QString &outText,
                      QString &outError);

    // 把 HTTP 层失败映射成中文原因。
    static QString describeHttpFailure(const net::HttpResponse &resp);

    // 剥掉模型偶发包裹的 ``` 代码块 / 成对引号。
    static QString stripWrapper(QString text);

    config::LlmConfig m_cfg;
    net::IHttpClient *m_http = nullptr;   // 不持有，生命周期由调用方保证
    bool m_cancelRequested = false;
};

} // namespace studious::translate
