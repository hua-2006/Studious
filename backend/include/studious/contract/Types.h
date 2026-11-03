#pragma once

#include <QString>
#include <QStringList>
#include <QHash>
#include <QVector>
#include <QJsonObject>

namespace studious::core {

// 一次翻译请求。段落数组 + 语言对 + 可选的引擎偏好。
struct TranslateRequest {
    QStringList paragraphs;      // 每项一个段落
    QString srcLang = "auto";    // "auto" 或内部语言码
    QStringList dstLangs;        // 复数，为多目标预留
    QString preferredEngine;     // 空=按优先级自动选
    QJsonObject options;
};

// 组件"是否可用 / 缺什么"的回答，供 UI 显示资产下载引导。
struct ResolveResult {
    bool ready = false;
    QVector<QString> missingAssets;   // 缺失资产 id
    QString reason;                   // 人类可读原因
};

// 一次翻译的结果。byLang 的每个 value 与请求段落一一对应（含空段落占位）。
struct TranslateOutcome {
    bool ok = false;
    QHash<QString, QStringList> byLang;  // dstLang -> 段落译文
    QString error;
};

} // namespace studious::core
