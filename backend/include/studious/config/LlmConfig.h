#pragma once

#include <QString>

namespace studious::config {

// 云端 LLM（OpenAI 兼容 /v1/chat/completions）的连接与采样参数。
// 一份配置覆盖三种后端：内置网关（127.0.0.1:xxxx）、Ollama（:11434）、远程商业 API。
struct LlmConfig {
    QString baseUrl;              // 例如 "http://127.0.0.1:3065/v1"
    QString apiKey;
    QString model;                // 例如 "deepseek-v4.1-flash"
    int     timeoutMs   = 60000;
    int     maxTokens   = 2048;
    double  temperature = 0.2;
    QString systemPrompt;         // 空则用内置默认

    // 思考等级（推理模型专用）。翻译是确定性任务，开思考只增加延迟不提升质量。
    // 取值："none"（默认，彻底关闭）/ "minimal" / "low" / "medium" / "high"，空串 = 不传该参数（跟随服务端默认）。
    //
    // 本机网关实测（deepseek-v4.1-flash）：
    //   不传           → 2.0s，reasoning_tokens ≈ 81
    //   "minimal"      → 1.5s，reasoning_tokens ≈ 73（无效，思考照旧）
    //   "none"         → 1.1s，reasoning_tokens = 0  ← 生效
    //   enable_thinking=false 字段 → 无效（仍产生思考 token）
    // 另外部分网关认 Anthropic 风格，故 reasoningEffort == "none" 时会一并发送
    // thinking: {type: "disabled"}（两者同时发本机网关实测不报错）。
    QString reasoningEffort = QStringLiteral("none");

    bool isValid() const { return !baseUrl.trimmed().isEmpty() && !model.trimmed().isEmpty(); }
};

} // namespace studious::config
