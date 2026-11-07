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

#include <studious/asset/TrustKeys.h>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>

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

// Windows 保留设备名：CON / PRN / AUX / NUL / COM1-9 / LPT1-9。
// 判断时去掉扩展名（Win32 解析路径会忽略扩展名，CON.txt 依然落到设备上）并去掉尾随空格。
bool isReservedDeviceStem(const QString &segment)
{
    QString stem = segment;
    const int dot = stem.indexOf(QLatin1Char('.'));
    if (dot > 0) {
        stem.truncate(dot);
    }
    while (stem.endsWith(QLatin1Char(' '))) {
        stem.chop(1);
    }
    if (stem.isEmpty()) {
        return false;
    }

    const QString upper = stem.toUpper();
    if (upper == QLatin1String("CON") || upper == QLatin1String("PRN")
        || upper == QLatin1String("AUX") || upper == QLatin1String("NUL")
        || upper == QLatin1String("CONIN$") || upper == QLatin1String("CONOUT$")) {
        return true;
    }
    if (upper.size() == 4) {
        const QString prefix = upper.left(3);
        const QChar last = upper.at(3);
        if ((prefix == QLatin1String("COM") || prefix == QLatin1String("LPT"))
            && last >= QLatin1Char('0') && last <= QLatin1Char('9')) {
            return true;
        }
    }
    return false;
}

// 编译进二进制的默认验签公钥（TrustKeys.h）。返回空 = 这个构建没配公钥。
QByteArray builtinTrustedKey()
{
    const QByteArray raw = QByteArray::fromHex(QByteArray(kManifestPublicKeyHex));
    return raw.size() == 32 ? raw : QByteArray();
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

    // id 会被拼成 <root>/models/<id> 或 <root>/runtime/<id>，卸载时还会对该路径递归删除，
    // 所以这里必须白名单化：不合规的 id 一律在解析阶段就拒绝，绝不带进任何路径拼接。
    QString idError;
    if (!isSafeAssetId(entry->id, &idError)) {
        *error = i18n::sf("资产 id \"%1\" 非法：%2", "Unsafe asset id \"%1\": %2",
                          {entry->id, idError});
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
    // files 是"安装后逐个检查存在性"的清单：里面的相对路径同样不允许穿越/ADS/保留名，
    // 否则恶意清单能靠 "files": ["../../../etc/passwd"] 把校验点挪到安装目录外。
    for (const QString &relative : entry.files) {
        QString pathError;
        if (!isSafeRelativeEntryPath(relative, &pathError)) {
            return i18n::sf("资产 %1 的 files 条目 \"%2\" 非法：%3",
                            "Asset %1 has an unsafe files entry \"%2\": %3",
                            {entry.id, relative, pathError});
        }
    }

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

    // 验签密钥来源：调用方显式给的优先；没给但要求验签时，退回编译进二进制的
    // TrustKeys 公钥——"忘记传公钥"不应该退化成"根本不验签"。
    QByteArray trustKey = opts.trustedPublicKey;
    if (trustKey.isEmpty() && opts.requireSignature) {
        trustKey = builtinTrustedKey();
    }

    if (sigHex.isEmpty()) {
        if (opts.requireSignature) {
            return failResult(i18n::s("清单缺少 sig 签名，拒绝加载未签名的资产清单",
                                      "The manifest has no sig; refusing to load an unsigned asset "
                                      "manifest"));
        }
        // 走到这里说明调用方主动关了验签（开发期 qrc 清单 / 手工构造的清单）。
        // 不能静默：日志里必须能看出"这份清单没被密码学校验过"。
        const QString warning =
            i18n::s("清单未验签：调用方关闭了 requireSignature，清单内容未经密码学校验，"
                    "请勿在生产环境使用",
                    "Manifest is NOT signature-verified: the caller disabled requireSignature, so "
                    "the manifest content is unauthenticated; do not use this in production");
        qWarning("%s", qUtf8Printable(warning));
    } else if (trustKey.size() == 32) {
        // 签名覆盖字节：payload 的 Compact 序列化（QJsonObject 按 key 排序，确定性可保证）。
        const QByteArray signedBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QByteArray sigBytes = hexToBytes(sigHex);
        const uint8_t *message = reinterpret_cast<const uint8_t *>(signedBytes.constData());
        const int checked = crypto_ed25519_check(
            reinterpret_cast<const uint8_t *>(sigBytes.constData()),
            reinterpret_cast<const uint8_t *>(trustKey.constData()),
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
        // requireSignature 为真但没给公钥（调用方没传、TrustKeys 也没配）：配置错误，
        // 必须显式失败，不能静默跳过。
        return failResult(i18n::s("未提供清单验签公钥（trustedPublicKey 必须为 32 字节）",
                                  "No manifest verification key was provided (trustedPublicKey must "
                                  "be 32 bytes)"));
    } else {
        // 有签名、没公钥、调用方又不要求验签：同样留痕，别让它悄悄变成"看起来验过了"。
        const QString warning = i18n::sf(
            "清单未验签：清单带 sig 但没有可用的验签公钥（keyId=%1），签名未被校验",
            "Manifest is NOT signature-verified: it carries a sig but no usable public key is "
            "configured (keyId=%1), so the signature was never checked",
            {root.value(QStringLiteral("keyId")).toString()});
        qWarning("%s", qUtf8Printable(warning));
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

// ---------------- 路径安全原语 ----------------

bool isSafeAssetId(const QString &id, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    if (id.isEmpty()) {
        return fail(i18n::s("资产 id 为空", "The asset id is empty"));
    }
    if (id.size() > kMaxAssetIdLength) {
        return fail(i18n::sf("资产 id 过长：%1 字符（上限 %2）",
                             "The asset id is too long: %1 characters (limit %2)",
                             {id.size(), kMaxAssetIdLength}));
    }

    // 字符白名单：'/'、':'、'\'、控制字符全部在这里被挡掉，路径穿越与 ADS 无从下手。
    for (const QChar &c : id) {
        const ushort u = c.unicode();
        const bool allowed = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
            || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        if (!allowed) {
            return fail(i18n::sf("资产 id 含非法字符 \"%1\"（只允许 A-Z a-z 0-9 . _ -）",
                                 "The asset id contains an illegal character \"%1\" (only A-Z a-z "
                                 "0-9 . _ - are allowed)",
                                 {QString(c)}));
        }
    }
    if (id.startsWith(QLatin1Char('.'))) {
        return fail(i18n::s("资产 id 不能以 '.' 开头（隐藏文件 / 相对路径）",
                            "The asset id must not start with '.' (hidden file or relative path)"));
    }
    if (id.endsWith(QLatin1Char('.'))) {
        return fail(i18n::s("资产 id 不能以 '.' 结尾（Windows 会静默去掉尾点）",
                            "The asset id must not end with '.' (Windows strips trailing dots)"));
    }
    if (id.contains(QLatin1String(".."))) {
        return fail(i18n::s("资产 id 不能包含 \"..\"（可能造成目录穿越）",
                            "The asset id must not contain \"..\" (directory traversal)"));
    }
    if (isReservedDeviceStem(id)) {
        return fail(i18n::sf("资产 id 是 Windows 保留设备名：%1",
                             "The asset id is a reserved Windows device name: %1", {id}));
    }
    return true;
}

bool isSafeRelativeEntryPath(const QString &raw, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    if (raw.isEmpty()) {
        return fail(i18n::s("路径为空", "The path is empty"));
    }

    // 统一分隔符：Windows 上打的 zip 常带反斜杠，先归一，免得漏判每一种检查。
    QString name = raw;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // 去掉 "./" 前缀与目录条目尾部的 '/'（打包工具会带）。
    while (name.startsWith(QLatin1String("./"))) {
        name.remove(0, 2);
    }
    while (name.endsWith(QLatin1Char('/'))) {
        name.chop(1);
    }
    if (name.isEmpty() || name == QLatin1String(".")) {
        return fail(i18n::s("路径为空（只有 ./ 或 /）", "The path is empty (only ./ or /)"));
    }

    for (const QChar &c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            return fail(i18n::sf("路径含控制字符，拒绝：%1",
                                 "The path contains a control character; refusing: %1", {raw}));
        }
    }

    if (name.startsWith(QLatin1Char('/'))) {
        return fail(i18n::sf("路径是绝对路径（或 UNC 路径），拒绝：%1",
                             "The path is absolute (or a UNC path); refusing: %1", {raw}));
    }
    // 盘符：C:/x 以及 C:x 这种"驱动器相对路径"（Windows 会解析到该盘的当前目录）。
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':')) {
        return fail(i18n::sf("路径带盘符，拒绝：%1",
                             "The path carries a drive letter; refusing: %1", {raw}));
    }
    // NTFS 备用数据流（a.txt:evil）：写进流里，主文件仍是干净的原内容——
    // 校验看的是主流，运行的却可能是别的，所以任意位置的 ':' 一律拒绝。
    if (name.contains(QLatin1Char(':'))) {
        return fail(i18n::sf("路径含 ':'（NTFS 备用数据流），拒绝：%1",
                             "The path contains ':' (NTFS alternate data stream); refusing: %1",
                             {raw}));
    }

    const QStringList segments = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QLatin1String(".")) {
            continue;
        }
        if (segment == QLatin1String("..")) {
            return fail(i18n::sf("路径包含 \"..\" 段，可能造成目录穿越（zip-slip），拒绝：%1",
                                 "The path contains a \"..\" segment (zip-slip); refusing: %1",
                                 {raw}));
        }
        if (segment.endsWith(QLatin1Char('.')) || segment.endsWith(QLatin1Char(' '))) {
            return fail(i18n::sf("路径段以 '.' 或空格结尾（Windows 会静默去掉，落盘名与校验名不一致），"
                                 "拒绝：%1",
                                 "A path segment ends with '.' or a space (Windows silently strips "
                                 "it, so the on-disk name would differ from the checked name); "
                                 "refusing: %1",
                                 {raw}));
        }
        if (isReservedDeviceStem(segment)) {
            return fail(i18n::sf("路径段 \"%1\" 是 Windows 保留设备名，拒绝：%2",
                                 "The path segment \"%1\" is a reserved Windows device name; "
                                 "refusing: %2",
                                 {segment, raw}));
        }
    }
    return true;
}

bool isPathInside(const QString &path, const QString &dir)
{
    if (path.isEmpty() || dir.isEmpty()) {
        return false;
    }
    const QString cleanDir = QDir::cleanPath(dir);
    const QString cleanPath = QDir::cleanPath(path);
    if (cleanDir.isEmpty() || cleanPath.isEmpty() || cleanPath == cleanDir) {
        return false;   // 自身不算"在里面"
    }

    QString prefix = cleanDir;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
#if defined(Q_OS_WIN)
    return cleanPath.startsWith(prefix, Qt::CaseInsensitive);
#else
    return cleanPath.startsWith(prefix);
#endif
}

} // namespace studious::asset
