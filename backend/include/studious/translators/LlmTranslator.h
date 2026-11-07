#pragma once

#include <atomic>

#include "config/LlmConfig.h"
#include "contract/ITranslator.h"
#include "net/IHttpClient.h"

#include <QElapsedTimer>
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
    // 缓存键要带上模型与网关：换模型后译文会变，不能复用旧结果。
    QString cacheTag() const override;
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

    // 多段合并成一次请求（JSON 字符串数组输出）。
    // 解析失败内部会重试 1 次；仍失败返回 false，由调用方回退到逐段。
    bool translateBatch(const QStringList &texts,
                        const QString &dstLang,
                        const QString &endpoint,
                        QStringList &outTexts,
                        QString &outError);

    // 发一次 /chat/completions，返回已剥掉包装的 content。
    // translateOne 与 translateBatch 共用，避免两份 HTTP 代码各写各的。
    bool postChat(const QString &systemPrompt,
                  const QString &userPrompt,
                  double temperature,
                  int maxTokens,
                  QString &outContent,
                  QString &outError);

    // 唯一一份"配置 -> HTTP 请求"的构造逻辑：payload（model / temperature /
    // max_tokens / reasoning_effort / thinking）、URL、鉴权头全在这里。
    // postChat() 调它，所以单段与批量两条路径拿到的参数一定同源——
    // 之前批量路径自己硬编码 prompt / 温度 / maxTokens，用户的设置会静默失效。
    net::HttpRequest buildChatRequest(const QString &systemPrompt,
                                      const QString &userPrompt,
                                      double temperature,
                                      int maxTokens) const;

    // retryBudgetMs 是**整个 translate() 调用**共享的预算，不是每段各自重置。
    // 每段重新计时会让 N 段的实际耗时无上界（实测 2 段 × 20s 早已超 30s 还在跑）。
    // 注意：预算只在两次请求之间检查，已经发出去的请求无法被它中断
    // （同步 HTTP 实现没有外部打断能力），单次超时由 timeoutMs 负责。
    bool retryBudgetExhausted() const;

    // 从模型输出里抠出 JSON 字符串数组，并校验长度与内容非空。
    static bool parseJsonStringArray(const QString &content, int expected, QStringList &out);

    // 把 HTTP 层失败映射成中文原因。
    static QString describeHttpFailure(const net::HttpResponse &resp);

    // 剥掉模型偶发包裹的 ``` 代码块 / 成对引号。
    static QString stripWrapper(QString text);

    // 清理小模型的"注记噪声"：整行都是括注 / 语言标签 / 边界符号的行。
    // 只在 cfg.stripModelNotes == true 时调用（本地 0.5B 场景），云端行为不受影响。
    static QString stripAnnotationLines(QString text);

    config::LlmConfig m_cfg;
    net::IHttpClient *m_http = nullptr;   // 不持有，生命周期由调用方保证
    std::atomic_bool m_cancelRequested{false};

    // 本次 translate() 的时间基准，retryBudgetExhausted() 用它取 elapsed()。
    // 每次 translate() 开头重置一次，整次调用的所有段 / 所有目标语言共用一个 deadline。
    QElapsedTimer m_retryClock;

    // 本次 translate() 的源语言（"auto" 或内部码）。
    // 用它给 prompt 补上「把英文翻译成中文」这类明确指示——
    // 小模型（0.5B 级）在缺省源语言时经常直接照抄原文。
    QString m_activeSrcLang;
};

} // namespace studious::translate
