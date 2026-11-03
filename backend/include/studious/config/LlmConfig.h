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

    // 批量合并：一次 HTTP 请求最多翻多少段。<= 1 表示退回"逐段请求"。
    //
    // 为什么默认 8：实测本机网关的延迟几乎全部来自**每次请求固定的上游首字延迟**
    // （约 1.1s），与输入/输出 token 数几乎无关：
    //   输入 32 token / 输出 4 token   → 1031~1357 ms
    //   输入 69 token / 输出 5 token   →  991~1431 ms   ← token 变多，耗时没变
    //   流式 TTFT                      → 1278~1638 ms   ← 流式也救不了
    // 因此 N 段逐段请求 = N × 1.1s；合并成 1 次 = 1.1s。
    // 实测 5 段：逐段 5662 ms → 合并 1034 ms（5.5 倍）。
    //
    // 代价：批量返回需要 JSON 数组校验，模型可能漏条/加前言，故失败会整块回退到逐段。
    int batchSize = 8;

    bool isValid() const { return !baseUrl.trimmed().isEmpty() && !model.trimmed().isEmpty(); }
};

} // namespace studious::config
