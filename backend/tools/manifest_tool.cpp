// Studious 资产清单签名 / 校验工具（开发与发布用，**不随产品分发**）。
//
// 子命令：
//   genkey                                          生成一对 ed25519 密钥，打印 seed / pubkey
//   sign <payload.json> <seedHex> <out.json>        读 payload，Compact 序列化后签名，输出完整信封
//   verify <manifest.json> [pubkeyHex]              调 parseManifest 校验（省略 pubkey = 只做结构校验）
//
// 关键约束：签名覆盖的字节必须与 parseManifest 验签时重算的字节一致 ——
// 两边都是 QJsonDocument(payload).toJson(QJsonDocument::Compact)，所以本工具**不自己拼字符串**，
// 一律走 QJsonDocument。改这里的序列化方式 = 破坏服务端与客户端互信。
//
// 构建产物：backend/build/manifest_tool.exe（依赖 Qt DLL 时先把 Qt bin 加进 PATH）。

#include <studious/asset/Manifest.h>
#include <studious/asset/TrustKeys.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"   // crypto_wipe：擦掉私钥缓冲

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstring>

using namespace studious::asset;

namespace {

void say(const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int usage()
{
    say(QStringLiteral("用法："));
    say(QStringLiteral("  manifest_tool genkey"));
    say(QStringLiteral("  manifest_tool sign <payload.json> <seedHex> <out.json>"));
    say(QStringLiteral("  manifest_tool verify <manifest.json> [pubkeyHex]"));
    say(QString());
    say(QStringLiteral("  genkey  生成 ed25519 密钥对；seed 是私钥，必须离线保存，绝不进仓库。"));
    say(QStringLiteral("  sign    payload.json 只含 payload 对象，输出完整信封（schema/keyId/payload/sig）。"));
    say(QStringLiteral("  verify  省略 pubkeyHex 时只做结构校验（requireSignature=false，仅开发期用）。"));
    return 2;
}

QByteArray readWholeFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法打开 %1：%2").arg(path, file.errorString());
        return {};
    }
    const QByteArray data = file.readAll();
    file.close();
    return data;
}

bool writeWholeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }
    const qint64 written = file.write(data);
    file.close();
    if (written != data.size()) {
        *error = QStringLiteral("写入 %1 不完整（%2/%3 字节）").arg(path).arg(written).arg(data.size());
        return false;
    }
    return true;
}

// 定宽补空格：中文按 2 列粗算，够看表格就行。
QString pad(const QString &text, int width)
{
    int used = 0;
    for (const QChar &c : text) {
        used += (c.unicode() > 0x2E80) ? 2 : 1;
    }
    QString out = text;
    for (int i = used; i < width; ++i) {
        out += QLatin1Char(' ');
    }
    return out;
}

QString humanSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("-");
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return QStringLiteral("%1 GB").arg(QString::number(mb / 1024.0, 'f', 2));
    }
    return QStringLiteral("%1 MB").arg(QString::number(mb, 'f', 1));
}

void printAssets(const Manifest &manifest)
{
    say(QStringLiteral("资产共 %1 条：").arg(manifest.assets.size()));
    const QString header = pad(QStringLiteral("id"), 40) + QLatin1Char('|')
                           + pad(QStringLiteral("type"), 9) + QLatin1Char('|')
                           + pad(QStringLiteral("平台"), 16) + QLatin1Char('|')
                           + pad(QStringLiteral("backend"), 9) + QLatin1Char('|')
                           + pad(QStringLiteral("体积"), 10) + QLatin1Char('|')
                           + pad(QStringLiteral("源"), 4) + QLatin1Char('|')
                           + QStringLiteral("sha256 / 备注");
    say(header);
    say(QString(static_cast<int>(99), QLatin1Char('-')));

    for (const AssetEntry &entry : manifest.assets) {
        const QString platform = entry.op.isEmpty() && entry.arch.isEmpty()
                                     ? QStringLiteral("全平台")
                                     : entry.op + QLatin1Char('/') + entry.arch;
        QString tail;
        if (entry.isPlaceholder()) {
            tail = QStringLiteral("<占位：无下载地址>");
        } else if (entry.sha256.isEmpty()) {
            tail = QStringLiteral("<无 sha256>");
        } else {
            tail = entry.sha256.left(16) + QStringLiteral("…");
        }
        if (!entry.note.isEmpty()) {
            tail += QStringLiteral("  ") + entry.note;
        }

        say(pad(entry.id, 40) + QLatin1Char('|') + pad(entry.type, 9) + QLatin1Char('|')
            + pad(platform, 16) + QLatin1Char('|')
            + pad(entry.backend.isEmpty() ? QStringLiteral("-") : entry.backend, 9)
            + QLatin1Char('|') + pad(humanSize(entry.size), 10) + QLatin1Char('|')
            + pad(QString::number(entry.urls.size()), 4) + QLatin1Char('|') + tail);
    }
}

int cmdGenkey()
{
    // QRandomGenerator::generate(begin,end) 按 32 位整数填充，拿 uint8_t* 当迭代器会越界写，
    // 所以先填 8 个 quint32 再 memcpy 成 32 字节 seed。
    quint32 words[8] = {};
    QRandomGenerator::system()->fillRange(words, 8);

    uint8_t seed[32] = {};
    std::memcpy(seed, words, sizeof(seed));
    std::memset(words, 0, sizeof(words));

    // ⚠️ 坑：Monocypher 的 crypto_ed25519_key_pair() 内部会 crypto_wipe(seed, 32)
    // （seed 参数非 const 就是为了这个），调用完 seed 已经全 0 —— 必须先把 hex 抄出来。
    const QByteArray seedHex = QByteArray(reinterpret_cast<const char *>(seed), 32).toHex();

    uint8_t secretKey[64] = {};
    uint8_t publicKey[32] = {};
    crypto_ed25519_key_pair(secretKey, publicKey, seed);
    crypto_wipe(secretKey, sizeof(secretKey));

    const QByteArray pubHex = QByteArray(reinterpret_cast<const char *>(publicKey), 32).toHex();

    say(QStringLiteral("=== ed25519 密钥对（RFC 8032 / Monocypher 4.0.2） ==="));
    say(QStringLiteral("seed=%1").arg(QString::fromLatin1(seedHex)));
    say(QStringLiteral("pubkey=%1").arg(QString::fromLatin1(pubHex)));
    say(QString());
    say(QStringLiteral("把 pubkey 填进 backend/include/studious/asset/TrustKeys.h 的 "
                       "kManifestPublicKeyHex；"));
    say(QStringLiteral("seed 就是私钥：离线保存，绝不进仓库、不发网络。"));
    return 0;
}

int cmdSign(const QString &payloadPath, const QString &seedHex, const QString &outPath)
{
    const QByteArray seedBytes = hexToBytes(seedHex.trimmed());
    if (seedBytes.size() != 32) {
        say(QStringLiteral("[FAIL] seed 必须是 64 位 hex（32 字节），实际 %1 字节").arg(seedBytes.size()));
        return 1;
    }

    QString error;
    const QByteArray rawPayload = readWholeFile(payloadPath, &error);
    if (!error.isEmpty()) {
        say(QStringLiteral("[FAIL] ") + error);
        return 1;
    }

    QJsonParseError parseError{};
    const QJsonDocument payloadDoc = QJsonDocument::fromJson(rawPayload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !payloadDoc.isObject()) {
        say(QStringLiteral("[FAIL] payload 不是合法 JSON 对象：%1（偏移 %2）")
                .arg(parseError.errorString())
                .arg(parseError.offset));
        return 1;
    }

    // 签名覆盖字节：与 parseManifest 验签时重算的完全一致。
    const QJsonObject payload = payloadDoc.object();
    const QByteArray signedBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    uint8_t seedBuf[32] = {};
    std::memcpy(seedBuf, seedBytes.constData(), 32);

    uint8_t secretKey[64] = {};
    uint8_t publicKey[32] = {};
    crypto_ed25519_key_pair(secretKey, publicKey, seedBuf);

    uint8_t signature[64] = {};
    crypto_ed25519_sign(signature, secretKey,
                        reinterpret_cast<const uint8_t *>(signedBytes.constData()),
                        static_cast<size_t>(signedBytes.size()));

    // 一次性擦掉私钥缓冲（工具进程，防随手 dump 内存）。
    crypto_wipe(secretKey, sizeof(secretKey));
    crypto_wipe(seedBuf, sizeof(seedBuf));

    QJsonObject envelope;
    envelope.insert(QStringLiteral("schema"), QLatin1String(kManifestSchema));
    envelope.insert(QStringLiteral("keyId"), QLatin1String(kManifestKeyId));
    envelope.insert(QStringLiteral("payload"), payload);
    envelope.insert(QStringLiteral("sig"),
                    QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(signature), 64)
                                            .toHex()));

    QByteArray out = QJsonDocument(envelope).toJson(QJsonDocument::Indented);
    out.append('\n');

    if (!writeWholeFile(outPath, out, &error)) {
        say(QStringLiteral("[FAIL] ") + error);
        return 1;
    }

    say(QStringLiteral("[OK] 已签名：%1").arg(outPath));
    say(QStringLiteral("     keyId    = %1").arg(QLatin1String(kManifestKeyId)));
    say(QStringLiteral("     pubkey   = %1")
            .arg(QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(publicKey), 32)
                                         .toHex())));
    say(QStringLiteral("     签名覆盖 = QJsonDocument(payload).toJson(Compact)，%1 字节")
            .arg(signedBytes.size()));
    return 0;
}

int cmdVerify(const QString &manifestPath, const QString &pubkeyHex)
{
    QString error;
    const QByteArray raw = readWholeFile(manifestPath, &error);
    if (!error.isEmpty()) {
        say(QStringLiteral("[FAIL] ") + error);
        return 1;
    }

    ManifestVerifyOptions opts;
    opts.maxSchemaVersion = 1;

    if (pubkeyHex.trimmed().isEmpty()) {
        // 开发期：只做结构校验，不验签。
        opts.requireSignature = false;
        opts.allowMissingSha256 = true;
        say(QStringLiteral("[INFO] 未提供 pubkeyHex → requireSignature=false，只做结构校验（不验签）"));
    } else {
        const QByteArray key = hexToBytes(pubkeyHex.trimmed());
        if (key.size() != 32) {
            say(QStringLiteral("[FAIL] pubkey 必须是 64 位 hex（32 字节），实际 %1 字节").arg(key.size()));
            return 1;
        }
        opts.trustedPublicKey = key;
        opts.requireSignature = true;
    }

    const ManifestLoadResult result = parseManifest(raw, opts);
    if (!result.ok) {
        say(QStringLiteral("[FAIL] 清单校验失败：") + result.error);
        return 1;
    }

    const Manifest &manifest = result.manifest;
    say(QStringLiteral("[OK] 清单校验通过：%1").arg(manifestPath));
    say(QStringLiteral("     schemaVersion = %1").arg(manifest.schemaVersion));
    say(QStringLiteral("     generatedAt   = ")
        + (manifest.generatedAt.isEmpty() ? QStringLiteral("<空>") : manifest.generatedAt));
    say(QStringLiteral("     channel       = ")
        + (manifest.channel.isEmpty() ? QStringLiteral("<空>") : manifest.channel));
    say(QStringLiteral("     keyId         = ")
        + (manifest.keyId.isEmpty() ? QStringLiteral("<空>") : manifest.keyId));
    say(QStringLiteral("     验签          = %1").arg(opts.requireSignature ? QStringLiteral("已验签（ed25519）")
                                                                            : QStringLiteral("跳过")));
    say(QString());

    printAssets(manifest);
    say(QString());
    say(QStringLiteral("按类型统计：runtime=%1，model=%2")
            .arg(manifest.ofType(QStringLiteral("runtime")).size())
            .arg(manifest.ofType(QStringLiteral("model")).size()));

    const int placeholders = [&manifest]() {
        int n = 0;
        for (const AssetEntry &e : manifest.assets) {
            if (e.isPlaceholder()) {
                ++n;
            }
        }
        return n;
    }();
    say(QStringLiteral("占位条目（无下载地址）= %1").arg(placeholders));
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();

    if (args.size() < 2) {
        return usage();
    }

    const QString command = args.at(1);

    if (command == QStringLiteral("genkey")) {
        return cmdGenkey();
    }
    if (command == QStringLiteral("sign")) {
        if (args.size() != 5) {
            return usage();
        }
        return cmdSign(args.at(2), args.at(3), args.at(4));
    }
    if (command == QStringLiteral("verify")) {
        if (args.size() < 3 || args.size() > 4) {
            return usage();
        }
        return cmdVerify(args.at(2), args.size() == 4 ? args.at(3) : QString());
    }

    say(QStringLiteral("[FAIL] 未知子命令：") + command);
    return usage();
}
