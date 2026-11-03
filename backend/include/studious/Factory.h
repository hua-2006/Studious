#pragma once

#include "contract/ITranslator.h"
#include "router/TranslatorRouter.h"

#include <memory>

namespace studious::net {
class IHttpClient;
}

namespace studious::factory {

// ============================================================
// 组件工厂
//
// 前端不应该知道具体实现类，也无法构造内部适配器
// （GoogleFreeAdapter / MyMemoryAdapter 位于 src/ 下，不对外暴露）。
// 统一从这里拿实例。
// ============================================================

std::unique_ptr<core::ITranslator> makeMockTranslator();

// Web 组件默认装配 Google 免费接口 + MyMemory 两个适配器，
// 前者失败自动降级到后者。
std::unique_ptr<core::ITranslator> makeWebTranslator(net::IHttpClient &http);

std::unique_ptr<core::ITranslator> makeLlmTranslator();

std::unique_ptr<core::ITranslator> makeLocalMtTranslator();

// 装配默认组件集合，顺序即 Router 的默认优先级：
//   mock -> web -> llm -> local-mt
//
// 注意：http 的生命周期必须长于返回的组件集合，
// 且必须与使用它们的线程一致（QNetworkAccessManager 是线程亲和的）。
router::TranslatorList makeDefaultTranslators(net::IHttpClient &http);

} // namespace studious::factory
