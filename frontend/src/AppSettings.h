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

    // 本地引擎开关（对应界面上的「本地引擎」勾选框，QSettings 键 settings/llm/useLocalEngine）。
    // true 时翻译走本机 llama.cpp（llama-server.exe + 已下载的 GGUF），
    // 与 llm.baseUrl / apiKey / model 那套云端配置完全解耦：本地模式只借用 llm 组件，
    // 连接参数在提交任务前被覆盖成 127.0.0.1:<port>/v1。
    bool useLocalEngine = false;

    // 本地引擎用哪个模型：models/ 下的文件名（键 settings/llm/localModel）。
    // 空 = 用 availableModels() 的第一个；存着的模型被删了也按空处理（界面会回落到第一项）。
    QString localModelFileName;

    // 本地引擎是否优先用 GPU（键 settings/llm/localUseGpu，默认 true）。
    // true 且 vulkan 运行时已安装 -> llama-server 带 -ngl 99；否则自动回落 CPU。
    // 「优先」是刻意的措辞：没装 vulkan 运行时不会报错，只是当成 false。
    bool localUseGpu = true;

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
