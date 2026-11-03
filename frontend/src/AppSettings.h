#pragma once

#include <QString>

#include <studious/config/LlmConfig.h>

// 前端持久化配置快照。
//
// 落盘位置：QSettings("Studious", "Studious")——Windows 下进注册表
// HKCU\Software\Studious\Studious，不自己管文件路径。
//
// 约定：所有字符串字段"空 = 用内置默认 / 空 = 自动"，
// 所以 load() 里除 systemPrompt 外都做空值回落，systemPrompt 空是合法取值
// （空 = 让后端用内置 prompt）。
struct AppSettings {
    studious::config::LlmConfig llm;   // LLM 网关连接与采样参数
    QString webProviderId;             // 空 = 自动（google -> mymemory）
    QString engineId;                  // 空 = 自动
    QString srcLang = QStringLiteral("auto");
    QString dstLang = QStringLiteral("zh");

    // 从 QSettings 读；缺失 / 空值回落 defaults()。
    static AppSettings load();

    // 写回 QSettings。
    void save() const;

    // 内置默认值（含本机网关配置），也是"恢复默认"按钮的目标状态。
    static AppSettings defaults();
};
