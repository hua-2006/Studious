#include "Factory.h"

#include "net/IHttpClient.h"
#include "translators/LlmTranslator.h"
#include "translators/LocalMtTranslator.h"
#include "translators/MockTranslator.h"
#include "translators/WebTranslator.h"
#include "translators/adapters/GoogleFreeAdapter.h"
#include "translators/adapters/MyMemoryAdapter.h"

namespace studious::factory {

using namespace studious::translate;

std::unique_ptr<core::ITranslator> makeMockTranslator()
{
    return std::make_unique<MockTranslator>();
}

std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http)
{
    // 适配器顺序即降级顺序：先 Google（语言覆盖广），失败再 MyMemory。
    QVector<std::shared_ptr<IWebAdapter>> adapters;
    adapters.push_back(std::make_shared<GoogleFreeAdapter>());
    adapters.push_back(std::make_shared<MyMemoryAdapter>());
    return std::make_unique<WebTranslator>(http, adapters);
}

std::unique_ptr<core::ITranslator> makeLlmTranslator()
{
    return std::make_unique<LlmTranslator>();
}

std::unique_ptr<core::ITranslator> makeLocalMtTranslator()
{
    return std::make_unique<LocalMtTranslator>();
}

router::TranslatorList makeDefaultTranslators(net::IHttpClient &http)
{
    router::TranslatorList list;
    list.push_back(makeMockTranslator());
    list.push_back(makeWebTranslator(http));
    list.push_back(makeLlmTranslator());
    list.push_back(makeLocalMtTranslator());
    return list;
}

} // namespace studious::factory
