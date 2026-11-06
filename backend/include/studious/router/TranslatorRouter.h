#pragma once

#include "contract/ITranslator.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

namespace studious::core {
class ITranslator;
}
namespace studious::cache {
class TranslationCache;
}

namespace studious::router {

// 组件所有权容器。
//
// 为什么不是 QVector<std::unique_ptr<ITranslator>>：Qt6 的 QList（QVector 是它的别名）
// 要求元素类型可拷贝——扩容、reserve、甚至 operator[]/data() 都会实例化 copyAppend 路径，
// 而 std::unique_ptr 删除了拷贝构造，直接编译失败（与 Q_DECLARE_TYPEINFO 无关，实测确认）。
// 因此这里用 std::vector 存放 unique_ptr，保持"谁构造谁释放"的所有权语义。
using TranslatorList = std::vector<std::unique_ptr<core::ITranslator>>;

// 翻译调度器：按优先级选组件、按目标语言拆分、失败时给出可读降级原因。
// 这是后端的唯一入口，前端只与它打交道。
class TranslatorRouter {
public:
    // priority 里列出的 id 按顺序尝试；未在 priority 中出现的组件永不自动选中。
    //
    // 默认顺序：本地优先（local-mt -> llm -> web）。
    // mock 只是测试用的假译文引擎，绝不进生产装配——想要它必须由调用方
    // 显式列进 priority（以及 Factory::makeDefaultTranslators(..., includeMock=true)）。
    //
    // priority 里写了但没注册的 id 属于"跳过并继续下一个"，不会让整单失败。
    explicit TranslatorRouter(TranslatorList translators,
                              QStringList priority = {"local-mt", "llm", "web"});

    ~TranslatorRouter();

    TranslatorRouter(const TranslatorRouter &) = delete;
    TranslatorRouter &operator=(const TranslatorRouter &) = delete;

    // 强制组件：req.preferredEngine 非空时只用该组件（即使不可用也返回明确错误，不降级）。
    // 否则按 priority 收集所有 isReady() 且 supports 全部目标语言的组件，逐个尝试：
    // 可用性类失败（网络/超时/429/5xx/本地服务没起来）自动换下一个，语义类失败立刻返回。
    core::TranslateOutcome translate(const core::TranslateRequest &req);

    // 列出所有组件状态，供 UI 显示（组件 id + 资产就绪情况）。
    QVector<QPair<QString, core::ResolveResult>> status(const QString &src,
                                                       const QString &dst) const;

    QStringList engineIds() const;

    // ---------- 翻译缓存（可选） ----------
    //
    // 不注入 = 完全不用缓存，行为与以前一模一样。
    // 注入后还需要 enabled=true 才真正生效，方便界面上放一个开关。
    void setTranslationCache(cache::TranslationCache *cache) { m_cache = cache; }
    void setCacheEnabled(bool enabled) { m_cacheEnabled = enabled; }
    bool isCacheEnabled() const { return m_cache != nullptr && m_cacheEnabled; }

    // 上一次 translate() 的缓存命中情况，供界面显示"命中 3/5（省了 2 次请求）"。
    struct CacheReport {
        bool  used = false;
        int   hits = 0;
        int   misses = 0;
    };
    CacheReport lastCacheReport() const { return m_lastCacheReport; }

private:
    core::ITranslator *findById(const QString &id) const;

    // 用单个引擎跑一遍完整请求（含缓存逻辑）。由 translate() 在降级循环里逐个调用：
    // 返回值就是"这一个引擎的结果"，是否降级、错误怎么聚合由 translate() 决定。
    core::TranslateOutcome runEngine(core::ITranslator *engine,
                                     const core::TranslateRequest &req,
                                     const QString &src,
                                     const QStringList &dsts);

    TranslatorList m_translators;
    QStringList m_priority;

    cache::TranslationCache *m_cache = nullptr;
    bool m_cacheEnabled = true;
    CacheReport m_lastCacheReport;
};

} // namespace studious::router
