#pragma once

#include "translators/IWebAdapter.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace studious::translate {

// 一个可选 Web 来源的元信息，供设置页展示与前端勾选。
struct WebProviderInfo {
    QString id;           // "google" | "azure" | "mymemory"
    QString displayName;  // 中文显示名
    bool    requiresApiKey = false;
    bool    configured = false;   // 当前是否可用
    QString note;         // 补充说明（如"免费，国内需代理"）
};

// 全部可选来源（顺序即默认优先级）。
QVector<WebProviderInfo> listWebProviders();

// 按 id 查单个来源的元信息；未知 id 返回默认构造（id 为空）的对象。
// WebTranslator::providers() 用它给已装配的适配器补上 note 字段。
WebProviderInfo findWebProvider(const QString &id);

// 按给定 id 顺序装配适配器；未知 id 忽略（前端可能传入旧版本遗留的 id）；
// 空列表 = 默认顺序 google -> mymemory。
QVector<std::shared_ptr<IWebAdapter>> makeWebAdapters(const QStringList &providerIds);

} // namespace studious::translate
