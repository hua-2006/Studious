#pragma once

#include <QString>

#include <studious/config/LlmConfig.h>

// 前端持久化配置快照。
//
// 落盘位置：QSettings("Studious", "Studious")——Windows 下进注册表
// HKCU\Software\Studious\Studious，不自己管文件路径。
//
// 约定：所有字符串字段"空 = 用内置默认 / 空 = 自动"，
// 所以 load() 里除 systemPrompt / uiLanguage 外都做空值回落：
//   systemPrompt 空是合法取值（空 = 让后端用内置 prompt）；
//   uiLanguage 空是合法取值（空 = 跟随系统语言）。
struct AppSettings {
    studious::config::LlmConfig llm;   // LLM 网关连接与采样参数
    QString webProviderId;             // 空 = 自动（google -> mymemory）
    QString engineId;                  // 空 = 自动
    QString srcLang = QStringLiteral("auto");
    QString dstLang = QStringLiteral("zh");

    // 翻译缓存开关（对应界面上的「启用缓存」勾选框）。
    // 关掉后 Router 既不查缓存也不写缓存，翻译耗时回到纯网络往返。
    bool cacheEnabled = true;

    // 界面语言：空 = 跟随系统；"zh" = 强制中文；"en" = 强制英文。
    //
    // 只影响**界面文案与后端本地化文案**（后端通过 i18n::setLanguage 覆盖），
    // 不改翻译本身的目标语言（那是 dstLang 的事）。
    // Qt 没有运行时换语言的机制，所以改完必须重启进程才生效。
    QString uiLanguage;

    // 从 QSettings 读；缺失 / 空值回落 defaults()。
    static AppSettings load();

    // 写回 QSettings。
    void save() const;

    // 内置默认值（含本机网关配置），也是"恢复默认"按钮的目标状态。
    static AppSettings defaults();
};
