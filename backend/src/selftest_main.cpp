// Studious 后端控制台自测。
// 覆盖：Mock 引擎数据契约、语言码表、Router 强制引擎降级、真实 Web 翻译（允许失败）、
//       Web 来源注册表、LLM 真实接入、资产清单验签、双语文案、资产下载/安装/卸载。
// 用法：
//   backend_selftest.exe            包含网络测试
//   backend_selftest.exe --offline  跳过网络测试
//   backend_selftest.exe --providers 打印 Web 来源表 + 装配校验
//   backend_selftest.exe --manifest  资产清单解析 + ed25519 验签（离线，用 TEST seed 现场签名）
//   backend_selftest.exe --i18n      错误文案中英双语（离线，纯本地）
//   backend_selftest.exe --assets    资产下载 / 续传 / 校验 / 安装 / 卸载（本地 HTTP 服务器，不碰外网）
//   backend_selftest.exe --llm       用本机网关真实翻译两句话（需要网关在 127.0.0.1:3065）
//                                    可用环境变量临时换网关/Key/模型排查：
//                                    STUDIOUS_LLM_BASE_URL / STUDIOUS_LLM_API_KEY / STUDIOUS_LLM_MODEL

#include "Factory.h"
#include "asset/AssetRegistry.h"
#include "asset/Downloader.h"
#include "asset/Manifest.h"
#include "asset/TrustKeys.h"
#include "cache/TranslationCache.h"
#include "config/LlmConfig.h"
#include "i18n/L10n.h"
#include "llm/LlmCatalog.h"
#include "llm/LlmModelInfo.h"
#include "contract/LanguageRegistry.h"
#include "net/QtHttpClient.h"
#include "router/TranslatorRouter.h"
#include "translators/LlmTranslator.h"
#include "translators/LocalMtTranslator.h"
#include "translators/MockTranslator.h"
#include "translators/WebProvider.h"
#include "translators/WebTranslator.h"
#include "translators/adapters/GoogleFreeAdapter.h"
#include "translators/adapters/MyMemoryAdapter.h"

#include "miniz.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

using namespace studious;

namespace {

int g_failed = 0;
QStringList g_failedItems;

// 控制台输出：一律写 UTF-8 字节。
// 现代 Windows Terminal / PowerShell 7 默认 UTF-8；老的 cp936 控制台请先执行 `chcp 65001`。
// 这里不用平台 API 探测代码页，后端保持平台无关。
void say(const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void markPass(const QString &name)
{
    say(QStringLiteral("[PASS] ") + name);
}

void markFail(const QString &name, const QString &why)
{
    ++g_failed;
    g_failedItems << QStringLiteral("%1: %2").arg(name, why);
    say(QStringLiteral("[FAIL] %1 — %2").arg(name, why));
}

void check(const QString &name, bool condition, const QString &why)
{
    if (condition) {
        markPass(name);
    } else {
        markFail(name, why);
    }
}

// 定宽补空格：仅按字符数补，中文按 2 列宽粗略估算，够表格看就行。
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

// 把所有需要断言的子条件攒起来，全部通过才算这一项 PASS。
class Assertions {
public:
    void expect(bool cond, const QString &detail)
    {
        if (!cond) {
            m_ok = false;
            m_details << detail;
        }
    }

    bool ok() const { return m_ok; }
    QString details() const { return m_details.join(QStringLiteral("；")); }

private:
    bool m_ok = true;
    QStringList m_details;
};

// ---------- 1. Mock 引擎 ----------
void testMock()
{
    studious::translate::MockTranslator mock;
    const QStringList paragraphs = {QStringLiteral("Hello world"),
                                    QStringLiteral("This is a test.")};
    const QStringList dsts = {QStringLiteral("zh"), QStringLiteral("ja")};

    const core::TranslateOutcome r = mock.translate(paragraphs, QStringLiteral("auto"), dsts, {});

    Assertions a;
    a.expect(r.ok, QStringLiteral("ok=false, error=%1").arg(r.error));
    a.expect(r.byLang.size() == 2,
             QStringLiteral("byLang 语言数=%1（期望 2）").arg(r.byLang.size()));

    const QStringList zh = r.byLang.value(QStringLiteral("zh"));
    const QStringList ja = r.byLang.value(QStringLiteral("ja"));

    a.expect(zh.size() == 2, QStringLiteral("zh 段落数=%1（期望 2）").arg(zh.size()));
    a.expect(ja.size() == 2, QStringLiteral("ja 段落数=%1（期望 2）").arg(ja.size()));

    if (zh.size() == 2) {
        a.expect(zh.at(0).startsWith(QStringLiteral("【zh】")),
                 QStringLiteral("zh[0] 前缀不对: \"%1\"").arg(zh.at(0)));
        a.expect(zh.at(1).startsWith(QStringLiteral("【zh】")),
                 QStringLiteral("zh[1] 前缀不对: \"%1\"").arg(zh.at(1)));
    }
    if (ja.size() == 2) {
        a.expect(ja.at(0).startsWith(QStringLiteral("【ja】")),
                 QStringLiteral("ja[0] 前缀不对: \"%1\"").arg(ja.at(0)));
        a.expect(ja.at(1).startsWith(QStringLiteral("【ja】")),
                 QStringLiteral("ja[1] 前缀不对: \"%1\"").arg(ja.at(1)));
    }

    check(QStringLiteral("mock"), a.ok(), a.details());
    if (zh.size() == 2) {
        say(QStringLiteral("        zh[0] = ") + zh.at(0));
    }
    if (ja.size() == 2) {
        say(QStringLiteral("        ja[0] = ") + ja.at(0));
    }
}

// ---------- 2. 语言码表 ----------
void testLanguageRegistry()
{
    Assertions a;
    const auto eq = [&a](const QString &input, const QString &expect, const QString &what) {
        const QString actual = core::LanguageRegistry::normalize(input);
        a.expect(actual == expect,
                 QStringLiteral("%1: normalize(\"%2\") = \"%3\"（期望 \"%4\"）")
                     .arg(what, input, actual, expect));
    };

    eq(QStringLiteral("zh-cn"), QStringLiteral("zh"), QStringLiteral("简体别名"));
    eq(QStringLiteral("zh_CN"), QStringLiteral("zh"), QStringLiteral("下划线写法"));
    eq(QStringLiteral("zh-Hans"), QStringLiteral("zh"), QStringLiteral("Hans"));
    eq(QStringLiteral("zh-Hant"), QStringLiteral("zh-TW"), QStringLiteral("繁体别名"));
    eq(QStringLiteral("zh_TW"), QStringLiteral("zh-TW"), QStringLiteral("繁体下划线"));
    eq(QStringLiteral("jp"), QStringLiteral("ja"), QStringLiteral("日语误写"));
    eq(QStringLiteral("kr"), QStringLiteral("ko"), QStringLiteral("韩语误写"));
    eq(QStringLiteral("pt-br"), QStringLiteral("pt-BR"), QStringLiteral("巴西葡语"));
    eq(QStringLiteral("AUTO"), QStringLiteral("auto"), QStringLiteral("自动检测大写"));

    const QString googleZh = core::LanguageRegistry::toGoogle(QStringLiteral("zh"));
    a.expect(googleZh == QStringLiteral("zh-CN"),
             QStringLiteral("toGoogle(\"zh\") = \"%1\"（期望 \"zh-CN\"）").arg(googleZh));

    const QString myMemoryZh = core::LanguageRegistry::toMyMemory(QStringLiteral("zh"));
    a.expect(myMemoryZh == QStringLiteral("zh-CN"),
             QStringLiteral("toMyMemory(\"zh\") = \"%1\"（期望 \"zh-CN\"）").arg(myMemoryZh));

    a.expect(core::LanguageRegistry::allInternal().contains(QStringLiteral("zh-TW")),
             QStringLiteral("allInternal() 缺少 zh-TW"));
    a.expect(core::LanguageRegistry::displayName(QStringLiteral("zh")) == QStringLiteral("中文(简体)"),
             QStringLiteral("displayName(\"zh\") 不对"));

    check(QStringLiteral("langreg"), a.ok(), a.details());
}

// ---------- 3. Router 强制引擎降级 ----------
router::TranslatorList buildAllEngines(studious::net::QtHttpClient &http)
{
    using namespace studious::translate;

    QVector<std::shared_ptr<IWebAdapter>> adapters;
    adapters.reserve(2);
    adapters.push_back(std::make_shared<GoogleFreeAdapter>());
    adapters.push_back(std::make_shared<MyMemoryAdapter>());

    // std::vector 而非 QVector：QList 要求元素可拷贝，装不了 unique_ptr（见 TranslatorRouter.h）。
    router::TranslatorList engines;
    engines.reserve(4);
    engines.push_back(std::make_unique<MockTranslator>());
    engines.push_back(std::make_unique<WebTranslator>(http, adapters));
    engines.push_back(std::make_unique<LlmTranslator>());
    engines.push_back(std::make_unique<LocalMtTranslator>());
    return engines;
}

void testRouterDegrade(studious::net::QtHttpClient &http)
{
    studious::router::TranslatorRouter router(buildAllEngines(http));

    core::TranslateRequest req;
    req.paragraphs = {QStringLiteral("Hello world")};
    req.srcLang = QStringLiteral("auto");
    req.dstLangs = {QStringLiteral("zh")};
    req.preferredEngine = QStringLiteral("llm");

    const core::TranslateOutcome r = router.translate(req);

    Assertions a;
    a.expect(!r.ok, QStringLiteral("强制 llm 竟然成功了，状态不可信"));
    a.expect(!r.error.isEmpty(), QStringLiteral("失败但 error 为空，UI 无法提示"));
    a.expect(r.error.contains(QStringLiteral("llm")),
             QStringLiteral("error 未指明是 llm 引擎: %1").arg(r.error));

    check(QStringLiteral("router-degrade"), a.ok(), a.details());
    say(QStringLiteral("        error = ") + r.error);
}

// ---------- 4. 真实网络翻译（允许失败） ----------
void testWebNetwork(studious::net::QtHttpClient &http)
{
    studious::router::TranslatorRouter router(buildAllEngines(http));

    core::TranslateRequest req;
    req.paragraphs = {QStringLiteral("Hello, world!")};
    req.srcLang = QStringLiteral("auto");
    req.dstLangs = {QStringLiteral("zh")};
    req.preferredEngine = QStringLiteral("web");

    const core::TranslateOutcome r = router.translate(req);

    // 网络不通不算失败：只要求不崩溃、结果或错误都清晰可读。
    if (r.ok) {
        const QStringList zh = r.byLang.value(QStringLiteral("zh"));
        say(QStringLiteral("[INFO] web: ") + (zh.isEmpty() ? QStringLiteral("<空>") : zh.at(0)));
    } else {
        say(QStringLiteral("[INFO] web 失败（网络问题不算用例失败）: ") + r.error);
    }
}

// ---------- 4.5 资产清单 / ed25519 验签（--manifest） ----------
// 全部在内存里造 payload + 现场签名，不依赖网络、不依赖仓库里的示例清单。
//
// ⚠️ TEST KEY：下面这个 seed 是 RFC 8032 的公开测试向量，写死在源码里 = 谁都能签出"合法"清单。
//    它只能用来验证"验签链路本身对不对"，绝不能用于任何真实发布签名。
//    真实私钥离线保存，公钥走 include/studious/asset/TrustKeys.h。
constexpr const char *kTestSeedHex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

struct TestKeyPair {
    QByteArray seed;   // 32 字节
    QByteArray pub;    // 32 字节
};

TestKeyPair testKeyPair()
{
    TestKeyPair kp;
    kp.seed = asset::hexToBytes(QString::fromLatin1(kTestSeedHex));
    if (kp.seed.size() != 32) {
        return kp;
    }

    uint8_t seedBuf[32] = {};
    std::memcpy(seedBuf, kp.seed.constData(), 32);

    uint8_t secretKey[64] = {};
    uint8_t publicKey[32] = {};
    crypto_ed25519_key_pair(secretKey, publicKey, seedBuf);
    crypto_wipe(secretKey, sizeof(secretKey));

    kp.pub = QByteArray(reinterpret_cast<const char *>(publicKey), 32);
    return kp;
}

// 用 TEST seed 签一份 payload，返回完整信封的 JSON 字节。
QByteArray signTestPayload(const QJsonObject &payload, const TestKeyPair &keys)
{
    const QByteArray signedBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    uint8_t seedBuf[32] = {};
    std::memcpy(seedBuf, keys.seed.constData(), 32);

    uint8_t secretKey[64] = {};
    uint8_t publicKey[32] = {};
    crypto_ed25519_key_pair(secretKey, publicKey, seedBuf);

    uint8_t signature[64] = {};
    crypto_ed25519_sign(signature, secretKey,
                        reinterpret_cast<const uint8_t *>(signedBytes.constData()),
                        static_cast<size_t>(signedBytes.size()));
    crypto_wipe(secretKey, sizeof(secretKey));
    crypto_wipe(seedBuf, sizeof(seedBuf));

    QJsonObject envelope;
    envelope.insert(QStringLiteral("schema"), QLatin1String(asset::kManifestSchema));
    envelope.insert(QStringLiteral("keyId"), QStringLiteral("studious-dev"));
    envelope.insert(QStringLiteral("payload"), payload);
    envelope.insert(QStringLiteral("sig"),
                    QString::fromLatin1(
                        QByteArray(reinterpret_cast<const char *>(signature), 64).toHex()));
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

// 64 位小写 hex 的假 sha256（只验证格式，不对应任何真实文件）。
QString fakeSha256(QChar fill)
{
    return QString(64, fill);
}

QJsonObject makeAsset(const QString &id, const QString &type, const QStringList &urls,
                      const QString &sha256, double size = 1024.0)
{
    QJsonArray urlArray;
    for (const QString &url : urls) {
        urlArray.append(url);
    }

    QJsonObject asset;
    asset.insert(QStringLiteral("id"), id);
    asset.insert(QStringLiteral("type"), type);
    asset.insert(QStringLiteral("op"), QStringLiteral("windows"));
    asset.insert(QStringLiteral("arch"), QStringLiteral("x64"));
    asset.insert(QStringLiteral("backend"), QStringLiteral("vulkan"));
    asset.insert(QStringLiteral("version"), QStringLiteral("b4200"));
    asset.insert(QStringLiteral("size"), size);
    asset.insert(QStringLiteral("sha256"), sha256);
    asset.insert(QStringLiteral("archive"), QStringLiteral("zip"));
    asset.insert(QStringLiteral("urls"), urlArray);
    return asset;
}

// 默认 payload：两条资产（一条 runtime + 一条 model），都用合法 https 地址 + 合法 sha256。
QJsonObject makeTestPayload(int schemaVersion = 1)
{
    QJsonArray assets;
    assets.append(makeAsset(QStringLiteral("runtime.llama.vulkan.win-x64"), QStringLiteral("runtime"),
                            {QStringLiteral("https://cdn.studious.app/runtime/llama-vulkan.zip")},
                            fakeSha256(QLatin1Char('a')), 262144000.0));
    assets.append(makeAsset(QStringLiteral("model.qwen2.5-3b-instruct.q4km"), QStringLiteral("model"),
                            {QStringLiteral("https://hf-mirror.com/x/qwen2.5-3b-q4_k_m.gguf")},
                            fakeSha256(QLatin1Char('b')), 1930000000.0));

    QJsonObject payload;
    payload.insert(QStringLiteral("schemaVersion"), schemaVersion);
    payload.insert(QStringLiteral("generatedAt"), QStringLiteral("2026-11-04T00:00:00Z"));
    payload.insert(QStringLiteral("channel"), QStringLiteral("stable"));
    payload.insert(QStringLiteral("assets"), assets);
    return payload;
}

asset::ManifestVerifyOptions manifestOptions(const TestKeyPair &keys, bool requireSignature = true)
{
    asset::ManifestVerifyOptions opts;
    opts.trustedPublicKey = keys.pub;
    opts.requireSignature = requireSignature;
    opts.maxSchemaVersion = 1;
    return opts;
}

void runManifestMode()
{
    // 本模式的断言检查的是中文 error 的子串（"签名" / "版本过旧" / "重复" …）。
    // 文案已经双语化，这里显式锁中文，保证在任何系统语言下结论一致。
    i18n::setLanguage(i18n::Language::Chinese);

    say(QStringLiteral("=== 资产清单 / ed25519 验签自测 ==="));

    const TestKeyPair keys = testKeyPair();
    Assertions keyAssert;
    keyAssert.expect(keys.seed.size() == 32, QStringLiteral("TEST seed 不是 32 字节"));
    keyAssert.expect(keys.pub.size() == 32, QStringLiteral("TEST 公钥不是 32 字节"));
    check(QStringLiteral("manifestTestKey"), keyAssert.ok(), keyAssert.details());
    say(QStringLiteral("        TEST pubkey = ") + asset::bytesToHex(keys.pub)
        + QStringLiteral("   ← 仅自测，不可用于发布"));

    // 编译进二进制的正式公钥必须是 32 字节，且不能还是全 0 占位。
    {
        const QByteArray trustKey = asset::hexToBytes(QString::fromLatin1(asset::kManifestPublicKeyHex));
        const bool filled = trustKey.size() == 32
                            && trustKey != QByteArray(32, '\0');
        check(QStringLiteral("manifestTrustKey"), filled,
              QStringLiteral("TrustKeys.h 的 kManifestPublicKeyHex 不是 32 字节有效 hex（或仍是全 0 占位）：%1")
                  .arg(QLatin1String(asset::kManifestPublicKeyHex)));
        say(QStringLiteral("        TrustKeys pubkey = ") + QLatin1String(asset::kManifestPublicKeyHex)
            + QStringLiteral("   (keyId=%1)").arg(QLatin1String(asset::kManifestKeyId)));
    }

    // 1) 合法签名
    {
        const QByteArray raw = signTestPayload(makeTestPayload(), keys);
        const asset::ManifestLoadResult r = asset::parseManifest(raw, manifestOptions(keys));

        Assertions a;
        a.expect(r.ok, QStringLiteral("ok=false，error=%1").arg(r.error));
        a.expect(r.manifest.assets.size() == 2,
                 QStringLiteral("资产条数=%1（期望 2）").arg(r.manifest.assets.size()));
        a.expect(r.manifest.schemaVersion == 1,
                 QStringLiteral("schemaVersion=%1（期望 1）").arg(r.manifest.schemaVersion));
        a.expect(r.manifest.channel == QStringLiteral("stable"),
                 QStringLiteral("channel=%1（期望 stable）").arg(r.manifest.channel));
        a.expect(r.manifest.keyId == QStringLiteral("studious-dev"),
                 QStringLiteral("keyId=%1").arg(r.manifest.keyId));
        if (const asset::AssetEntry *e = r.manifest.find(QStringLiteral("model.qwen2.5-3b-instruct.q4km"))) {
            a.expect(e->isModel(), QStringLiteral("model 条目 isModel()=false"));
            a.expect(e->size == 1930000000LL,
                     QStringLiteral("model size=%1（期望 1930000000）").arg(e->size));
        } else {
            a.expect(false, QStringLiteral("find() 找不到 model 条目"));
        }
        a.expect(r.manifest.ofType(QStringLiteral("runtime")).size() == 1,
                 QStringLiteral("ofType(runtime) 数量不对"));
        a.expect(r.manifest.forPlatform(QStringLiteral("windows"), QStringLiteral("x64")).size() == 2,
                 QStringLiteral("forPlatform(windows,x64) 数量不对"));
        check(QStringLiteral("manifestValid"), a.ok(), a.details());
    }

    // 2) 签名后篡改 payload（size 改大）→ 必须验签失败
    {
        QByteArray raw = signTestPayload(makeTestPayload(), keys);
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        QJsonObject envelope = doc.object();
        QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        QJsonArray assets = payload.value(QStringLiteral("assets")).toArray();
        QJsonObject first = assets.at(0).toObject();
        first.insert(QStringLiteral("size"), 999999999999.0);   // 篡改：体积改大
        assets.replace(0, first);
        payload.insert(QStringLiteral("assets"), assets);
        envelope.insert(QStringLiteral("payload"), payload);   // 不动 sig

        const QByteArray tampered = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        const asset::ManifestLoadResult r = asset::parseManifest(tampered, manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("篡改后竟然解析成功（验签形同虚设）"));
        a.expect(r.error.contains(QStringLiteral("签名")),
                 QStringLiteral("error 未提到签名：%1").arg(r.error));
        check(QStringLiteral("manifestTampered"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 3) 删掉 sig 且 requireSignature=true → 必须拒绝
    {
        QByteArray raw = signTestPayload(makeTestPayload(), keys);
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        QJsonObject envelope = doc.object();
        envelope.remove(QStringLiteral("sig"));
        const QByteArray unsignedRaw = QJsonDocument(envelope).toJson(QJsonDocument::Compact);

        const asset::ManifestLoadResult r = asset::parseManifest(unsignedRaw, manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("缺 sig 竟然解析成功"));
        a.expect(r.error.contains(QStringLiteral("签名")),
                 QStringLiteral("error 未提到签名：%1").arg(r.error));
        check(QStringLiteral("manifestMissingSig"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 4) schemaVersion=99 → 必须提示客户端版本过旧
    {
        const QByteArray raw = signTestPayload(makeTestPayload(99), keys);
        const asset::ManifestLoadResult r = asset::parseManifest(raw, manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("schemaVersion=99 竟然解析成功"));
        a.expect(r.error.contains(QStringLiteral("版本过旧")),
                 QStringLiteral("error 未提示客户端版本过旧：%1").arg(r.error));
        check(QStringLiteral("manifestBadSchema"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 5) 两条同 id → 必须拒绝
    {
        QJsonObject payload = makeTestPayload();
        QJsonArray assets = payload.value(QStringLiteral("assets")).toArray();
        assets.append(makeAsset(QStringLiteral("runtime.llama.vulkan.win-x64"),
                                QStringLiteral("runtime"),
                                {QStringLiteral("https://cdn.studious.app/runtime/dup.zip")},
                                fakeSha256(QLatin1Char('c'))));
        payload.insert(QStringLiteral("assets"), assets);

        const asset::ManifestLoadResult r =
            asset::parseManifest(signTestPayload(payload, keys), manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("重复 id 竟然解析成功"));
        a.expect(r.error.contains(QStringLiteral("重复")),
                 QStringLiteral("error 未提到 id 重复：%1").arg(r.error));
        check(QStringLiteral("manifestDupId"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 6) urls 里放明文 http → 必须拒绝
    {
        QJsonObject payload = makeTestPayload();
        QJsonArray assets = payload.value(QStringLiteral("assets")).toArray();
        assets.append(makeAsset(QStringLiteral("runtime.http.win-x64"), QStringLiteral("runtime"),
                                {QStringLiteral("http://insecure.example.com/evil.zip")},
                                fakeSha256(QLatin1Char('d'))));
        payload.insert(QStringLiteral("assets"), assets);

        const asset::ManifestLoadResult r =
            asset::parseManifest(signTestPayload(payload, keys), manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("明文 http 地址竟然解析成功"));
        a.expect(r.error.contains(QStringLiteral("https")),
                 QStringLiteral("error 未提到 https：%1").arg(r.error));
        check(QStringLiteral("manifestHttpUrl"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 7) 有 urls 但 sha256 不是 64 hex → 必须拒绝
    {
        QJsonObject payload = makeTestPayload();
        QJsonArray assets = payload.value(QStringLiteral("assets")).toArray();
        assets.append(makeAsset(QStringLiteral("runtime.bad.sha.win-x64"), QStringLiteral("runtime"),
                                {QStringLiteral("https://cdn.studious.app/runtime/bad.zip")},
                                QStringLiteral("abc")));
        payload.insert(QStringLiteral("assets"), assets);

        const asset::ManifestLoadResult r =
            asset::parseManifest(signTestPayload(payload, keys), manifestOptions(keys));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("sha256=\"abc\" 竟然解析成功"));
        a.expect(r.error.contains(QStringLiteral("sha256")),
                 QStringLiteral("error 未提到 sha256：%1").arg(r.error));
        check(QStringLiteral("manifestBadSha"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // 8) sha256File / verifyFileSha256（临时文件跨 1 MiB 块边界）
    {
        QTemporaryDir tmpDir(QDir::tempPath() + QStringLiteral("/studious_manifest_XXXXXX"));
        Assertions a;
        a.expect(tmpDir.isValid(), QStringLiteral("无法创建临时目录"));
        if (tmpDir.isValid()) {
            QByteArray blob;
            blob.resize(3 * 1024 * 1024 + 12345);   // 3 MiB 多一点：必然跨多个分块
            for (int i = 0; i < blob.size(); ++i) {
                blob[i] = static_cast<char>((i * 37 + 11) & 0xFF);
            }

            const QString path = tmpDir.path() + QStringLiteral("/blob.bin");
            QFile file(path);
            a.expect(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                     QStringLiteral("无法写入临时文件：%1").arg(file.errorString()));
            if (file.isOpen()) {
                file.write(blob);
                file.close();
            }

            const QString expect =
                QString::fromLatin1(QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toHex());

            QString fileError;
            const QString actual = asset::sha256File(path, &fileError);
            a.expect(actual == expect,
                     QStringLiteral("sha256File=%1 与期望 %2 不一致（%3）").arg(actual, expect, fileError));

            QString verifyError;
            a.expect(asset::verifyFileSha256(path, expect, &verifyError),
                     QStringLiteral("verifyFileSha256 应通过，却失败：%1").arg(verifyError));
            check(QStringLiteral("sha256FileOk"), a.ok(), a.details());
            say(QStringLiteral("        sha256 = ") + actual);

            // 改一个字节 → 必须失败
            Assertions t;
            QFile edit(path);
            if (edit.open(QIODevice::ReadWrite)) {
                edit.seek(12345);
                char byte = 0;
                edit.getChar(&byte);
                byte = static_cast<char>(byte ^ 0x01);
                edit.seek(12345);
                edit.putChar(byte);
                edit.close();
            } else {
                t.expect(false, QStringLiteral("无法改写临时文件：%1").arg(edit.errorString()));
            }

            QString tamperedError;
            const bool pass = asset::verifyFileSha256(path, expect, &tamperedError);
            t.expect(!pass, QStringLiteral("改了一个字节后 verifyFileSha256 竟然还通过"));
            t.expect(!tamperedError.isEmpty(),
                     QStringLiteral("失败但 error 为空，调用方无法提示"));
            check(QStringLiteral("sha256FileTampered"), t.ok(), t.details());
            say(QStringLiteral("        error = ") + tamperedError);
        } else {
            check(QStringLiteral("sha256FileOk"), false, a.details());
        }
    }
}

// ---------- 4.6 错误文案双语（--i18n） ----------

// 样例文案全部来自**真实模块**，不是自造的字符串：
// 哪个模块被改回单语，这里立刻红。
QStringList collectLocalizedSamples()
{
    QStringList out;

    // 1) LlmTranslator::resolve()：未配置 LLM API
    {
        translate::LlmTranslator llm;   // 空配置 = 未就绪
        out << llm.resolve(QStringLiteral("auto"), QStringLiteral("zh")).reason;
    }

    // 2) llm::listModels()：Base URL 为空（不联网）
    {
        studious::net::QtHttpClient http;
        out << llm::listModels(config::LlmConfig{}, http).error;
    }

    // 3) Router：指定的引擎不存在
    {
        studious::net::QtHttpClient http;
        router::TranslatorRouter r(buildAllEngines(http));

        core::TranslateRequest req;
        req.paragraphs = {QStringLiteral("Hello")};
        req.dstLangs = {QStringLiteral("zh")};
        req.preferredEngine = QStringLiteral("no-such-engine");
        out << r.translate(req).error;
    }

    // 4) AssetRegistry：清单里没有该资产
    {
        asset::AssetRegistry reg(QDir::tempPath() + QStringLiteral("/studious_i18n_root"),
                                 asset::Manifest{});
        out << reg.status(QStringLiteral("no.such.asset")).detail;
    }

    // 5) ManifestParser：验签失败
    {
        const TestKeyPair keys = testKeyPair();
        QJsonObject envelope = QJsonDocument::fromJson(signTestPayload(makeTestPayload(), keys)).object();
        QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        payload.insert(QStringLiteral("channel"), QStringLiteral("tampered"));   // 改 payload 不动 sig
        envelope.insert(QStringLiteral("payload"), payload);
        out << asset::parseManifest(QJsonDocument(envelope).toJson(QJsonDocument::Compact),
                                    manifestOptions(keys))
                   .error;
    }

    return out;
}

void runI18nMode()
{
    say(QStringLiteral("=== 错误文案中英双语自测（--i18n） ==="));

    // 默认判定只看系统语言，这里把判定结果打出来（不覆盖时生效的那个）。
    i18n::resetToSystemLanguage();
    say(QStringLiteral("系统语言判定 = %1（由 QLocale::system() 推出，不读任何配置）")
            .arg(i18n::current() == i18n::Language::Chinese ? QStringLiteral("中文")
                                                            : QStringLiteral("英文")));

    i18n::setLanguage(i18n::Language::Chinese);
    const QStringList zh = collectLocalizedSamples();
    i18n::setLanguage(i18n::Language::English);
    const QStringList en = collectLocalizedSamples();
    i18n::resetToSystemLanguage();   // 别把覆盖状态漏给别的模式

    say(QString());
    for (int i = 0; i < zh.size() && i < en.size(); ++i) {
        say(QStringLiteral("  [%1] zh: %2").arg(i + 1).arg(zh.at(i)));
        say(QStringLiteral("  [%1] en: %2").arg(i + 1).arg(en.at(i)));
    }

    Assertions a;
    a.expect(zh.size() == en.size(),
             QStringLiteral("中英样例条数不一致：%1 vs %2").arg(zh.size()).arg(en.size()));
    a.expect(zh.size() >= 3, QStringLiteral("样例只有 %1 条（至少要 3 条）").arg(zh.size()));
    for (int i = 0; i < zh.size() && i < en.size(); ++i) {
        a.expect(!zh.at(i).trimmed().isEmpty(), QStringLiteral("第 %1 条中文为空").arg(i + 1));
        a.expect(!en.at(i).trimmed().isEmpty(), QStringLiteral("第 %1 条英文为空").arg(i + 1));
        a.expect(zh.at(i) != en.at(i),
                 QStringLiteral("第 %1 条中英文本完全相同：%2").arg(i + 1).arg(zh.at(i)));
    }

    check(QStringLiteral("i18n-bilingual"), a.ok(), a.details());
    say(QStringLiteral("        共比对 %1 条文案（LLM / 模型目录 / Router / 资产 / 清单验签）")
            .arg(zh.size()));
}

// ---------- 4.7 资产下载 / 安装 / 卸载（--assets） ----------

// 本地最小 HTTP 服务器：只服务确定性用例，绝不依赖外网。
//
// 支持 Range：请求带 "Range: bytes=N-" 时返回 206 并记一笔，
// assetsResume 用它断言"客户端真的发了 Range"而不是整份重下。
class TestHttpServer {
public:
    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket]() { onReadyRead(socket); });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
        return true;
    }

    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

    void route(const QString &path, const QByteArray &body) { m_routes.insert(path.toUtf8(), body); }

    int rangeRequests() const { return m_rangeRequests; }
    void resetCounters() { m_rangeRequests = 0; }

private:
    void onReadyRead(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;   // 请求头还没收全
        }
        const QByteArray head = buffer.left(headerEnd);
        buffer.remove(0, headerEnd + 4);
        handle(socket, head);
    }

    void handle(QTcpSocket *socket, const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> requestLine =
            lines.isEmpty() ? QList<QByteArray>() : lines.first().trimmed().split(' ');
        if (requestLine.size() < 2) {
            writeRaw(socket, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        const QByteArray path = requestLine.at(1);

        QByteArray range;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            if (line.left(colon).trimmed().toLower() == QByteArrayLiteral("range")) {
                range = line.mid(colon + 1).trimmed();
            }
        }

        if (!m_routes.contains(path)) {
            writeRaw(socket,
                     "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found");
            return;
        }

        const QByteArray body = m_routes.value(path);
        qint64 offset = 0;
        if (range.startsWith("bytes=")) {
            ++m_rangeRequests;
            QByteArray spec = range.mid(6);
            const int dash = spec.indexOf('-');
            if (dash > 0) {
                spec = spec.left(dash);
            }
            offset = spec.trimmed().toLongLong();
            if (offset < 0) {
                offset = 0;
            }
        }

        if (offset >= body.size()) {
            // .part 已经和整份文件一样长：告诉客户端"这个 Range 没法满足"，客户端据此直接去校验。
            QByteArray response = QByteArrayLiteral("HTTP/1.1 416 Range Not Satisfiable\r\n");
            response += "Content-Range: bytes */" + QByteArray::number(body.size()) + "\r\n";
            response += QByteArrayLiteral("Content-Length: 0\r\nConnection: close\r\n\r\n");
            writeRaw(socket, response);
            return;
        }

        const QByteArray chunk = body.mid(offset);
        QByteArray response = offset > 0 ? QByteArrayLiteral("HTTP/1.1 206 Partial Content\r\n")
                                         : QByteArrayLiteral("HTTP/1.1 200 OK\r\n");
        response += QByteArrayLiteral("Content-Type: application/octet-stream\r\n");
        response += QByteArrayLiteral("Accept-Ranges: bytes\r\n");
        response += "Content-Length: " + QByteArray::number(chunk.size()) + "\r\n";
        if (offset > 0) {
            response += "Content-Range: bytes " + QByteArray::number(offset) + "-"
                        + QByteArray::number(body.size() - 1) + "/"
                        + QByteArray::number(body.size()) + "\r\n";
        }
        response += QByteArrayLiteral("Connection: close\r\n\r\n");
        writeRaw(socket, response + chunk);
    }

    static void writeRaw(QTcpSocket *socket, const QByteArray &bytes)
    {
        socket->write(bytes);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QByteArray, QByteArray> m_routes;
    int m_rangeRequests = 0;
};

// 512 KiB 确定性伪随机内容：写死了生成算法，每次跑出来字节完全一样。
QByteArray makeAssetPayload()
{
    QByteArray blob;
    blob.resize(512 * 1024);
    quint32 state = 0x12345678u;
    for (int i = 0; i < blob.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        blob[i] = static_cast<char>((state >> 16) & 0xFF);
    }
    return blob;
}

QString sha256Of(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// 用 miniz 在内存里造 zip。条目名可以是恶意的（"../evil.txt"），用于构造 zip-slip 用例。
QByteArray buildZip(const QVector<QPair<QString, QByteArray>> &entries, QString *error)
{
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        *error = QStringLiteral("mz_zip_writer_init_heap 失败");
        return QByteArray();
    }
    for (const QPair<QString, QByteArray> &entry : entries) {
        const QByteArray name = entry.first.toUtf8();
        if (!mz_zip_writer_add_mem(&zip, name.constData(), entry.second.constData(),
                                   static_cast<size_t>(entry.second.size()),
                                   MZ_BEST_COMPRESSION)) {
            *error = QStringLiteral("mz_zip_writer_add_mem 失败：%1").arg(entry.first);
            mz_zip_writer_end(&zip);
            return QByteArray();
        }
    }

    void *buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) {
        *error = QStringLiteral("mz_zip_writer_finalize_heap_archive 失败");
        mz_zip_writer_end(&zip);
        return QByteArray();
    }
    const QByteArray out(static_cast<const char *>(buffer), static_cast<qsizetype>(size));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

asset::AssetEntry makeTestEntry(const QString &id,
                                const QStringList &urls,
                                const QString &sha256,
                                const QString &archive = QString(),
                                const QStringList &files = QStringList())
{
    asset::AssetEntry entry;
    entry.id = id;
    entry.type = QStringLiteral("model");   // 落到 <root>/models/<id>
    entry.urls = urls;
    entry.sha256 = sha256;
    entry.archive = archive;
    entry.files = files;
    return entry;
}

void runAssetsMode()
{
    // 本模式的断言逐条检查中文 error 的关键词，锁中文保证任何系统语言下结论一致。
    i18n::setLanguage(i18n::Language::Chinese);

    say(QStringLiteral("=== 资产下载 / 安装 / 卸载自测（--assets） ==="));

    QTemporaryDir tmpDir(QDir::tempPath() + QStringLiteral("/studious_assets_XXXXXX"));
    if (!tmpDir.isValid()) {
        check(QStringLiteral("assetsSetup"), false, QStringLiteral("无法创建临时目录"));
        return;
    }
    const QString root = tmpDir.path() + QStringLiteral("/root");

    const QByteArray payload = makeAssetPayload();
    const QByteArray wrongPayload =
        QByteArray("this is definitely not the asset you asked for").repeated(64);
    const QByteArray innerFile = QByteArray("hello from inside the zip");
    const QByteArray extraFile = QByteArray("not listed as a required file");

    QString zipError;
    const QByteArray zipBytes =
        buildZip({{QStringLiteral("inner.txt"), innerFile},
                  {QStringLiteral("readme.txt"), extraFile}},
                 &zipError);
    const QByteArray slipZipBytes =
        buildZip({{QStringLiteral("../evil.txt"), QByteArray("pwned")}}, &zipError);
    {
        Assertions a;
        a.expect(!zipBytes.isEmpty(), QStringLiteral("造 zip 失败：%1").arg(zipError));
        a.expect(!slipZipBytes.isEmpty(), QStringLiteral("造恶意 zip 失败：%1").arg(zipError));
        check(QStringLiteral("assetsFixtures"), a.ok(), a.details());
        if (zipBytes.isEmpty() || slipZipBytes.isEmpty()) {
            return;
        }
    }

    TestHttpServer server;
    if (!server.start()) {
        check(QStringLiteral("assetsServer"), false,
              QStringLiteral("无法在 127.0.0.1 上随机端口监听"));
        return;
    }
    server.route(QStringLiteral("/file"), payload);
    server.route(QStringLiteral("/bad"), wrongPayload);
    server.route(QStringLiteral("/zip"), zipBytes);
    server.route(QStringLiteral("/slip"), slipZipBytes);
    // 其余路径一律 404。
    say(QStringLiteral("本地测试服务器: ") + server.url(QStringLiteral("/file")));

    const QString goodSha = sha256Of(payload);
    const QString badSha = sha256Of(QByteArray("some other bytes entirely"));

    asset::Manifest manifest;
    manifest.schemaVersion = 1;
    manifest.channel = QStringLiteral("stable");
    manifest.keyId = QStringLiteral("studious-test");
    manifest.assets.append(
        makeTestEntry(QStringLiteral("test.good"), {server.url(QStringLiteral("/file"))}, goodSha));
    manifest.assets.append(
        makeTestEntry(QStringLiteral("test.bad"), {server.url(QStringLiteral("/bad"))}, badSha));
    manifest.assets.append(makeTestEntry(QStringLiteral("test.zip"),
                                         {server.url(QStringLiteral("/zip"))}, sha256Of(zipBytes),
                                         QStringLiteral("zip"), {QStringLiteral("inner.txt")}));
    manifest.assets.append(makeTestEntry(QStringLiteral("test.slip"),
                                         {server.url(QStringLiteral("/slip"))},
                                         sha256Of(slipZipBytes), QStringLiteral("zip"),
                                         {QStringLiteral("evil.txt")}));
    manifest.assets.append(
        makeTestEntry(QStringLiteral("test.resume"), {server.url(QStringLiteral("/file"))}, goodSha));
    manifest.assets.append(
        makeTestEntry(QStringLiteral("test.cancel"), {server.url(QStringLiteral("/file"))}, goodSha));
    manifest.assets.append(
        makeTestEntry(QStringLiteral("test.import"), {server.url(QStringLiteral("/file"))}, goodSha));
    manifest.assets.append(makeTestEntry(
        QStringLiteral("test.failover"),
        {QStringLiteral("http://127.0.0.1:1/file"), server.url(QStringLiteral("/file"))}, goodSha));

    asset::AssetRegistry registry(root, manifest);
    registry.refresh();
    studious::net::QtHttpClient http;
    asset::Downloader downloader(http, registry);

    // ---- 1) 正常下载：落盘 + 大小 + 哈希 + .part 清理 + 进度 ----
    {
        qint64 lastReceived = -1;
        qint64 lastTotal = -1;
        int lastIndex = -1;
        int lastCount = -1;
        int callbacks = 0;

        const asset::DownloadResult r = downloader.download(
            QStringLiteral("test.good"),
            [&](const asset::DownloadProgress &p) {
                ++callbacks;
                lastReceived = p.received;
                lastTotal = p.total;
                lastIndex = p.sourceIndex;
                lastCount = p.sourceCount;
            },
            nullptr);

        const QString target = registry.targetPath(QStringLiteral("test.good"));
        Assertions a;
        a.expect(r.ok, QStringLiteral("下载失败：%1").arg(r.error));
        a.expect(r.installedPath == target,
                 QStringLiteral("installedPath=%1（期望 %2）").arg(r.installedPath, target));
        a.expect(QFileInfo(target).isFile(), QStringLiteral("目标不是文件：%1").arg(target));
        a.expect(QFileInfo(target).size() == payload.size(),
                 QStringLiteral("大小=%1（期望 %2）").arg(QFileInfo(target).size()).arg(payload.size()));
        QString hashError;
        a.expect(asset::sha256File(target, &hashError) == goodSha,
                 QStringLiteral("落盘文件 sha256 不符：%1").arg(hashError));
        a.expect(!QFileInfo::exists(registry.partialPath(QStringLiteral("test.good"))),
                 QStringLiteral("成功后 .part 没清理"));
        a.expect(callbacks > 0, QStringLiteral("进度回调一次都没触发"));
        a.expect(lastReceived == payload.size(),
                 QStringLiteral("最终 received=%1（期望 %2）").arg(lastReceived).arg(payload.size()));
        a.expect(lastTotal == payload.size(),
                 QStringLiteral("最终 total=%1（期望 %2）").arg(lastTotal).arg(payload.size()));
        a.expect(lastIndex == 0 && lastCount == 1,
                 QStringLiteral("sourceIndex/Count=%1/%2（期望 0/1）").arg(lastIndex).arg(lastCount));
        a.expect(registry.status(QStringLiteral("test.good")).state == asset::AssetState::Installed,
                 QStringLiteral("下载完状态不是 Installed"));
        check(QStringLiteral("assetsDownloadOk"), a.ok(), a.details());
        say(QStringLiteral("        %1 字节，%2 次进度回调，sha256=%3")
                .arg(QFileInfo(target).size())
                .arg(callbacks)
                .arg(goodSha.left(16) + QStringLiteral("…")));
    }

    // ---- 2) 哈希不匹配：必须失败、必须清掉 .part、绝不能装上 ----
    {
        const asset::DownloadResult r = downloader.download(QStringLiteral("test.bad"), nullptr, nullptr);

        Assertions a;
        a.expect(!r.ok, QStringLiteral("哈希不匹配竟然返回成功"));
        a.expect(r.error.contains(QStringLiteral("sha256")),
                 QStringLiteral("error 未提到哈希不匹配：%1").arg(r.error));
        a.expect(!QFileInfo::exists(registry.partialPath(QStringLiteral("test.bad"))),
                 QStringLiteral("校验失败后 .part 未清理"));
        a.expect(!QFileInfo::exists(registry.targetPath(QStringLiteral("test.bad"))),
                 QStringLiteral("坏包竟然被安装了"));
        check(QStringLiteral("assetsShaMismatch"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // ---- 3) 断点续传：先放半截 .part，下载必须发 Range 且最终内容正确 ----
    {
        const QString partPath = registry.partialPath(QStringLiteral("test.resume"));
        QDir().mkpath(QFileInfo(partPath).absolutePath());
        const qint64 half = payload.size() / 2;

        QFile part(partPath);
        bool preWritten = part.open(QIODevice::WriteOnly);
        if (preWritten) {
            preWritten = part.write(payload.left(half)) == half;
            part.close();
        }

        server.resetCounters();
        const asset::DownloadResult r =
            downloader.download(QStringLiteral("test.resume"), nullptr, nullptr);

        Assertions a;
        a.expect(preWritten, QStringLiteral("无法预写半截 .part"));
        a.expect(r.ok, QStringLiteral("续传失败：%1").arg(r.error));
        a.expect(server.rangeRequests() >= 1,
                 QStringLiteral("服务器没收到 Range 请求（说明根本没走续传）"));
        QString hashError;
        a.expect(asset::sha256File(registry.targetPath(QStringLiteral("test.resume")), &hashError)
                     == goodSha,
                 QStringLiteral("续传后文件内容不对：%1").arg(hashError));
        check(QStringLiteral("assetsResume"), a.ok(), a.details());
        say(QStringLiteral("        预置 %1 字节，服务器收到 %2 次 Range 请求")
                .arg(half)
                .arg(server.rangeRequests()));
    }

    // ---- 4) 多源 failover：第一个源是死端口，必须自动换到第二个 ----
    {
        int lastIndex = -1;
        const asset::DownloadResult r = downloader.download(
            QStringLiteral("test.failover"),
            [&](const asset::DownloadProgress &p) { lastIndex = p.sourceIndex; }, nullptr);

        Assertions a;
        a.expect(r.ok, QStringLiteral("failover 失败：%1").arg(r.error));
        a.expect(lastIndex > 0,
                 QStringLiteral("最后的 sourceIndex=%1（期望 >0：说明确实换了源）").arg(lastIndex));
        QString hashError;
        a.expect(asset::sha256File(registry.targetPath(QStringLiteral("test.failover")), &hashError)
                     == goodSha,
                 QStringLiteral("failover 后文件内容不对：%1").arg(hashError));
        check(QStringLiteral("assetsFailover"), a.ok(), a.details());
        say(QStringLiteral("        最终使用的源下标 = %1").arg(lastIndex));
    }

    // ---- 5) 取消：返回已取消、.part 保留、目标没生成 ----
    {
        const QString partPath = registry.partialPath(QStringLiteral("test.cancel"));
        QDir().mkpath(QFileInfo(partPath).absolutePath());
        QFile part(partPath);
        if (part.open(QIODevice::WriteOnly)) {
            part.write(payload.left(1024));
            part.close();
        }

        std::atomic_bool cancel{true};
        const asset::DownloadResult r =
            downloader.download(QStringLiteral("test.cancel"), nullptr, &cancel);

        Assertions a;
        a.expect(!r.ok, QStringLiteral("置了取消标志竟然下载成功"));
        a.expect(r.error.contains(QStringLiteral("取消")),
                 QStringLiteral("error 未说明已取消：%1").arg(r.error));
        a.expect(QFileInfo::exists(partPath),
                 QStringLiteral("取消后 .part 被删了（续传点丢了）"));
        a.expect(!QFileInfo::exists(registry.targetPath(QStringLiteral("test.cancel"))),
                 QStringLiteral("取消后竟然装上了"));
        check(QStringLiteral("assetsCancel"), a.ok(), a.details());
    }

    // ---- 6) zip 资产：解压安装 + 原子改名，临时目录不留 ----
    {
        const asset::DownloadResult r = downloader.download(QStringLiteral("test.zip"), nullptr, nullptr);
        const QString target = registry.targetPath(QStringLiteral("test.zip"));

        Assertions a;
        a.expect(r.ok, QStringLiteral("zip 资产下载失败：%1").arg(r.error));
        a.expect(QFileInfo(target).isDir(), QStringLiteral("zip 资产应解成目录：%1").arg(target));
        const QString innerPath = target + QStringLiteral("/inner.txt");
        a.expect(QFileInfo::exists(innerPath), QStringLiteral("缺少解压出的 inner.txt"));

        QFile inner(innerPath);
        QByteArray got;
        if (inner.open(QIODevice::ReadOnly)) {
            got = inner.readAll();
            inner.close();
        }
        a.expect(got == innerFile, QStringLiteral("inner.txt 内容不对：\"%1\"").arg(QString::fromUtf8(got)));
        a.expect(QFileInfo::exists(target + QStringLiteral("/readme.txt")),
                 QStringLiteral("多文件 zip 只解出了第一个条目"));
        a.expect(!QFileInfo::exists(target + QStringLiteral(".tmp")),
                 QStringLiteral("临时解压目录 <target>.tmp 没清掉"));
        a.expect(registry.status(QStringLiteral("test.zip")).state == asset::AssetState::Installed,
                 QStringLiteral("解压完状态不是 Installed"));
        check(QStringLiteral("assetsZipInstall"), a.ok(), a.details());
    }

    // ---- 7) 关键文件被删 → Corrupted ----
    {
        QFile::remove(registry.targetPath(QStringLiteral("test.zip")) + QStringLiteral("/inner.txt"));
        registry.refresh();

        const asset::AssetStatus st = registry.status(QStringLiteral("test.zip"));
        Assertions a;
        a.expect(st.state == asset::AssetState::Corrupted,
                 QStringLiteral("删了关键文件仍是 Installed/NotInstalled"));
        a.expect(!st.detail.isEmpty(), QStringLiteral("Corrupted 但 detail 为空（UI 没法提示）"));
        a.expect(st.detail.contains(QStringLiteral("inner.txt")),
                 QStringLiteral("detail 未点名缺哪个文件：%1").arg(st.detail));
        check(QStringLiteral("assetsCorrupted"), a.ok(), a.details());
        say(QStringLiteral("        detail = ") + st.detail);
    }

    // ---- 8) 卸载：幂等 ----
    {
        const QString target = registry.targetPath(QStringLiteral("test.zip"));
        QString error;
        const bool first = registry.uninstall(QStringLiteral("test.zip"), &error);

        Assertions a;
        a.expect(first, QStringLiteral("卸载失败：%1").arg(error));
        a.expect(!QFileInfo::exists(target), QStringLiteral("卸载后目录还在：%1").arg(target));
        a.expect(registry.status(QStringLiteral("test.zip")).state == asset::AssetState::NotInstalled,
                 QStringLiteral("卸载后状态不是 NotInstalled"));

        QString secondError;
        a.expect(registry.uninstall(QStringLiteral("test.zip"), &secondError),
                 QStringLiteral("重复卸载应幂等返回 true：%1").arg(secondError));
        check(QStringLiteral("assetsUninstall"), a.ok(), a.details());
    }

    // ---- 9) zip-slip：恶意条目必须整体拒绝，且没有任何文件逃出目标目录 ----
    {
        const asset::DownloadResult r = downloader.download(QStringLiteral("test.slip"), nullptr, nullptr);
        const QString target = registry.targetPath(QStringLiteral("test.slip"));

        Assertions a;
        a.expect(!r.ok, QStringLiteral("含 \"..\" 的 zip 竟然装上了"));
        a.expect(r.error.contains(QStringLiteral("..")) || r.error.contains(QStringLiteral("zip-slip")),
                 QStringLiteral("error 未说明是路径穿越：%1").arg(r.error));
        a.expect(!QFileInfo::exists(root + QStringLiteral("/models/evil.txt")),
                 QStringLiteral("文件逃出了目标目录（zip-slip 成功！）"));
        a.expect(!QFileInfo::exists(root + QStringLiteral("/evil.txt")),
                 QStringLiteral("文件逃到了安装根目录（zip-slip 成功！）"));
        a.expect(!QFileInfo::exists(target), QStringLiteral("失败后目标目录仍然存在"));
        a.expect(!QFileInfo::exists(target + QStringLiteral(".tmp")),
                 QStringLiteral("失败后临时解压目录没清掉"));
        check(QStringLiteral("assetsZipSlip"), a.ok(), a.details());
        say(QStringLiteral("        error = ") + r.error);
    }

    // ---- 10) 手动导入：内容对就装、内容不对就拒 ----
    {
        const QString goodFile = tmpDir.path() + QStringLiteral("/manual.good");
        QFile good(goodFile);
        bool wroteGood = good.open(QIODevice::WriteOnly);
        if (wroteGood) {
            wroteGood = good.write(payload) == payload.size();
            good.close();
        }

        const QString badFile = tmpDir.path() + QStringLiteral("/manual.bad");
        QFile bad(badFile);
        if (bad.open(QIODevice::WriteOnly)) {
            bad.write("nope");
            bad.close();
        }

        const asset::DownloadResult goodResult =
            downloader.installFromFile(QStringLiteral("test.import"), goodFile);
        const asset::DownloadResult badResult =
            downloader.installFromFile(QStringLiteral("test.import"), badFile);

        Assertions a;
        a.expect(wroteGood, QStringLiteral("无法写手动导入文件"));
        a.expect(goodResult.ok, QStringLiteral("手动导入失败：%1").arg(goodResult.error));
        a.expect(QFileInfo::exists(goodFile), QStringLiteral("手动导入不该搬走用户的原文件"));
        QString hashError;
        a.expect(asset::sha256File(registry.targetPath(QStringLiteral("test.import")), &hashError)
                     == goodSha,
                 QStringLiteral("导入后文件内容不对：%1").arg(hashError));
        a.expect(!badResult.ok, QStringLiteral("哈希不符的导入文件竟然被接受"));
        a.expect(badResult.error.contains(QStringLiteral("哈希")) || badResult.error.contains(QStringLiteral("sha256")),
                 QStringLiteral("导入失败的 error 未提哈希：%1").arg(badResult.error));
        check(QStringLiteral("assetsInstallFromFile"), a.ok(), a.details());
    }

    // ---- 11) 状态汇总 + 中英双语 ----
    {
        Assertions a;
        a.expect(registry.totalInstalledBytes() > 0, QStringLiteral("totalInstalledBytes()==0"));
        const QVector<asset::AssetStatus> all = registry.listStatus();
        a.expect(all.size() == manifest.assets.size(),
                 QStringLiteral("listStatus 条数=%1（期望 %2）").arg(all.size()).arg(manifest.assets.size()));
        check(QStringLiteral("assetsStatusList"), a.ok(), a.details());
        say(QStringLiteral("        已安装总字节 = %1").arg(registry.totalInstalledBytes()));
        for (const asset::AssetStatus &st : all) {
            say(QStringLiteral("        %1 | %2 | %3 字节")
                    .arg(pad(st.id, 28),
                         pad(st.state == asset::AssetState::Installed
                                 ? QStringLiteral("已安装")
                                 : (st.state == asset::AssetState::Corrupted ? QStringLiteral("损坏")
                                                                             : QStringLiteral("未安装")),
                             10))
                    .arg(st.installedBytes));
        }
    }
    {
        i18n::setLanguage(i18n::Language::Chinese);
        const QString zh = downloader.download(QStringLiteral("no.such.asset"), nullptr, nullptr).error;
        i18n::setLanguage(i18n::Language::English);
        const QString en = downloader.download(QStringLiteral("no.such.asset"), nullptr, nullptr).error;
        i18n::setLanguage(i18n::Language::Chinese);

        Assertions a;
        a.expect(!zh.isEmpty() && !en.isEmpty(), QStringLiteral("中英案例有空的"));
        a.expect(zh != en, QStringLiteral("中英文案相同：%1").arg(zh));
        check(QStringLiteral("assetsI18n"), a.ok(), a.details());
        say(QStringLiteral("        zh = ") + zh);
        say(QStringLiteral("        en = ") + en);
    }
}

// ---------- 5. Web 来源注册表 ----------
void runProvidersMode(studious::net::QtHttpClient &http)
{
    const QVector<translate::WebProviderInfo> all = translate::listWebProviders();

    say(QStringLiteral("=== Web 翻译来源（listWebProviders） ==="));
    say(pad(QStringLiteral("id"), 10) + QLatin1Char('|') + pad(QStringLiteral("名称"), 26)
        + QLatin1Char('|') + pad(QStringLiteral("需key"), 8) + QLatin1Char('|')
        + pad(QStringLiteral("已配置"), 10) + QLatin1Char('|') + QStringLiteral("备注"));
    for (const translate::WebProviderInfo &info : all) {
        say(pad(info.id, 10) + QLatin1Char('|') + pad(info.displayName, 26) + QLatin1Char('|')
            + pad(info.requiresApiKey ? QStringLiteral("是") : QStringLiteral("否"), 8)
            + QLatin1Char('|')
            + pad(info.configured ? QStringLiteral("是") : QStringLiteral("否"), 10)
            + QLatin1Char('|') + info.note);
    }

    Assertions a;
    a.expect(all.size() == 3,
             QStringLiteral("来源数量=%1（期望 google/azure/mymemory 三个）").arg(all.size()));
    a.expect(all.size() >= 1 && all.first().id == QStringLiteral("google"),
             QStringLiteral("第一个来源不是 google（默认优先级应以 google 打头）"));

    // 附加：校验真实装配链路 —— 按 "azure,google" 顺序装配，期望 azure 在前且未配置。
    auto adapters = translate::makeWebAdapters({QStringLiteral("azure"), QStringLiteral("google"),
                                               QStringLiteral("不存在的来源")});
    a.expect(adapters.size() == 2,
             QStringLiteral("makeWebAdapters 装配数量=%1（未知 id 应被忽略）").arg(adapters.size()));

    translate::WebTranslator web(http, adapters);
    const QVector<translate::WebProviderInfo> active = web.providers();
    a.expect(active.size() == 2,
             QStringLiteral("providers() 数量=%1（期望 2）").arg(active.size()));
    if (active.size() == 2) {
        a.expect(active.at(0).id == QStringLiteral("azure"),
                 QStringLiteral("providers()[0].id=%1（期望 azure）").arg(active.at(0).id));
        a.expect(active.at(0).requiresApiKey,
                 QStringLiteral("azure 应声明需要 API key"));
        a.expect(!active.at(0).configured,
                 QStringLiteral("azure v1 占位应报告未配置"));
        a.expect(active.at(1).id == QStringLiteral("google"),
                 QStringLiteral("providers()[1].id=%1（期望 google）").arg(active.at(1).id));
        a.expect(active.at(1).configured,
                 QStringLiteral("google 无需 key，应报告已配置"));
    }

    say(QString());
    say(QStringLiteral("[INFO] 实际装配顺序: azure -> google（未知 id 已忽略）"));
    for (const translate::WebProviderInfo &info : active) {
        say(QStringLiteral("        %1 | %2 | 需key=%3 | 已配置=%4")
                .arg(info.id, info.displayName,
                     info.requiresApiKey ? QStringLiteral("是") : QStringLiteral("否"),
                     info.configured ? QStringLiteral("是") : QStringLiteral("否")));
    }

    // 空列表 = 默认顺序：google -> mymemory。
    auto defaults = translate::makeWebAdapters({});
    a.expect(defaults.size() == 2, QStringLiteral("默认装配数量=%1（期望 2）").arg(defaults.size()));
    if (defaults.size() == 2) {
        a.expect(defaults.at(0)->id() == QStringLiteral("google")
                     && defaults.at(1)->id() == QStringLiteral("mymemory"),
                 QStringLiteral("默认顺序应是 google -> mymemory，实际 %1 -> %2")
                     .arg(defaults.at(0)->id(), defaults.at(1)->id()));
    }

    // 降级实测（联网才有结论，故只 [INFO] 不参与断言）：
    // azure 在最前但未配置 -> 应被跳过并继续用 google，而不是整体失败。
    const core::TranslateOutcome degrade = web.translate({QStringLiteral("Hello, world!")},
                                                         QStringLiteral("auto"),
                                                         {QStringLiteral("zh")},
                                                         {});
    if (degrade.ok) {
        say(QStringLiteral("[INFO] azure->google 降级实测: ") +
            degrade.byLang.value(QStringLiteral("zh")).value(0));
    } else {
        say(QStringLiteral("[INFO] azure->google 降级实测失败（网络问题不算用例失败）: ") +
            degrade.error);
    }

    check(QStringLiteral("providers"), a.ok(), a.details());
}

// ---------- 6. LLM 真实接入 ----------
// 失败时把原始 HTTP 响应打出来：网关/模型名/额度问题只能靠原始字节定位，不能瞎猜。
void dumpRawLlmResponse(const config::LlmConfig &cfg, studious::net::QtHttpClient &http)
{
    QJsonObject userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), QStringLiteral("Hello, world!"));

    QJsonArray messages;
    messages.append(userMessage);

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), cfg.model);
    payload.insert(QStringLiteral("messages"), messages);
    payload.insert(QStringLiteral("max_tokens"), 64);
    payload.insert(QStringLiteral("stream"), false);

    net::HttpRequest req;
    req.url = QUrl(cfg.baseUrl + QStringLiteral("/chat/completions"));
    req.method = QByteArrayLiteral("POST");
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.timeoutMs = 30000;
    req.headers.append(qMakePair(QByteArrayLiteral("Content-Type"),
                                 QByteArrayLiteral("application/json")));
    if (!cfg.apiKey.isEmpty()) {
        req.headers.append(qMakePair(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + cfg.apiKey.toUtf8()));
    }

    const net::HttpResponse resp = http.send(req);
    say(QStringLiteral("[DEBUG] 原始请求: POST %1  model=%2")
            .arg(req.url.toString(), cfg.model));
    say(QStringLiteral("[DEBUG] 原始响应: status=%1 ok=%2 error=%3")
            .arg(resp.status)
            .arg(resp.ok ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(resp.error));
    say(QStringLiteral("[DEBUG] 响应体前 800 字节: ") + QString::fromUtf8(resp.body.left(800)));
}

// 各专项模式共用的默认配置（本机网关），允许环境变量覆盖以便排查：
//   STUDIOUS_LLM_BASE_URL / STUDIOUS_LLM_API_KEY / STUDIOUS_LLM_MODEL
static config::LlmConfig defaultLlmConfig()
{
    config::LlmConfig cfg;
    cfg.baseUrl = QStringLiteral("http://127.0.0.1:3065/v1");
    cfg.apiKey = QStringLiteral("12345678");
    cfg.model = QStringLiteral("deepseek-v4.1-flash");

    const auto envOr = [](const char *name, const QString &fallback) {
        const QString v = qEnvironmentVariable(name).trimmed();
        return v.isEmpty() ? fallback : v;
    };
    cfg.baseUrl = envOr("STUDIOUS_LLM_BASE_URL", cfg.baseUrl);
    cfg.apiKey = envOr("STUDIOUS_LLM_API_KEY", cfg.apiKey);
    cfg.model = envOr("STUDIOUS_LLM_MODEL", cfg.model);
    return cfg;
}

void runLlmMode()
{
    config::LlmConfig cfg = defaultLlmConfig();

    say(QStringLiteral("=== LLM 真实接入自测 ==="));
    say(QStringLiteral("baseUrl = ") + cfg.baseUrl);
    say(QStringLiteral("model   = ") + cfg.model);
    say(QStringLiteral("timeout = %1 ms, maxTokens = %2, temperature = %3, reasoningEffort = %4")
            .arg(cfg.timeoutMs)
            .arg(cfg.maxTokens)
            .arg(cfg.temperature)
            .arg(cfg.reasoningEffort.isEmpty() ? QStringLiteral("<默认>") : cfg.reasoningEffort));
    say(QString());

    studious::net::QtHttpClient http;
    translate::LlmTranslator llm(cfg, http);

    Assertions a;
    a.expect(llm.isReady(), QStringLiteral("isReady()=false，配置被判定无效"));
    a.expect(llm.id() == QStringLiteral("llm"), QStringLiteral("id() 不是 llm"));
    a.expect(!llm.resolve(QStringLiteral("auto"), QStringLiteral("zh")).reason.isEmpty(),
             QStringLiteral("resolve() 没给原因"));
    a.expect(llm.supports(QStringLiteral("auto"), QStringLiteral("zh")),
             QStringLiteral("supports(auto,zh) 应为 true"));

    // 逐段独立请求：这样耗时统计就是"每次请求"的真实耗时，也便于定位是哪一段失败。
    const QStringList paragraphs = {QStringLiteral("Hello, world!"),
                                    QStringLiteral("This is a test.")};
    qint64 totalMs = 0;
    int requests = 0;

    for (int i = 0; i < paragraphs.size(); ++i) {
        QElapsedTimer timer;
        timer.start();
        const core::TranslateOutcome r =
            llm.translate({paragraphs.at(i)}, QStringLiteral("auto"), {QStringLiteral("zh")}, {});
        const qint64 elapsed = timer.elapsed();
        totalMs += elapsed;
        ++requests;

        const QStringList zh = r.byLang.value(QStringLiteral("zh"));
        const QString text = zh.isEmpty() ? QString() : zh.first();

        if (!r.ok || text.trimmed().isEmpty()) {
            // 引擎的 error 自己带"目标语言 X 第 N 段失败"前缀（单段请求，所以 N 恒为 1），原样贴出来即可。
            a.expect(false,
                     QStringLiteral("第 %1 段失败（%2 ms），引擎返回：%3")
                         .arg(i + 1)
                         .arg(elapsed)
                         .arg(r.error.isEmpty() ? QStringLiteral("译文为空") : r.error));
            continue;
        }
        say(QStringLiteral("第 %1 段 原文: %2").arg(i + 1).arg(paragraphs.at(i)));
        say(QStringLiteral("第 %1 段 译文: %2    [耗时 %3 ms]").arg(i + 1).arg(text).arg(elapsed));
        say(QString());

        // 中文译文的最朴素校验：必须含中日韩统一表意文字。
        bool hasCjk = false;
        for (const QChar &c : text) {
            if (c.unicode() >= 0x4E00 && c.unicode() <= 0x9FFF) {
                hasCjk = true;
                break;
            }
        }
        a.expect(hasCjk, QStringLiteral("第 %1 段译文不含中文字符: \"%2\"").arg(i + 1).arg(text));
    }

    if (a.ok()) {
        say(QStringLiteral("[INFO] %1 次请求共 %2 ms，平均 %3 ms/次")
                .arg(requests)
                .arg(totalMs)
                .arg(requests > 0 ? totalMs / requests : 0));
        markPass(QStringLiteral("llm-real"));
        return;
    }

    markFail(QStringLiteral("llm-real"), a.details());
    say(QString());
    dumpRawLlmResponse(cfg, http);
}

} // namespace

// --models：验证"前端填完 Key + 地址后去问网关"这条链路。
// 打印模型目录、按模型能力推导的思考等级，并真发一次探测确认思考能被关掉。
void runModelsMode()
{
    const config::LlmConfig cfg = defaultLlmConfig();
    studious::net::QtHttpClient http;

    say(QStringLiteral("=== 模型目录拉取（GET {baseUrl}/models） ==="));
    say(QStringLiteral("baseUrl = ") + cfg.baseUrl);
    say(QString());

    const llm::ModelListResult result = llm::listModels(cfg, http);
    if (!result.ok) {
        markFail(QStringLiteral("listModels"), result.error);
        return;
    }

    say(QStringLiteral("共 %1 个聊天模型：").arg(result.models.size()));
    for (const llm::LlmModelInfo &m : result.models) {
        say(QStringLiteral("  %1").arg(m.id));
        say(QStringLiteral("      名称=%1  供应商=%2  推理=%3  工具=%4  输入上限=%5  输出上限=%6")
                .arg(m.name,
                     m.ownedBy.isEmpty() ? QStringLiteral("-") : m.ownedBy,
                     m.supportsReasoning ? QStringLiteral("是") : QStringLiteral("否"),
                     m.supportsTools ? QStringLiteral("是") : QStringLiteral("否"))
                .arg(m.maxInputTokens)
                .arg(m.maxOutputTokens));
    }
    markPass(QStringLiteral("listModels"));

    // 找出配置里指定的模型，推导它支持的思考等级。
    llm::LlmModelInfo target;
    for (const llm::LlmModelInfo &m : result.models) {
        if (m.id == cfg.model) {
            target = m;
            break;
        }
    }

    say(QString());
    if (target.id.isEmpty()) {
        markFail(QStringLiteral("findTarget"),
                 QStringLiteral("模型 %1 不在返回列表里").arg(cfg.model));
        return;
    }

    const QVector<llm::ReasoningOption> options = llm::reasoningOptionsFor(target);
    say(QStringLiteral("模型 %1 支持的思考等级（由能力推导）：").arg(target.id));
    for (const llm::ReasoningOption &o : options) {
        say(QStringLiteral("  [%1] %2%3")
                .arg(o.value.isEmpty() ? QStringLiteral("<不发送>") : o.value,
                     o.label,
                     o.recommended ? QStringLiteral("   ← 推荐") : QString()));
    }
    markPass(QStringLiteral("reasoningOptions"));

    // 真实探测：元数据说支持推理，不代表真的能被关掉。
    say(QString());
    say(QStringLiteral("真实探测：reasoning_effort=none 是否真能关掉思考…"));
    const llm::ReasoningProbeResult probe = llm::probeReasoning(cfg, http);
    if (!probe.ok) {
        markFail(QStringLiteral("probeReasoning"), probe.error);
        return;
    }
    say(QStringLiteral("  reasoning_tokens  = %1").arg(probe.reasoningTokens));
    say(QStringLiteral("  reasoning_content = %1")
            .arg(probe.hasReasoningContent ? QStringLiteral("有（仍在思考）")
                                           : QStringLiteral("无")));
    say(QStringLiteral("  探测译文           = %1").arg(probe.sample));

    switch (probe.probe) {
    case llm::ReasoningProbe::Disabled:
        say(QStringLiteral("  结论：思考已成功关闭"));
        markPass(QStringLiteral("probeReasoning"));
        break;
    case llm::ReasoningProbe::StillThinking:
        markFail(QStringLiteral("probeReasoning"), QStringLiteral("参数无效，模型仍在思考"));
        break;
    case llm::ReasoningProbe::Unknown:
        markFail(QStringLiteral("probeReasoning"),
                 QStringLiteral("网关未返回 usage 明细，无法判定"));
        break;
    }
}

// --llm-batch：验证"批量合并请求"带来的加速，并保证译文与逐段一致。
void runLlmBatchMode()
{
    config::LlmConfig base = defaultLlmConfig();
    studious::net::QtHttpClient http;

    const QStringList paragraphs = {
        QStringLiteral("Hello, world!"),
        QStringLiteral("Good morning."),
        QStringLiteral("How are you today?"),
        QStringLiteral("See you tomorrow."),
        QStringLiteral("Thank you very much."),
    };

    const auto runWith = [&](int batchSize, QStringList &outTexts, qint64 &outMs) {
        config::LlmConfig cfg = base;
        cfg.batchSize = batchSize;
        translate::LlmTranslator translator(cfg, http);
        QElapsedTimer timer;
        timer.start();
        const core::TranslateOutcome outcome =
            translator.translate(paragraphs, QStringLiteral("auto"), {QStringLiteral("zh")}, QJsonObject());
        outMs = timer.elapsed();
        outTexts = outcome.byLang.value(QStringLiteral("zh"));
        return outcome;
    };

    say(QStringLiteral("=== LLM 批量合并对比（%1 段，英->中） ===").arg(paragraphs.size()));
    say(QStringLiteral("model = ") + base.model);
    say(QString());

    QStringList onceTexts;
    qint64 onceMs = 0;
    const core::TranslateOutcome onceOutcome = runWith(1, onceTexts, onceMs);
    say(QStringLiteral("[batchSize=1 逐段请求] %1 ms").arg(onceMs));
    if (!onceOutcome.ok) {
        markFail(QStringLiteral("perSegment"), onceOutcome.error);
        return;
    }
    for (const QString &t : onceTexts) {
        say(QStringLiteral("    ") + t);
    }
    markPass(QStringLiteral("perSegment"));

    say(QString());
    QStringList batchTexts;
    qint64 batchMs = 0;
    const core::TranslateOutcome batchOutcome = runWith(8, batchTexts, batchMs);
    say(QStringLiteral("[batchSize=8 合并请求] %1 ms").arg(batchMs));
    if (!batchOutcome.ok) {
        markFail(QStringLiteral("batch"), batchOutcome.error);
        return;
    }
    for (const QString &t : batchTexts) {
        say(QStringLiteral("    ") + t);
    }
    markPass(QStringLiteral("batch"));

    // 段落数必须与输入一致，否则回填会错位。
    if (batchTexts.size() != paragraphs.size()) {
        markFail(QStringLiteral("batchCount"),
                 QStringLiteral("返回 %1 段，期望 %2 段").arg(batchTexts.size()).arg(paragraphs.size()));
        return;
    }
    markPass(QStringLiteral("batchCount"));

    say(QString());
    if (batchMs < onceMs) {
        say(QStringLiteral("加速：%1 ms -> %2 ms（%3x）")
                .arg(onceMs)
                .arg(batchMs)
                .arg(QString::number(double(onceMs) / double(qMax<qint64>(1, batchMs)), 'f', 1)));
    } else {
        say(QStringLiteral("注意：批量未更快（%1 ms vs %2 ms），可能是网关不支持长输出或多段合并")
                .arg(batchMs)
                .arg(onceMs));
    }
}

// --cache：验证翻译记忆确实生效（第二次调用 0 请求、耗时断崖式下降）。
void runCacheMode()
{
    config::LlmConfig cfg = defaultLlmConfig();
    studious::net::QtHttpClient http;

    // 用临时库，别污染用户真实缓存。
    const QString dbPath = QDir::tempPath() + QStringLiteral("/studious_cache_selftest.db");
    QFile::remove(dbPath);

    cache::TranslationCache store(dbPath);
    QString openError;
    if (!store.ensureOpen(&openError)) {
        markFail(QStringLiteral("cacheOpen"), openError);
        return;
    }
    say(QStringLiteral("=== 翻译缓存自测 ==="));
    say(QStringLiteral("库路径 = ") + dbPath);
    markPass(QStringLiteral("cacheOpen"));

    const QStringList paragraphs = {
        QStringLiteral("Hello, world!"),
        QStringLiteral("Good morning."),
        QStringLiteral("How are you today?"),
    };

    const auto buildRouter = [&](cache::TranslationCache *cachePtr, bool enabled) {
        auto translators = factory::makeDefaultTranslators(http, cfg, {});
        auto r = std::make_unique<router::TranslatorRouter>(
            std::move(translators), QStringList{QStringLiteral("llm")});
        if (cachePtr != nullptr) {
            r->setTranslationCache(cachePtr);
            r->setCacheEnabled(enabled);
        }
        return r;
    };

    const auto makeRequest = [&]() {
        core::TranslateRequest req;
        req.paragraphs = paragraphs;
        req.srcLang = QStringLiteral("auto");
        req.dstLangs = {QStringLiteral("zh")};
        req.preferredEngine = QStringLiteral("llm");
        return req;
    };

    // 1) 关闭缓存：报告应为 used=false（完全不碰缓存）
    {
        auto r = buildRouter(&store, false);
        const core::TranslateOutcome out = r->translate(makeRequest());
        const auto report = r->lastCacheReport();
        if (out.ok && !report.used) {
            markPass(QStringLiteral("cacheDisabledIgnored"));
        } else {
            markFail(QStringLiteral("cacheDisabledIgnored"),
                     out.ok ? QStringLiteral("关闭缓存后仍然记录了命中统计")
                            : out.error);
            return;
        }
    }

    // 2) 开启缓存，第一次：应全部 miss
    QElapsedTimer timer;
    timer.start();
    QStringList firstRun;
    {
        auto r = buildRouter(&store, true);
        const core::TranslateOutcome out = r->translate(makeRequest());
        const auto report = r->lastCacheReport();
        if (!out.ok) {
            markFail(QStringLiteral("cacheFirstRun"), out.error);
            return;
        }
        firstRun = out.byLang.value(QStringLiteral("zh"));
        say(QStringLiteral("第一次：hits=%1 misses=%2  %3 ms")
                .arg(report.hits).arg(report.misses).arg(timer.elapsed()));
        if (report.hits == 0 && report.misses == paragraphs.size()) {
            markPass(QStringLiteral("cacheFirstMiss"));
        } else {
            markFail(QStringLiteral("cacheFirstMiss"),
                     QStringLiteral("首次应全部未命中，实际 hits=%1").arg(report.hits));
        }
    }
    const qint64 firstMs = timer.elapsed();

    // 3) 第二次：应全部命中且几乎不耗时
    timer.restart();
    QStringList secondRun;
    {
        auto r = buildRouter(&store, true);
        const core::TranslateOutcome out = r->translate(makeRequest());
        const auto report = r->lastCacheReport();
        if (!out.ok) {
            markFail(QStringLiteral("cacheSecondRun"), out.error);
            return;
        }
        secondRun = out.byLang.value(QStringLiteral("zh"));
        say(QStringLiteral("第二次：hits=%1 misses=%2  %3 ms")
                .arg(report.hits).arg(report.misses).arg(timer.elapsed()));
        if (report.hits == paragraphs.size() && report.misses == 0) {
            markPass(QStringLiteral("cacheSecondHit"));
        } else {
            markFail(QStringLiteral("cacheSecondHit"),
                     QStringLiteral("第二次应全部命中，实际 hits=%1").arg(report.hits));
        }
    }
    const qint64 secondMs = timer.elapsed();

    // 4) 译文必须一致，缓存不能改变结果
    if (firstRun == secondRun && firstRun.size() == paragraphs.size()) {
        markPass(QStringLiteral("cacheSameResult"));
    } else {
        markFail(QStringLiteral("cacheSameResult"), QStringLiteral("两次译文不一致"));
    }

    say(QString());
    say(QStringLiteral("耗时：首次 %1 ms -> 缓存命中 %2 ms").arg(firstMs).arg(secondMs));
    say(QStringLiteral("库内条数 = %1").arg(store.count()));

    // 5) 关闭开关后不再写入（清空后再跑一次，条数应保持 0）
    store.clear();
    {
        auto r = buildRouter(&store, false);
        const core::TranslateOutcome out = r->translate(makeRequest());
        if (out.ok && store.count() == 0) {
            markPass(QStringLiteral("cacheOffNoWrite"));
        } else {
            markFail(QStringLiteral("cacheOffNoWrite"),
                     QStringLiteral("关闭缓存后仍写入了 %1 条").arg(store.count()));
        }
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    const bool offline = args.contains(QStringLiteral("--offline"));
    const bool providersMode = args.contains(QStringLiteral("--providers"));
    const bool llmMode = args.contains(QStringLiteral("--llm"));
    const bool modelsMode = args.contains(QStringLiteral("--models"));
    const bool llmBatchMode = args.contains(QStringLiteral("--llm-batch"));
    const bool cacheMode = args.contains(QStringLiteral("--cache"));
    const bool manifestMode = args.contains(QStringLiteral("--manifest"));
    const bool i18nMode = args.contains(QStringLiteral("--i18n"));
    const bool assetsMode = args.contains(QStringLiteral("--assets"));

    if (i18nMode) {
        runI18nMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("I18N CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    if (assetsMode) {
        runAssetsMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("ASSETS CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    // 专项模式互斥，先处理它们，互不影响原有两条路径。
    if (manifestMode) {
        runManifestMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("MANIFEST CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    if (cacheMode) {
        runCacheMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("CACHE CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    if (llmBatchMode) {
        runLlmBatchMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("LLM BATCH CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    if (modelsMode) {
        runModelsMode();
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("MODELS CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        for (const QString &item : g_failedItems) {
            say(QStringLiteral("  - ") + item);
        }
        return 1;
    }

    if (providersMode) {
        studious::net::QtHttpClient http;
        runProvidersMode(http);
        say(QString());
        if (g_failed == 0) {
            say(QStringLiteral("PROVIDERS CHECK PASSED"));
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        return 1;
    }

    if (llmMode) {
        runLlmMode();
        say(QString());
        if (g_failed == 0) {
            return 0;
        }
        say(QStringLiteral("FAILED: %1 项").arg(g_failed));
        return 1;
    }

    say(QStringLiteral("=== Studious 后端自测 ==="));
    say(QStringLiteral("模式: ") + (offline ? QStringLiteral("离线（跳过网络测试）")
                                            : QStringLiteral("联网")));
    say(QString());

    testMock();
    testLanguageRegistry();

    studious::net::QtHttpClient http;
    testRouterDegrade(http);

    if (offline) {
        say(QStringLiteral("[SKIP] web（--offline）"));
    } else {
        testWebNetwork(http);
    }

    say(QString());
    if (g_failed == 0) {
        say(QStringLiteral("ALL CORE TESTS PASSED"));
        return 0;
    }

    say(QStringLiteral("FAILED: %1 项").arg(g_failed));
    for (const QString &item : g_failedItems) {
        say(QStringLiteral("  - ") + item);
    }
    return 1;
}
