// 资产清单（manifest）解析 + ed25519 验签 + 结构校验。
//
// 设计原则：
//   * 任何一条不合规都返回 ok=false + 中文 error，绝不"尽力加载半份清单"；
//   * 验签与结构校验分离：签名覆盖的是 payload 的 Compact 序列化字节，
//     所以顺序固定为 解析 → 验签 → 结构校验（篡改 payload 的用例必须报"签名"而不是别的错）；
//   * 不依赖任何平台头文件，只用 Qt Core + Monocypher。

#include <studious/asset/Manifest.h>

#include "i18n/L10n.h"
#include "monocypher-ed25519.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace studious::asset {
namespace {

ManifestLoadResult failResult(const QString &why)
{
    ManifestLoadResult r;
    r.ok = false;
    r.error = why;
    return r;
}

// hex 长度与字符集检查。lowerOnly=true 时只接受小写 a-f（sha256 的要求）。
bool isHexOfLength(const QString &s, int length, bool lowerOnly)
{
    if (s.size() != length) {
        return false;
    }
    for (const QChar &c : s) {
        const ushort u = c.unicode();
        const bool isDigit = (u >= '0' && u <= '9');
        const bool isLower = (u >= 'a' && u <= 'f');
        const bool isUpper = (u >= 'A' && u <= 'F');
        if (isDigit || isLower) {
            continue;
        }
        if (!lowerOnly && isUpper) {
            continue;
        }
        return false;
    }
    return true;
}

QStringList stringList(const QJsonValue &value)
{
    QStringList out;
    if (!value.isArray()) {
        return out;
    }
    const QJsonArray arr = value.toArray();
    out.reserve(arr.size());
    for (const QJsonValue &item : arr) {
        if (item.isString()) {
            out << item.toString();
        }
    }
    return out;
}

// 单条资产解析。失败时写 *error 并返回 false。
bool parseEntry(const QJsonObject &obj, AssetEntry *entry, QString *error)
{
    entry->id = obj.value(QStringLiteral("id")).toString();
    if (entry->id.isEmpty()) {
        *error = i18n::s("资产缺少 id（id 不能为空）",
                         "The asset is missing its id (id must not be empty)");
        return false;
    }

    entry->type = obj.value(QStringLiteral("type")).toString();
    if (entry->type != QLatin1String("runtime") && entry->type != QLatin1String("model")) {
        *error = i18n::sf("资产 %1 的 type 非法：\"%2\"（只允许 runtime / model）",
                          "Asset %1 has an invalid type: \"%2\" (only runtime / model are allowed)",
                          {entry->id, entry->type});
        return false;
    }

    entry->op = obj.value(QStringLiteral("op")).toString();
    entry->arch = obj.value(QStringLiteral("arch")).toString();
    entry->backend = obj.value(QStringLiteral("backend")).toString();
    entry->version = obj.value(QStringLiteral("version")).toString();
    entry->family = obj.value(QStringLiteral("family")).toString();
    entry->quant = obj.value(QStringLiteral("quant")).toString();
    entry->pair = stringList(obj.value(QStringLiteral("pair")));
    entry->sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed();
    entry->sig = obj.value(QStringLiteral("sig")).toString().trimmed();
    entry->archive = obj.value(QStringLiteral("archive")).toString();
    entry->urls = stringList(obj.value(QStringLiteral("urls")));
    entry->files = stringList(obj.value(QStringLiteral("files")));
    entry->requiresRamGB = obj.value(QStringLiteral("requiresRamGB")).toInt(0);
    entry->requiresVramGB = obj.value(QStringLiteral("requiresVramGB")).toInt(0);
    entry->note = obj.value(QStringLiteral("note")).toString();

    // size 不得为负（JSON number，可能是小数，按字节取整）。
    const QJsonValue sizeValue = obj.value(QStringLiteral("size"));
    if (sizeValue.isDouble()) {
        const double size = sizeValue.toDouble();
        if (size < 0.0) {
            *error = i18n::sf("资产 %1 的 size 为负数（%2），清单被篡改或发布脚本出错",
                              "Asset %1 has a negative size (%2); the manifest was tampered with or "
                              "the release script is broken",
                              {entry->id, size});
            return false;
        }
        entry->size = static_cast<qint64>(size);
    } else if (!sizeValue.isUndefined() && !sizeValue.isNull()) {
        *error = i18n::sf("资产 %1 的 size 不是数字",
                          "Asset %1 has a non-numeric size", {entry->id});
        return false;
    }

    // urls 只允许 https（拒绝明文 http 与任何其它协议）。
    for (const QString &url : entry->urls) {
        if (!url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
            *error = i18n::sf("资产 %1 的下载地址不是 https:// 开头，拒绝明文传输：%2",
                              "Asset %1 has a non-https download URL; plain-text transfer is "
                              "refused: %2",
                              {entry->id, url});
            return false;
        }
    }
    return true;
}

// 结构与哈希校验（验签之后跑）。
QString validateEntry(const AssetEntry &entry, const ManifestVerifyOptions &opts)
{
    if (entry.urls.isEmpty()) {
        // 占位条目：不下载，sha256 可以为空。
        return QString();
    }
    if (entry.sha256.isEmpty()) {
        if (opts.allowMissingSha256) {
            return QString();
        }
        return i18n::sf("资产 %1 有下载地址但 sha256 为空（发布清单必须带整包哈希）",
                        "Asset %1 has download URLs but no sha256 (a published manifest must carry "
                        "the archive checksum)",
                        {entry.id});
    }
    if (!isHexOfLength(entry.sha256, 64, true)) {
        return i18n::sf("资产 %1 的 sha256 不是 64 位小写 hex：\"%2\"",
                        "Asset %1 has a sha256 that is not 64 lowercase hex characters: \"%2\"",
                        {entry.id, entry.sha256});
    }
    return QString();
}

} // namespace

// ---------------- AssetEntry / Manifest ----------------

bool AssetEntry::isRuntime() const
{
    return type == QLatin1String("runtime");
}

bool AssetEntry::isModel() const
{
    return type == QLatin1String("model");
}

bool AssetEntry::isPlaceholder() const
{
    return urls.isEmpty();
}

const AssetEntry *Manifest::find(const QString &id) const
{
    for (const AssetEntry &entry : assets) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

QVector<AssetEntry> Manifest::ofType(const QString &type) const
{
    QVector<AssetEntry> out;
    out.reserve(assets.size());
    for (const AssetEntry &entry : assets) {
        if (entry.type == type) {
            out.push_back(entry);
        }
    }
    return out;
}

QVector<AssetEntry> Manifest::forPlatform(const QString &op, const QString &arch) const
{
    QVector<AssetEntry> out;
    out.reserve(assets.size());
    for (const AssetEntry &entry : assets) {
        if (!entry.op.isEmpty() && entry.op != op) {
            continue;
        }
        if (!entry.arch.isEmpty() && entry.arch != arch) {
            continue;
        }
        out.push_back(entry);
    }
    return out;
}

// ---------------- parseManifest ----------------

ManifestLoadResult parseManifest(const QByteArray &rawJson, const ManifestVerifyOptions &opts)
{
    if (rawJson.trimmed().isEmpty()) {
        return failResult(i18n::s("清单内容为空（0 字节）", "The manifest is empty (0 bytes)"));
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return failResult(i18n::sf("清单 JSON 解析失败：%1（偏移 %2 字节）",
                                   "Failed to parse the manifest JSON: %1 (at offset %2 bytes)",
                                   {parseError.errorString(), parseError.offset}));
    }
    if (!doc.isObject()) {
        return failResult(i18n::s("清单顶层必须是 JSON 对象",
                                  "The manifest root must be a JSON object"));
    }
    const QJsonObject root = doc.object();

    // (1) payload 必须是 object。
    const QJsonValue payloadValue = root.value(QStringLiteral("payload"));
    if (!payloadValue.isObject()) {
        return failResult(i18n::s("清单缺少 payload 对象",
                                  "The manifest is missing its payload object"));
    }
    const QJsonObject payload = payloadValue.toObject();

    // (3) schema 白名单。放在验签前：schema 不对说明这压根不是我们的清单。
    const QString schema = root.value(QStringLiteral("schema")).toString();
    if (schema != QLatin1String(kManifestSchema)) {
        return failResult(i18n::sf("清单 schema 不受支持：\"%1\"（只接受 \"%2\"）",
                                   "Unsupported manifest schema: \"%1\" (only \"%2\" is accepted)",
                                   {schema, QLatin1String(kManifestSchema)}));
    }

    // (1)(2) sig 存在性 + 格式 + 验签。
    const QJsonValue sigValue = root.value(QStringLiteral("sig"));
    QString sigHex;
    if (!sigValue.isUndefined() && !sigValue.isNull()) {
        if (!sigValue.isString()) {
            return failResult(i18n::s("清单 sig 必须是字符串",
                                      "The manifest sig must be a string"));
        }
        sigHex = sigValue.toString().trimmed();
        if (!sigHex.isEmpty() && !isHexOfLength(sigHex, 128, false)) {
            return failResult(i18n::sf("清单 sig 必须是 128 位 hex（64 字节签名），实际 %1 位",
                                       "The manifest sig must be 128 hex characters (a 64-byte "
                                       "signature); got %1 characters",
                                       {sigHex.size()}));
        }
    }

    if (sigHex.isEmpty()) {
        if (opts.requireSignature) {
            return failResult(i18n::s("清单缺少 sig 签名，拒绝加载未签名的资产清单",
                                      "The manifest has no sig; refusing to load an unsigned asset "
                                      "manifest"));
        }
    } else if (opts.trustedPublicKey.size() == 32) {
        // 签名覆盖字节：payload 的 Compact 序列化（QJsonObject 按 key 排序，确定性可保证）。
        const QByteArray signedBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QByteArray sigBytes = hexToBytes(sigHex);
        const uint8_t *message = reinterpret_cast<const uint8_t *>(signedBytes.constData());
        const int checked = crypto_ed25519_check(
            reinterpret_cast<const uint8_t *>(sigBytes.constData()),
            reinterpret_cast<const uint8_t *>(opts.trustedPublicKey.constData()),
            message,
            static_cast<size_t>(signedBytes.size()));
        if (checked != 0) {
            return failResult(i18n::sf(
                "清单签名校验失败：payload 已被篡改，或不是由受信任密钥签发（keyId=%1）",
                "Manifest signature verification failed: the payload was tampered with, or it was "
                "not signed by the trusted key (keyId=%1)",
                {root.value(QStringLiteral("keyId")).toString()}));
        }
    } else if (opts.requireSignature) {
        // requireSignature 为真但没给公钥：配置错误，必须显式失败，不能静默跳过。
        return failResult(i18n::s("未提供清单验签公钥（trustedPublicKey 必须为 32 字节）",
                                  "No manifest verification key was provided (trustedPublicKey must "
                                  "be 32 bytes)"));
    }

    // (4) schemaVersion 范围。落后于清单 = 客户端太旧，要明确提示。
    Manifest manifest;
    const QJsonValue versionValue = payload.value(QStringLiteral("schemaVersion"));
    if (!versionValue.isDouble()) {
        return failResult(i18n::s("清单缺少 payload.schemaVersion 数字字段",
                                  "The manifest is missing the numeric payload.schemaVersion field"));
    }
    manifest.schemaVersion = versionValue.toInt();
    if (manifest.schemaVersion <= 0) {
        return failResult(i18n::sf("清单 schemaVersion 非法：%1（必须 > 0）",
                                   "Invalid manifest schemaVersion: %1 (it must be > 0)",
                                   {manifest.schemaVersion}));
    }
    if (manifest.schemaVersion > opts.maxSchemaVersion) {
        return failResult(i18n::sf(
            "清单 schemaVersion=%1 超出本客户端支持上限 %2：客户端版本过旧，请升级 Studious 后再试",
            "Manifest schemaVersion=%1 exceeds the highest version this client supports (%2): your "
            "client is too old, please update Studious and try again",
            {manifest.schemaVersion, opts.maxSchemaVersion}));
    }

    manifest.generatedAt = payload.value(QStringLiteral("generatedAt")).toString();
    manifest.channel = payload.value(QStringLiteral("channel")).toString();
    manifest.keyId = root.value(QStringLiteral("keyId")).toString();

    // (5)~(8) 逐条资产。
    const QJsonValue assetsValue = payload.value(QStringLiteral("assets"));
    if (!assetsValue.isArray()) {
        return failResult(i18n::s("清单 payload.assets 必须是数组",
                                  "The manifest payload.assets field must be an array"));
    }
    const QJsonArray assetsArray = assetsValue.toArray();

    QVector<AssetEntry> parsed;
    parsed.reserve(assetsArray.size());
    QStringList seenIds;
    seenIds.reserve(assetsArray.size());

    for (int i = 0; i < assetsArray.size(); ++i) {
        const QJsonValue itemValue = assetsArray.at(i);
        if (!itemValue.isObject()) {
            return failResult(i18n::sf("清单 assets[%1] 不是 JSON 对象",
                                       "Manifest entry assets[%1] is not a JSON object", {i}));
        }

        AssetEntry entry;
        QString error;
        if (!parseEntry(itemValue.toObject(), &entry, &error)) {
            return failResult(i18n::sf("清单 assets[%1]：%2", "Manifest assets[%1]: %2", {i, error}));
        }

        // id 全局唯一：重复 = 发布脚本出错，直接拒绝（否则安装器会随机选一条）。
        if (seenIds.contains(entry.id)) {
            return failResult(i18n::sf("清单资产 id 重复：%1（id 必须全局唯一）",
                                       "Duplicate asset id in the manifest: %1 (ids must be unique)",
                                       {entry.id}));
        }
        seenIds << entry.id;

        const QString invalid = validateEntry(entry, opts);
        if (!invalid.isEmpty()) {
            return failResult(i18n::sf("清单 assets[%1]：%2", "Manifest assets[%1]: %2",
                                       {i, invalid}));
        }

        parsed.push_back(entry);
    }

    manifest.assets = parsed;

    ManifestLoadResult result;
    result.ok = true;
    result.manifest = manifest;
    return result;
}

// ---------------- 工具函数 ----------------

QString sha256File(const QString &path, QString *error)
{
    const auto setError = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return QString();
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return setError(i18n::sf("无法打开文件：%1（%2）",
                                 "Unable to open the file: %1 (%2)", {path, file.errorString()}));
    }

    // 分块读：模型包动辄几 GB，绝不能整个读进内存。
    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunk = 1 << 20;   // 1 MiB
    QByteArray buffer;
    buffer.resize(static_cast<int>(kChunk));

    for (;;) {
        const qint64 read = file.read(buffer.data(), kChunk);
        if (read < 0) {
            return setError(i18n::sf("读取文件失败：%1（%2）",
                                     "Failed to read the file: %1 (%2)",
                                     {path, file.errorString()}));
        }
        if (read == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(read)));
    }
    file.close();

    if (error != nullptr) {
        error->clear();
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool verifyFileSha256(const QString &path, const QString &expectedHex, QString *error)
{
    const QString expected = expectedHex.trimmed().toLower();
    if (!isHexOfLength(expected, 64, true)) {
        if (error != nullptr) {
            *error = i18n::sf("期望的 sha256 不是 64 位 hex：\"%1\"",
                              "The expected sha256 is not 64 hex characters: \"%1\"", {expectedHex});
        }
        return false;
    }

    QString hashError;
    const QString actual = sha256File(path, &hashError);
    if (actual.isEmpty()) {
        if (error != nullptr) {
            *error = hashError;
        }
        return false;
    }

    if (actual != expected) {
        if (error != nullptr) {
            *error = i18n::sf("sha256 不匹配：文件 %1，期望 %2",
                              "SHA-256 mismatch: file is %1, expected %2", {actual, expected});
        }
        return false;
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

QByteArray hexToBytes(const QString &hex)
{
    return QByteArray::fromHex(hex.toLatin1());
}

QString bytesToHex(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

} // namespace studious::asset
