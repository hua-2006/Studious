#include "Factory.h"

#include "net/IHttpClient.h"
#include "translators/LlmTranslator.h"
#include "translators/LocalMtTranslator.h"
#include "translators/MockTranslator.h"
#include "translators/WebProvider.h"
#include "translators/WebTranslator.h"

namespace studious::factory {

using namespace studious::translate;

std::unique_ptr<core::ITranslator> makeMockTranslator()
{
    return std::make_unique<MockTranslator>();
}

std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http)
{
    // 旧签名保留给现有前端：等价于"按默认顺序装配"。
    return makeWebTranslator(http, {});
}

std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http,
                                                     const QStringList &providerIds)
{
    // 适配器顺序即降级顺序，全部由 makeWebAdapters 决定：
    //   空列表 -> google -> mymemory（与旧行为一致）
    //   指定列表 -> 按前端勾选顺序，未知 id 自动忽略
    return std::make_unique<WebTranslator>(http, makeWebAdapters(providerIds));
}

std::unique_ptr<core::ITranslator> makeLlmTranslator()
{
    // 空配置：isReady() == false，Router 会给出"未配置 LLM API"的可读原因。
    return std::make_unique<LlmTranslator>();
}

std::unique_ptr<core::ITranslator> makeLlmTranslator(const config::LlmConfig &cfg,
                                                     net::IHttpClient &http)
{
    return std::make_unique<LlmTranslator>(cfg, http);
}

std::unique_ptr<core::ITranslator> makeLocalMtTranslator()
{
    return std::make_unique<LocalMtTranslator>();
}

router::TranslatorList makeDefaultTranslators(net::IHttpClient &http)
{
    return makeDefaultTranslators(http, config::LlmConfig{}, {});
}

router::TranslatorList makeDefaultTranslators(net::IHttpClient &http,
                                              const config::LlmConfig &llmCfg,
                                              const QStringList &webProviders)
{
    router::TranslatorList list;
    list.reserve(4);
    list.push_back(makeMockTranslator());
    list.push_back(makeWebTranslator(http, webProviders));
    list.push_back(makeLlmTranslator(llmCfg, http));
    list.push_back(makeLocalMtTranslator());
    return list;
}

} // namespace studious::factory
