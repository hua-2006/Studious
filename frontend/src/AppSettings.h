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

    // ---- 桌面集成（Windows 原生实现；其它平台一律退化成"不支持"）----

    // 开机自启（键 settings/desktop/autoStart）：写 HKCU\...\Run 键，值是本进程 exe 路径。
    bool autoStart = false;

    // 点最小化就收进托盘（键 settings/desktop/minimizeToTray）。
    // 托盘不可用的平台（没实现 / 会话不支持）上这个开关会被界面置灰。
    bool minimizeToTray = false;

    // 点关闭按钮时干什么（键 settings/desktop/closeBehavior）：
    // "exit" = 直接退出；"tray" = 收进托盘继续在后台跑。
    QString closeBehavior = QStringLiteral("exit");

    // 单实例（键 settings/desktop/singleInstance）：第二次启动只把已有窗口叫出来。
    // 需要重启才生效 —— 本地 socket 在启动时就绑定了。
    bool singleInstance = true;

    // ---- 翻译行为（纯展示层开关，不改翻译结果）----

    // 源语言 == 目标语言时给一行「这是原文」的提示（键 settings/translate/sameLangHint）。
    // 后端本来就直接回填原文、不走引擎；这个开关只管提不提示。
    bool sameLangHint = true;

    // 源语言可疑时显示黄色警告条（键 settings/translate/suspiciousLangWarn）。
    bool suspiciousLangWarn = true;

    // 源语言下拉里提供「自动检测」（键 settings/translate/autoDetectSource）。
    // 关掉只是不再把 auto 作为可选值：本地 MT 那种"必须明确源语言"的引擎因此更好用。
    bool autoDetectSource = true;

    // ---- 网络 ----

    // 代理地址（键 settings/network/proxy），形如 http://127.0.0.1:7897。
    // 空 = 不覆盖，跟随后端的环境变量探测（HTTPS_PROXY / HTTP_PROXY / ALL_PROXY）。
    // 非空时翻译前 qputenv("STUDIOUS_HTTP_PROXY", 值)，下一次新建的 HttpClient 生效。
    QString proxyUrl;

    // 全局 HTTP 超时下限，毫秒（键 settings/network/timeoutMs，默认 30000，合法 1000~600000）。
    // 通过 qputenv("STUDIOUS_HTTP_TIMEOUT") 交给后端：只往上抬各调用点自己的超时，
    // 且回环地址（本地 llama-server / 本地网关）不受影响。
    int httpTimeoutMs = 30000;

    // ---- 资产根目录 ----

    // 资产安装根目录（键 settings/asset/root）。空 = 默认 %APPDATA%/studious/assets。
    // 改这个值只影响「以后从哪里读」，真正搬文件走 AppBackend::migrateAssetRoot()。
    QString assetRoot;

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

    // ---- 本地 llama-server 的生命周期策略（三个开关，默认全关）----
    // 默认全关是刻意的：谁能替用户决定"什么时候把几 GB 的模型从内存里卸掉"？
    // 想省内存的人自己去打开，不想等冷启动的人就让它常驻。

    // 切到别的引擎时顺手停掉本地服务（键 settings/llm/localStopOnSwitch，默认 false）。
    bool localStopOnSwitch = false;

    // 应用启动后自动把本地服务预热起来（键 settings/llm/localPreheat，默认 false）。
    // 只在「运行时 + 模型都在」时才真的拉起，否则静默跳过。
    bool localPreheat = false;

    // 空闲这么久没有本地翻译请求就自动停（键 settings/llm/localIdleStop，默认 false）。
    bool localIdleStop = false;

    // 空闲判定阈值，分钟（键 settings/llm/localIdleMinutes，默认 10，合法 1~240）。
    int localIdleMinutes = 10;

    // ---- 三个新入口（截屏翻译 / 小窗翻译 / 实时翻译）的共用设置 ----
    //
    // 设计稿 docs/TempUI/flows.html 的 A/B/C 三页共用一份 D「二级设置」，
    // 所以这些键集中在这里，不放各自的模块里。

    // 松开选区后直接进「翻译中」，跳过二次确认（键 settings/capture/autoRunOnSelect）。
    bool captureAutoRun = false;

    // 翻译完成后自动复制译文 / 自动关闭浮层（键 settings/capture/autoCopy、autoHide）。
    bool captureAutoCopy = false;
    bool captureAutoHide = true;

    // 浮层外观（键 settings/overlay/opacity 40~100、fontSize 10~20、keepOriginal）。
    int overlayOpacity = 96;
    int overlayFontSize = 13;
    bool overlayKeepOriginal = false;
    // 放大镜的缩放倍率（2~8）。不写死在 QML 里，设置页以后直接改这里。
    int overlayZoom = 3;
    // 放大镜开关与落点（键 settings/overlay/magnifier、magnifierCorner）。
    // corner 取 "bottomRight" / "bottomLeft" / "topRight" / "topLeft"：
    // 贴边放不下时 QML 会自己翻到另一侧，这里只记"用户偏好哪个角"。
    bool overlayMagnifier = true;
    QString magnifierCorner = QStringLiteral("bottomRight");

    // 遮罩压暗程度（键 settings/overlay/dim，20~80，百分比）。
    // 框选时只有选区露出来，其它地方压到接近全黑；太小会看不出"在被框的地方之外"。
    int overlayDim = 45;

    // 方向键微调选区（键 settings/overlay/arrowNudge，默认开）：
    // 鼠标定位到像素级很难，选中后用方向键补 1px（按住 Shift 时 10px）。
    bool overlayArrowNudge = true;

    // 复制后延迟关闭浮层（键 settings/overlay/autoHideDelay，单位秒，0~10）。
    // 0 = 复制完立刻收；非 0 是给"再看一眼译文"留的时间。
    int overlayAutoHideDelay = 0;

    // 实时翻译选区的背景底色（键 settings/overlay/backdrop，0~100，百分比）。
    // 实时贴纸是半透明的，底下原本的字会透出来、跟译文打架；铺一层底就好读多了。
    // 0 = 完全不要底色（只在有译文的地方画贴纸）。
    int overlayBackdrop = 70;

    // 小窗是否置顶（键 settings/mini/alwaysOnTop，默认 true）。
    bool miniAlwaysOnTop = true;

    // 实时翻译轮询间隔毫秒（键 settings/live/intervalMs，400~1500，默认 600）。
    // 太小会把引擎压死，太大会看不出"实时"，所以夹得很紧。
    int liveIntervalMs = 600;

    // 实时翻译锁本地引擎（键 settings/live/engineLocked，默认 true）：
    // 云端 / web 单次 1 秒以上，做实时没有意义，默认逼用户走本地。
    bool liveEngineLocked = true;

    // 只在内容变化时才翻译（键 settings/live/onlyOnChange，默认 true）：
    // 每一帧都翻会把引擎压满，而且同样的句子重复请求也只是吃缓存。
    // 关掉 = 每帧都重翻，用来验证"引擎是不是真的在跑"。
    bool liveOnlyOnChange = true;

    // 记录里存哪一份文本（键 settings/record/saveMode）："original"（OCR 原文）或 "current"。
    QString recordSaveMode = QStringLiteral("original");

    // ---- 翻译历史（键 settings/history/enabled、settings/history/dir）----
    //
    // 现在是"框架"：开关与目录先落地，写入 / 浏览界面在后面批次接上。
    // 目录为空串表示用默认位置（<AppData>/studious/history，由 AppBackend 解析成绝对路径），
    // 这样默认值不依赖运行时的 QStandardPaths，AppSettings 仍然可以被 CLI 无 GUI 地加载。
    bool historyEnabled = false;
    QString historyDir;

    // 三个全局热键（键 settings/hotkey/capture|mini|live）。
    // 空串 = 不注册（界面上显示「未设置」）。格式 "Ctrl+Alt+S"。
    QString hotkeyCapture = QStringLiteral("Ctrl+Alt+S");
    QString hotkeyMini = QStringLiteral("Ctrl+Alt+W");
    QString hotkeyLive = QStringLiteral("Ctrl+Alt+L");

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
