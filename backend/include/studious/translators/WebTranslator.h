#pragma once

#include "contract/ITranslator.h"
#include "net/IHttpClient.h"
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
class WebTranslator final : public core::ITranslator {
public:
    WebTranslator(net::IHttpClient &http, QVector<std::shared_ptr<IWebAdapter>> adapters);

    QString id() const override;

    // 缓存维度：id() 对所有来源都是 "web"，不足以区分 google / mymemory。
    // 这里把实际装配的来源顺序编进去，来源一变 key 就变。
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

    // 适配器列表（诊断 / UI 展示用）。
    QVector<std::shared_ptr<IWebAdapter>> adapters() const;

    // 当前实际装配了哪些来源，以及各自是否已配置（设置页展示 / 自测用）。
    QVector<WebProviderInfo> providers() const;

private:
    net::IHttpClient &m_http;
    QVector<std::shared_ptr<IWebAdapter>> m_adapters;
    bool m_cancelRequested = false;
};

} // namespace studious::translate
