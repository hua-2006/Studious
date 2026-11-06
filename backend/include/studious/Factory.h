#pragma once

#include "config/LlmConfig.h"
#include "contract/ITranslator.h"
#include "router/TranslatorRouter.h"

#include <QStringList>

#include <memory>

namespace studious::net {
class IHttpClient;
}

namespace studious::factory {

// ============================================================
// 组件工厂
//
// 前端不应该知道具体实现类，也无法构造内部适配器
// （GoogleFreeAdapter / MyMemoryAdapter / AzureTranslatorAdapter 位于 src/ 下，不对外暴露）。
// 统一从这里拿实例。
// ============================================================

std::unique_ptr<core::ITranslator> makeMockTranslator();

// Web 组件默认装配 Google 免费接口 + MyMemory 两个适配器，
// 前者失败自动降级到后者。
std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http);

// 按前端勾选的来源 id 顺序装配（"google" / "azure" / "mymemory"）。
// 未知 id 忽略；空列表等价于默认顺序 google -> mymemory。
std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http,
                                                     const QStringList &providerIds);

// 空配置版本：isReady() == false，用于"用户还没填 LLM 设置"的场景。
std::unique_ptr<core::ITranslator> makeLlmTranslator();

// 真实可用版本：把用户设置里的地址 / key / 模型注入进 LlmTranslator。
std::unique_ptr<core::ITranslator> makeLlmTranslator(const config::LlmConfig &cfg,
                                                     net::IHttpClient &http);

std::unique_ptr<core::ITranslator> makeLocalMtTranslator();

// 装配默认组件集合，顺序即 Router 的默认优先级：
//   local-mt -> llm -> web
//
// mock（假译文引擎）默认不注册：它只用于自测，任何生产路径都不该出现在装配里。
// 需要它时必须显式传 includeMock = true（目前只有 selftest 这么做）。
//
// 注意：http 的生命周期必须长于返回的组件集合，
// 且必须与使用它们的线程一致（QNetworkAccessManager 是线程亲和的）。
router::TranslatorList makeDefaultTranslators(net::IHttpClient &http, bool includeMock = false);

// 带配置版本：llmCfg 为空配置时等同于上一个重载（llm 组件仍会注册，但处于未就绪状态）。
// webProviders 为空时 web 组件走默认来源顺序。
// includeMock 语义同上：默认不注册 mock。
router::TranslatorList makeDefaultTranslators(net::IHttpClient &http,
                                              const config::LlmConfig &llmCfg,
                                              const QStringList &webProviders = {},
                                              bool includeMock = false);

} // namespace studious::factory
