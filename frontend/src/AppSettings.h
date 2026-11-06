#pragma once

#include <QString>
#include <QStringList>

#include <studious/config/LlmConfig.h>

// 前端持久化配置快照。
//
// 落盘位置：QSettings("Studious", "Studious")——Windows 下进注册表
// HKCU\Software\Studious\Studious，不自己管文件路径。
//
// 约定：所有字符串字段"空 = 用内置默认 / 空 = 自动"，
// 所以 load() 里除 systemPrompt / uiLanguage / accentSeed 外都做空值回落：
//   systemPrompt 空是合法取值（空 = 让后端用内置 prompt）；
//   uiLanguage 空是合法取值（空 = 跟随系统语言）；
//   accentSeed 空是合法取值（空 = 默认紫 #6750A4）。
struct AppSettings {
    studious::config::LlmConfig llm;   // LLM 网关连接与采样参数
    QString webProviderId;             // 空 = 自动（google -> mymemory）
    QString engineId;                  // 空 = 自动
    // 引擎优先级（等价于 TranslatorRouter 的 priority 参数）。
    // 空 = 用后端默认顺序 mock -> web -> llm -> local-mt。
    QStringList enginePriority;
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

    // ---- 本地 llama-server 运行时参数（下面 4 个一起喂给 LocalEngine 启动进程）----

    // 上下文窗口 token 数（键 settings/llm/localContextSize，默认 2048，合法 512~32768）。
    // 对应 llama-server 的 -c。太小装不下长句，太大白吃内存 / 显存。
    int localContextSize = 2048;

    // CPU 线程数（键 settings/llm/localThreads，默认 0，合法 0~64）。
    // 0 = 自动：不传 -t，交给 llama.cpp 自己按物理核心数决定。
    int localThreads = 0;

    // 卸载到 GPU 的层数（键 settings/llm/localGpuLayers，默认 -1，合法 -1~999）。
    // -1 = 自动：沿用 localUseGpu 的老逻辑（装了 vulkan 就 -ngl 99，否则全 CPU）；
    // 0 = 强制全 CPU；>0 = 只卸这么多层（显存不够时手动降档）。
    int localGpuLayers = -1;

    // 额外命令行参数原样拼到 llama-server 命令行尾部
    // （键 settings/llm/localExtraArgs，默认空串 = 不加）。
    // 存 trim 后的串；合法性由用户自己负责（这是给高级用户留的口子）。
    QString localExtraArgs;

    // 界面语言：空 = 跟随系统；"zh" = 强制中文；"en" = 强制英文。
    //    // 只影响**界面文案与后端本地化文案**（后端通过 i18n::setLanguage 覆盖），
    // 不改翻译本身的目标语言（那是 dstLang 的事）。
    // Qt 没有运行时换语言的机制，所以改完必须重启进程才生效。
    QString uiLanguage;

    // 主题："system"（默认）/ "light" / "dark"。
    // QML 侧 Theme 单例按这个值切换调色板，不需要重启。
    QString themeMode = QStringLiteral("system");

    // 字号缩放百分比（80~150，默认 100）。QML 侧 Theme.fontScale = 这个值 / 100。
    int fontScale = 100;

    // 强调色种子（QSettings 键 ui/accent），形如 "#RRGGBB"。
    // 空 = 用默认紫 #6750A4；load() 里非法值同样按空处理。
    // 整套调色板由 Theme 单例在 OKLab 空间从这个种子混出来。
    QString accentSeed;

    // 从 QSettings 读；缺失 / 空值回落 defaults()。
    static AppSettings load();

    // 写回 QSettings。
    void save() const;

    // 内置默认值（含本机网关配置），也是"恢复默认"按钮的目标状态。
    static AppSettings defaults();
};
