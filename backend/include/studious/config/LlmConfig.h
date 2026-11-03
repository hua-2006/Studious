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

    bool isValid() const { return !baseUrl.trimmed().isEmpty() && !model.trimmed().isEmpty(); }
};

} // namespace studious::config
