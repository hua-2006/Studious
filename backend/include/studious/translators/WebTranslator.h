#pragma once

#include "contract/ITranslator.h"
#include "net/IHttpClient.h"
#include "translators/IWebAdapter.h"

#include <QVector>

#include <memory>

namespace studious::translate {

// Web 翻译组件：持有一组可插拔适配器，任一适配器可用即可工作。
// 无资产依赖，故 isReady() 恒 true；不可用时以 error 形式反馈给上层。
class WebTranslator final : public core::ITranslator {
public:
    WebTranslator(net::IHttpClient &http, QVector<std::shared_ptr<IWebAdapter>> adapters);

    QString id() const override;
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

private:
    net::IHttpClient &m_http;
    QVector<std::shared_ptr<IWebAdapter>> m_adapters;
    bool m_cancelRequested = false;
};

} // namespace studious::translate
