#include "AppSettings.h"

#include <QSettings>

#include <algorithm>

namespace {

// QSettings 里存的是 QVariant，用户可能手改成越界值（注册表直接改），读回来必须夹紧，
// 否则 QSpinBox 之外的地方（比如 http 超时）会拿到荒谬数字。
int clampedInt(int value, int lo, int hi)
{
    return std::clamp(value, lo, hi);
}

double clampedDouble(double value, double lo, double hi)
{
    return std::clamp(value, lo, hi);
}

// QSettings 的 key 统一前缀，避免和将来别的模块撞名。
QString key(const char *suffix)
{
    return QLatin1String("settings/") + QLatin1String(suffix);
}

} // namespace

AppSettings AppSettings::defaults()
{
    AppSettings s;

    // 本机网关默认值（与 backend_selftest 的 --llm 模式保持一致）。
    s.llm.baseUrl = QStringLiteral("http://127.0.0.1:3065/v1");
    s.llm.apiKey = QStringLiteral("12345678");
    s.llm.model = QStringLiteral("deepseek-v4.1-flash");
    s.llm.timeoutMs = 60000;
    s.llm.maxTokens = 2048;
    s.llm.temperature = 0.2;
    s.llm.systemPrompt.clear();   // 空 = 后端内置 prompt

    s.webProviderId.clear();   // 自动
    s.engineId.clear();        // 自动
    s.srcLang = QStringLiteral("auto");
    s.dstLang = QStringLiteral("zh");
    return s;
}

AppSettings AppSettings::load()
{
    const AppSettings def = defaults();

    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));

    const auto textOr = [&store](const QString &k, const QString &fallback) {
        const QString value = store.value(k).toString().trimmed();
        return value.isEmpty() ? fallback : value;
    };

    AppSettings s = def;

    s.llm.baseUrl = textOr(key("llm/baseUrl"), def.llm.baseUrl);
    s.llm.apiKey = textOr(key("llm/apiKey"), def.llm.apiKey);
    s.llm.model = textOr(key("llm/model"), def.llm.model);
    // systemPrompt 不做空值回落：留空是有意义的状态。
    s.llm.systemPrompt = store.value(key("llm/systemPrompt")).toString();
    s.llm.timeoutMs = clampedInt(store.value(key("llm/timeoutMs"), def.llm.timeoutMs).toInt(),
                                 1000,
                                 180000);
    s.llm.maxTokens = clampedInt(store.value(key("llm/maxTokens"), def.llm.maxTokens).toInt(),
                                 1,
                                 32768);
    s.llm.temperature = clampedDouble(
        store.value(key("llm/temperature"), def.llm.temperature).toDouble(), 0.0, 1.5);

    // 这两个空串本身是合法值（= 自动），textOr 回落到的也是空串，行为一致。
    s.webProviderId = textOr(key("web/providerId"), def.webProviderId);
    s.engineId = textOr(key("engine/id"), def.engineId);
    s.srcLang = textOr(key("lang/src"), def.srcLang);
    s.dstLang = textOr(key("lang/dst"), def.dstLang);

    return s;
}

void AppSettings::save() const
{
    QSettings store(QStringLiteral("Studious"), QStringLiteral("Studious"));

    store.setValue(key("llm/baseUrl"), llm.baseUrl);
    store.setValue(key("llm/apiKey"), llm.apiKey);
    store.setValue(key("llm/model"), llm.model);
    store.setValue(key("llm/timeoutMs"), llm.timeoutMs);
    store.setValue(key("llm/maxTokens"), llm.maxTokens);
    store.setValue(key("llm/temperature"), llm.temperature);
    store.setValue(key("llm/systemPrompt"), llm.systemPrompt);

    store.setValue(key("web/providerId"), webProviderId);
    store.setValue(key("engine/id"), engineId);
    store.setValue(key("lang/src"), srcLang);
    store.setValue(key("lang/dst"), dstLang);

    store.sync();
}
