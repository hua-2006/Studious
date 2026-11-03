// Studious 后端控制台自测。
// 覆盖：Mock 引擎数据契约、语言码表、Router 强制引擎降级、真实 Web 翻译（允许失败）、
//       Web 来源注册表、LLM 真实接入。
// 用法：
//   backend_selftest.exe            包含网络测试
//   backend_selftest.exe --offline  跳过网络测试
//   backend_selftest.exe --providers 打印 Web 来源表 + 装配校验
//   backend_selftest.exe --llm       用本机网关真实翻译两句话（需要网关在 127.0.0.1:3065）
//                                    可用环境变量临时换网关/Key/模型排查：
//                                    STUDIOUS_LLM_BASE_URL / STUDIOUS_LLM_API_KEY / STUDIOUS_LLM_MODEL

#include "config/LlmConfig.h"
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

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QtGlobal>

#include <cstdio>
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

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    const bool offline = args.contains(QStringLiteral("--offline"));
    const bool providersMode = args.contains(QStringLiteral("--providers"));
    const bool llmMode = args.contains(QStringLiteral("--llm"));
    const bool modelsMode = args.contains(QStringLiteral("--models"));

    // 专项模式互斥，先处理它们，互不影响原有两条路径。
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
