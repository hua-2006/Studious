#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace studious::asset {

// 清单信封的 schema 标识，必须与文件里的顶层 "schema" 完全一致。
inline constexpr const char *kManifestSchema = "studious-manifest/1";

// 资产清单里的一条资产。
//
// 约定：所有下载地址必须是 https，sha256 是"下载完成后"的整包（或解压前归档）哈希。
// 无 urls 的条目是**占位**（例如还没发布的 Linux 版），不参与安装，只用于 UI 展示与提示。
struct AssetEntry {
    QString id;            // "runtime.llama.vulkan.win-x64"
    QString type;          // "runtime" | "model"
    QString op;            // "windows" | "linux" | ""
    QString arch;          // "x64" | ""
    QString backend;       // "vulkan" | "cpu" | ""
    QString version;
    QString family;        // 模型家族，如 "qwen2.5"
    QString quant;         // 量化，如 "Q4_K_M"
    QStringList pair;      // 本地 MT 语言对，如 {"en","zh"}
    qint64  size = 0;      // 字节
    QString sha256;        // 64 位小写 hex；无 urls 时可为空（占位）
    QString sig;           // 可选：runtime 包的独立 ed25519 签名
    QString archive;       // "zip" | ""
    QStringList urls;      // 多源，按优先级
    QStringList files;     // 解压后关键文件（可选）
    int requiresRamGB = 0;
    int requiresVramGB = 0;
    QString note;

    bool isRuntime() const;
    bool isModel() const;
    bool isPlaceholder() const;   // urls 为空 = 仅占位（如 Linux 版）
};

// 一份通过校验的清单（payload 部分）。
struct Manifest {
    int schemaVersion = 0;
    QString generatedAt;
    QString channel;              // "stable" | "beta"
    QString keyId;
    QVector<AssetEntry> assets;

    // 按 id 精确查找；找不到返回 nullptr（返回内部指针，生命周期同本 Manifest）。
    const AssetEntry *find(const QString &id) const;

    // 按 type 过滤（"runtime" / "model"）。顺序与清单一致。
    QVector<AssetEntry> ofType(const QString &type) const;

    // 按平台过滤：条目 op 非空时必须等于 op；条目 arch 非空时必须等于 arch。
    // 也就是说 op/arch 留空的条目（例如纯模型）对任何平台都算可选项。
    QVector<AssetEntry> forPlatform(const QString &op, const QString &arch) const;
};

struct ManifestVerifyOptions {
    QByteArray trustedPublicKey;   // 32 字节；为空表示不验签（仅开发/自测）
    bool requireSignature = true;
    int  maxSchemaVersion = 1;

    // 仅用于解析仓库里的示例清单（backend/asset/manifest.json）：那里 sha256 是"待发布时填入"
    // 的空串占位。默认 false = 严格（有 urls 就必须有 64 hex sha256）。
    // 任何发布/更新路径都必须保持 false。
    bool allowMissingSha256 = false;
};

struct ManifestLoadResult {
    bool ok = false;
    Manifest manifest;
    QString error;      // 中文可读原因
};

// 解析 + 验签 + 结构校验。envelope 格式见下方说明。
ManifestLoadResult parseManifest(const QByteArray &rawJson, const ManifestVerifyOptions &opts);

// 工具函数
QString sha256File(const QString &path, QString *error = nullptr);   // 分块计算，大文件友好
bool    verifyFileSha256(const QString &path, const QString &expectedHex, QString *error = nullptr);
QByteArray hexToBytes(const QString &hex);
QString     bytesToHex(const QByteArray &bytes);

} // namespace studious::asset

// ---------------------------------------------------------------------------
// 清单信封格式（parseManifest 的输入，必须严格按此实现）：
//
//   {
//     "schema": "studious-manifest/1",
//     "keyId": "studious-dev",
//     "payload": {
//       "schemaVersion": 1,
//       "generatedAt": "2026-11-04T00:00:00Z",
//       "channel": "stable",
//       "assets": [ /* AssetEntry[] */ ]
//     },
//     "sig": "<128 位 hex，对 payload 的签名>"
//   }
//
// 签名覆盖范围 = QJsonDocument(payload).toJson(QJsonDocument::Compact) 的字节。
// QJsonObject 内部按 key 排序，序列化确定性可保证；签名工具（backend/tools/manifest_tool.cpp）
// 必须用同一方式生成，否则验签必失败。
//
// parseManifest 的校验项（每一条失败都给中文 error）：
//   1. 顶层 JSON 合法、payload 是 object、sig 若存在必须是 128 位 hex
//   2. requireSignature 为真时 sig 必填，且用 trustedPublicKey（32 字节）验签
//   3. schema 必须是 "studious-manifest/1"
//   4. payload.schemaVersion 必须 > 0 且 <= maxSchemaVersion（超限提示"客户端版本过旧"）
//   5. 每条资产：id 非空且全局唯一；type 必须是 runtime / model
//   6. 有 urls 的资产，sha256 必须是 64 位小写 hex；无 urls 视为占位，sha256 可为空
//   7. urls 里出现非 https:// 开头的一律拒绝（不接受明文 http）
//   8. size 不得为负
//
// 注：仓库里的 backend/asset/manifest.json 是**给人看的示例清单**，未签名，且 sha256 是发布前
// 才填的占位空串 —— 解析它时必须 opts.requireSignature = false，并且要让"有 urls 但 sha256 为空"
// 这一条通过，因此在 ManifestVerifyOptions 后面追加了 allowMissingSha256（默认 false，保持严格）。
// 发布校验路径永远不要打开它。
// ---------------------------------------------------------------------------
