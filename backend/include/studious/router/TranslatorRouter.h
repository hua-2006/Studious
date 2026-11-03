#pragma once

#include "contract/ITranslator.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

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
    explicit TranslatorRouter(TranslatorList translators,
                              QStringList priority = {"mock", "web", "llm", "local-mt"});

    ~TranslatorRouter();

    TranslatorRouter(const TranslatorRouter &) = delete;
    TranslatorRouter &operator=(const TranslatorRouter &) = delete;

    // 强制组件：req.preferredEngine 非空时只用该组件（即使不可用也返回明确错误）。
    // 否则按 priority 选第一个 isReady() 且 supports 全部目标语言的组件。
    core::TranslateOutcome translate(const core::TranslateRequest &req);

    // 列出所有组件状态，供 UI 显示（组件 id + 资产就绪情况）。
    QVector<QPair<QString, core::ResolveResult>> status(const QString &src,
                                                       const QString &dst) const;

    QStringList engineIds() const;

private:
    core::ITranslator *findById(const QString &id) const;

    TranslatorList m_translators;
    QStringList m_priority;
};

} // namespace studious::router
