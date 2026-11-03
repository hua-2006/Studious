#pragma once

#include <QString>
#include <QVector>

namespace studious::llm {

// 某个网关模型的能力描述，来自 GET {baseUrl}/models 的响应。
//
// 注意：标准 OpenAI /models 只保证 id / owned_by，其余字段是各家网关的扩展。
// 缺省字段一律按"不支持"处理，调用方不要假设能力存在。
struct LlmModelInfo {
    QString id;                     // 请求时 `model` 字段要用这个
    QString name;                   // 展示名（缺省回落 id）
    QString ownedBy;                // 供应商，仅展示用
    QString kind;                   // "chat" / "image" / ...（缺省视为 chat）
    bool    supportsReasoning = false;
    bool    supportsTools = false;
    int     maxInputTokens = 0;     // 0 = 网关未告知
    int     maxOutputTokens = 0;

    bool isChatModel() const { return kind.isEmpty() || kind.compare(QLatin1String("chat")) == 0; }
};

// GET {baseUrl}/models 的结果。ok=false 时 error 是中文可读原因。
struct ModelListResult {
    bool ok = false;
    QVector<LlmModelInfo> models;   // 已过滤：只保留聊天模型
    QString error;
};

// 一个可选的思考等级。
//
// 网关**不会**返回"支持哪些等级"，只给 supports_reasoning 布尔值，
// 所以等级列表由 reasoningOptionsFor() 推导，而不是从接口读。
struct ReasoningOption {
    QString value;              // 透传给 reasoning_effort；空串 = 不发送该参数
    QString label;              // 中文显示名
    bool    recommended = false;
};

// 按模型能力推导可选思考等级：
//   - 不支持推理 → 只返回一项「不适用」（value 为空，即不发送该参数）
//   - 支持推理   → 关闭 / 极低 / 低 / 中 / 高 / 跟随默认
QVector<ReasoningOption> reasoningOptionsFor(const LlmModelInfo &model);

// 在给定等级列表里找到 value 对应的下标；找不到返回 0（即第一项）。
int indexOfReasoningOption(const QVector<ReasoningOption> &options, const QString &value);

} // namespace studious::llm
