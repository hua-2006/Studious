#pragma once

// 敏感设置的落盘形态。
//
// 为什么需要它：API Key 之前是明文写进注册表的（`settings/llm/apiKey`），
// 任何能读 HKCU 的进程都能直接拿走。这里把它变成一段只有本机当前用户能解开的密文。
//
// Windows 用 DPAPI（CryptProtectData）：密钥由系统按「用户 + 机器」派生，
// 换个账户或换台机器解不开 —— 正好符合"本机配置"的语义。
// 其它平台退化成 base64：只防肉眼误读，不防有心人；调用方要在文档里说清楚。
//
// 铁律：平台头只进 .cpp，本文件只有 Qt 类型。

#include <QString>

namespace studious::platform::secret {

// 明文 → 可落盘形态（带前缀，见 isProtected）。
// 空串原样返回空串：不存在"加密后的空值"这种状态。
QString protect(const QString &plain);

// 可落盘形态 → 明文。
// 兼容旧版本写进去的明文（没有前缀 → 原样返回），所以老用户升级后不会丢 Key。
// 密文解不开（换了账户 / 注册表被拷到别的机器）时返回空串，让调用方按"没配 Key"处理。
QString unprotect(const QString &stored);

// 是不是密文（`dpapi:` 或 `b64:` 前缀）。
bool isProtected(const QString &stored);

}   // namespace studious::platform::secret
