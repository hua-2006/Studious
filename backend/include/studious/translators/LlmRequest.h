#pragma once

#include "config/LlmConfig.h"
#include "net/IHttpClient.h"

#include <QJsonArray>
#include <QString>
#include <QUrl>

namespace studious::translate {

// OpenAI 兼容网关的 HTTP 请求构造，唯一一份实现。
//
// 以前两处各拼一套：LlmTranslator::buildChatRequest()（翻译用）与
// llm::probeReasoning()（"思考能不能关掉"探测用）。endpoint 拼法、Bearer 头、
// payload 字段名一旦漂移，就会出现"翻译能通、探测 401"这类难查的问题。

// baseUrl 末尾斜杠容忍后拼上 path：apiUrl("http://x/v1/", "/models")
//   -> "http://x/v1/models"。GET /models 这类非 chat 请求也走这里。
QUrl apiUrl(const QString &baseUrl, const QString &path);

// baseUrl -> chat completions 的完整地址。除了末尾斜杠，还兜住用户直接填
// "…/v1/chat/completions" 的情况（否则会重复拼一层）。
QString chatCompletionsUrl(const QString &baseUrl);

// apiKey 非空时追加一行 "Authorization: Bearer <key>"。
// 云端 / 本地 llama-server（无 key）都靠这一处判断，别在各处重写。
void appendAuthorization(net::HttpRequest &req, const QString &apiKey);

// 首字节等待上限：10 秒。整段生成慢是正常的（长文几十秒），但"一个字节都不回"
// 基本就是地址 / 端口 / 网关不对，让它等满 60 秒用户只会以为软件卡死了。
inline constexpr int kChatFirstByteTimeoutMs = 10000;

// 两处**有意不同**的参数。默认值 = 翻译路径的行为。
struct ChatRequestOverrides {
    // 0 = 用 cfg.timeoutMs（非法值退回 60s）。
    int timeoutMs = 0;
    // 首字节上限，默认就是 kChatFirstByteTimeoutMs；0 = 不设首字节超时。
    // 只有"思考能不能关掉"那种探测会传 0：它本来就只发一句短 prompt，
    // 让它和普通请求一样有 10 秒门槛没有意义。
    int firstByteTimeoutMs = kChatFirstByteTimeoutMs;
    // 空（QString()，即未设置）= 用 cfg.reasoningEffort（trim + 小写）后的值。
    // 显式给空串 = 请求里完全不发思考相关字段（跟随服务端默认）。
    QString reasoningEffort;
};

// 配置 + messages -> 一次 /chat/completions 请求（POST / JSON body / 鉴权 / 超时）。
//
// maxTokens 与 temperature 由调用方决定：单段、批量、探测三种场景的估算口径不同，
// 但"字段名怎么写、超时怎么给、body 怎么序列化"必须同源。
net::HttpRequest makeChatRequest(const config::LlmConfig &cfg,
                                 const QJsonArray &messages,
                                 int maxTokens,
                                 double temperature,
                                 const ChatRequestOverrides &overrides = {});

// 把 HTTP 层失败翻译成一句人话（401 / 402 / 403 / 404 / 429 / 5xx / 连不上）。
// 唯一一份文案表：翻译、模型列表、推理探测从此说的是同一套话。
QString describeHttpFailure(const net::HttpResponse &resp);

} // namespace studious::translate
