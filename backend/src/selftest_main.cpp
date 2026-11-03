// Studious 后端控制台自测。
// 覆盖：Mock 引擎数据契约、语言码表、Router 强制引擎降级、真实 Web 翻译（允许失败）。
// 用法：
//   backend_selftest.exe            包含网络测试
//   backend_selftest.exe --offline  跳过网络测试

#include "contract/LanguageRegistry.h"
#include "net/QtHttpClient.h"
#include "router/TranslatorRouter.h"
#include "translators/LlmTranslator.h"
#include "translators/LocalMtTranslator.h"
#include "translators/MockTranslator.h"
#include "translators/WebTranslator.h"
#include "translators/adapters/GoogleFreeAdapter.h"
#include "translators/adapters/MyMemoryAdapter.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    const bool offline = args.contains(QStringLiteral("--offline"));

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
