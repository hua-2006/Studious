#pragma once

#include "config/LlmConfig.h"
#include "llm/LlmModelInfo.h"

namespace studious::net {
class IHttpClient;
}

namespace studious::llm {

// 拉取网关可用模型列表（GET {baseUrl}/models）。
//
// 用途：用户在设置里填好 Base URL + API Key 后点「获取模型」，
// 由后端去问网关，前端只负责把结果渲染成下拉框——前端不该自己拼接口。
//
// 失败时返回 ok=false 并给出中文原因（key 无效 / 404 路径写错 / 超时等）。
ModelListResult listModels(const config::LlmConfig &cfg, net::IHttpClient &http);

// 思考关闭能力的**真实探测**。
//
// 为什么需要：supports_reasoning 只是元数据，网关可能标错，也可能虽然支持推理
// 但接受 reasoning_effort 却不生效（实测 minimax 系就有这种情况）。
// 探测方式是真发一次极短请求，看返回里是否仍产生思考内容/token。
//
// 结论是三态：确认能关 / 仍然在思考 / 无法判定（网关没回 usage 明细）。
enum class ReasoningProbe {
    Disabled,      // 确认关闭成功（无 reasoning_content 且 reasoning_tokens == 0）
    StillThinking, // 参数无效，仍在思考
    Unknown,       // 网关未返回足够信息
};

struct ReasoningProbeResult {
    ReasoningProbe probe = ReasoningProbe::Unknown;
    int  reasoningTokens = 0;
    bool hasReasoningContent = false;
    bool ok = false;          // 请求本身是否成功
    QString error;            // 请求失败原因
    QString sample;           // 探测请求得到的译文（便于人工核对没被翻译坏）
};

ReasoningProbeResult probeReasoning(const config::LlmConfig &cfg, net::IHttpClient &http);

} // namespace studious::llm
