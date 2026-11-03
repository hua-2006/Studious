#pragma once

#include "contract/ITranslator.h"

namespace studious::translate {

// 离线自测用的假引擎：不联网、无资产，把原文加上目标语言前缀原样返回。
// 用途：CI / 无网环境下验证 Router 选路与数据契约是否打通。
class MockTranslator final : public core::ITranslator {
public:
    QString id() const override;
    bool isReady() const override;
    core::ResolveResult resolve(const QString &src, const QString &dst) const override;
    QStringList supportedTargetLangs() const override;
    bool supports(const QString &src, const QString &dst) const override;
    QString detectLanguage(const QStringList &texts) const override;
    core::TranslateOutcome translate(const QStringList &paragraphs,
                                     const QString &srcLang,
                                     const QStringList &dstLangs,
                                     const QJsonObject &options) override;
    void cancel() override;

private:
    bool m_cancelRequested = false;
};

} // namespace studious::translate
