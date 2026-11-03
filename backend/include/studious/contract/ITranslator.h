#pragma once

#include "contract/Types.h"

namespace studious::core {

// 翻译组件统一接口。web / mock / local-mt / llm 全部实现它。
// 后端通过本接口调度，不知道具体实现类型。
class ITranslator {
public:
    virtual ~ITranslator() = default;

    virtual QString id() const = 0;                       // "web" | "mock" | "local-mt" | "llm"
    virtual bool isReady() const = 0;
    virtual ResolveResult resolve(const QString &src, const QString &dst) const = 0;
    virtual QStringList supportedTargetLangs() const = 0;
    virtual bool supports(const QString &src, const QString &dst) const = 0;

    // 可选能力：不支持检测的组件返回空串。
    virtual QString detectLanguage(const QStringList &texts) const { Q_UNUSED(texts); return {}; }

    virtual TranslateOutcome translate(const QStringList &paragraphs,
                                       const QString &srcLang,
                                       const QStringList &dstLangs,
                                       const QJsonObject &options) = 0;

    // 中止正在进行的翻译（实现可以是空操作）。
    virtual void cancel() = 0;
};

} // namespace studious::core

