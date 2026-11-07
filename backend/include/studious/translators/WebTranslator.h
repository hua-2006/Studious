#pragma once

#include <atomic>

#include "contract/ITranslator.h"
#include "net/IHttpClient.h"
#include "translators/ICancelControl.h"
#include "translators/IWebAdapter.h"
#include "translators/WebProvider.h"

#include <QVector>

#include <memory>

namespace studious::translate {

// Web 翻译组件：持有一组可插拔适配器，任一适配器可用即可工作。
// 无资产依赖，故 isReady() 恒 true；不可用时以 error 形式反馈给上层。
//
// 多来源语义：
//   1. 适配器顺序即降级顺序，由 Factory 按前端勾选的来源 id 装配（见 makeWebAdapters）。
//   2. 未配置的适配器（如 Azure 没填 key）直接跳过，不发无意义的请求。
//   3. 任一适配器失败都继续试下一个，只要有一家成功就采用它的译文。
class WebTranslator final : public core::ITranslator, public ICancelControl {
public:
    WebTranslator(net::IHttpClient &http, QVector<std::shared_ptr<IWebAdapter>> adapters);

    QString id() const override;

    // 缓存维度：id() 对所有来源都是 "web"，不足以区分 google / mymemory。
    // 这里编进去的是"上一次真正产出译文的来源顺序"，不是"装了哪些来源"：
    //   - 装了 google+mymemory、但 google 一直失败，实际译文全来自 mymemory，
    //     旧实现会把 key 记成 google+mymemory，切换顺序后拿到别家的旧译文；
    //   - 现在 key 只反映真实产出者，来源顺序或实际生效的来源一变，key 就变。
    // 本轮还没跑过 translate() 时退回"已装配的来源顺序"（tag 只是会算错一次，
    // 下一次调用即自愈，不会读错别人的译文）。
    QString cacheTag() const override;
    bool isReady() const override;
    core::ResolveResult resolve(const QString &src, const QString &dst) const override;
    QStringList supportedTargetLangs() const override;
    bool supports(const QString &src, const QString &dst) const override;
    core::TranslateOutcome translate(const QStringList &paragraphs,
                                     const QString &srcLang,
                                     const QStringList &dstLangs,
                                     const QJsonObject &options) override;
    void cancel() override;

    // 取消标志只由 Router 在每个请求开始时清零（见 ICancelControl.h 的说明）。
    void resetCancel() override;

    // 适配器列表（诊断 / UI 展示用）。
    QVector<std::shared_ptr<IWebAdapter>> adapters() const;

    // 当前实际装配了哪些来源，以及各自是否已配置（设置页展示 / 自测用）。
    QVector<WebProviderInfo> providers() const;

private:
    net::IHttpClient &m_http;
    QVector<std::shared_ptr<IWebAdapter>> m_adapters;
    std::atomic_bool m_cancelRequested{false};

    // 最近一次 translate() 里真正产出译文的适配器 id（按首次成功顺序，已去重）。
    // mutable：cacheTag() 是 const 但需要读它。写入只发生在 translate() 里，
    // 与 Router 的调用（同一工作线程）串行，不存在并发写。
    mutable QStringList m_activeSources;
};

} // namespace studious::translate
