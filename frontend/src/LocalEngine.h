#pragma once

#include "platform/ProcessGuard.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

namespace studious::runtime {
class LlamaServer;
}

// 启动本地引擎时的一次性参数快照（值语义，调用方可以随手构造 / 丢弃）。
struct LocalEngineConfig {
    // <root>/models/<file>。也可以只给文件名（相对路径），内部会拼到 models 目录下。
    // 空 = 用 availableModels() 的第一个（与界面「空 = 第一个可用模型」的约定一致）。
    QString modelPath;
    // true 且 Vulkan 运行时存在 -> 用 Vulkan（默认 -ngl 99）；否则回落 CPU（不传 -ngl）。
    bool    preferGpu = true;

    // 下面四项沿用「0 / -1 / 空 = 自动」的约定：界面把用户设置原样存下来交给这里，
    // 折算成真实命令行参数只发生在 LocalEngine 内部，界面不必知道内置默认值是多少
    // （默认值改在这里一次，界面和 --smoke 都跟着变，不会两头对不上）。
    int     contextSize = 0;      // 0 = 用内置默认 2048
    int     threads = 0;          // 0 = 按后端自动（CPU 6 / GPU 8，见 preferredThreads）
    int     gpuLayers = -1;       // -1 = 自动（Vulkan 99 / CPU 0）；0 = 显式不卸载任何层
    QString extraArgs;           // 追加到 llama-server 命令行的额外参数，空格分隔
};

// 本地 llama.cpp 引擎的轻量管理器（**主线程持有**）。
//
// 定位：把「选哪个运行时 / 用哪个模型 / 什么时候重启 / 起来后地址是多少」收在一处，
// 让 MainWindow 与 --smoke / --switch-demo 用同一套逻辑，不必各自拼路径。
//
// 资产布局（后缀与可执行名由平台层给，见 IPlatform::runtimeAssetSuffix /
// serverExecutableName，Windows 是 win-x64 / llama-server.exe）：
//   runtime: <installRoot>/runtime/runtime.llama.{cpu,vulkan}.<平台后缀>/<服务端可执行名>
//   model  : <installRoot>/models/llm/<资产 id>（清单里的 model.qwen2.5-* 就是无扩展名文件）
//
// 线程约束（重要）：ensureStarted() 会阻塞数百毫秒到数秒（GPU 冷启动实测约 2.2 秒），
// 且内部是 QProcess（主线程亲和），**只能在主线程调用**；
// 翻译本身仍然在工作线程里跑，靠把 baseUrl 快照传进去解耦。
//
// 热切换：ensureStarted(cfg) 发现「这次要的配置」与「当前进程实际用的配置」不等价时，
// 内部先 shutdown() 再重启。判据见 LocalEngine.cpp 的 sameLaunchConfig()：
// 模型、后端、上下文、线程、GPU 层数、附加参数任何一项变了都算变了——
// 少比一项，用户改了设置点「应用」就会看起来毫无反应（其实还在用旧参数）。
// 旧进程一定会被 terminate/kill 掉（QProcess 析构前 waitForFinished），不留孤儿。
class LocalEngine {
public:
    LocalEngine();
    ~LocalEngine();   // 停进程：退出应用绝不能留下 llama-server

    LocalEngine(const LocalEngine &) = delete;
    LocalEngine &operator=(const LocalEngine &) = delete;

    // 两路来源合并后返回文件名（不含目录）：
    //   1. 清单里 type=="model"、位于 <installRoot>/models/llm 且 AssetRegistry::isPresent()
    //      （存在且非空，不哈希）成立的资产；
    //   2. 用户自己导入的 .gguf —— 住在专门的 models/llm-user 里（见 userLlmModelsDir），
    //      那个目录里躺着的文件都是用户亲手放的，不必再靠登记表"证明"；
    //      另外老版本把导入件放在 models/llm，登记表里记着且文件还在的继续认。
    // 排序按文件名做稳定排序（大小写不敏感，同名再按原始串兜底）：
    // 不用修改时间是因为重下一次资产就会让下拉框顺序整体乱跳，用户点错模型代价是几秒重启。
    static QStringList availableModels();

    // ---- 用户自定义导入（本地 GGUF） ----
    // 把用户挑的 .gguf **复制**进 <installRoot>/models/llm-user，并登记到
    // assets/state/imported-llm.json；源文件留在原地不动。
    // 校验只做两级：文件非空 + 头 4 字节是 "GGUF"。民间量化版对不上清单里的
    // sha256，强制哈希等于禁止导入，所以这里不查清单、也不改清单。
    // 成功时 destName 返回落盘后的文件名（下拉框里就是它）。
    static bool importModelFile(const QString &sourcePath, QString *destName, QString *error);
    // 已登记过的导入模型文件名（文件是否还在由调用方判断）。仅用于兼容老版本落点。
    static QStringList importedModelNames();
    // 用户模型的专用目录：<installRoot>/models/llm-user。
    // 与清单资产（models/llm）物理分开 —— 同名文件不会互相顶掉，
    // 卸载清单资产也永远碰不到这里；代价是这里的内容不参与任何校验。
    static QString userLlmModelsDir();
    // llm-user 里真实存在的文件名（按 availableModels 同一套规则稳定排序）。
    static QStringList userModelFiles();
    // 删掉用户导入的模型：只动 llm-user 里的文件，并把名字从登记表里摘掉。
    static bool removeUserModel(const QString &fileName, QString *error);

    // 运行时目录里有服务端可执行文件就算可用（不探测显卡 / 驱动，扫描保持零成本）。
    static bool isGpuAvailable();   // runtime.llama.vulkan.<平台后缀>
    static bool isCpuAvailable();   // runtime.llama.cpu.<平台后缀>

    // 幂等启动：配置与当前进程等价且服务还活着 -> 什么都不做（lastStartupMs()==0）。
    // 变了 / 进程已死 -> 先停旧的，再按新配置起一个新的。失败时 error 可直接展示。
    bool ensureStarted(const LocalEngineConfig &cfg, QString *error = nullptr);

    void shutdown();
    bool isRunning() const;
    // 「正在拉起中」：ensureStarted() 还没返回。调用方（尤其是 QuickTranslate 的
    // 自动拉起）必须看它，否则一次没起来就会触发第二次、第三次启动 —— 每次都要
    // 重新把上 GB 的权重塞进显存，实测能把整机拖死。
    bool isStarting() const;
    QString baseUrl() const;        // 就绪后 http://127.0.0.1:<port>/v1；未就绪为空串
    QString currentBackend() const; // "vulkan" / "cpu" / ""（未运行）
    QString currentModel() const;   // 正在跑的模型文件名（不是路径）；未运行为空

    // 最近一次 ensureStarted 真正拉起服务花的时间（毫秒）；
    // 0 = 本次没有启动新进程（复用了已经在跑的服务）。
    qint64 lastStartupMs() const;
    int port() const;
    qint64 processId() const;   // 当前 llama-server 的 pid；未运行为 0

    // llama-server 的 stdout/stderr 落盘路径与尾部若干行。
    // 后端 LlamaServer 已经有这两个方法，这里只是转发给界面用。
    QString logFilePath() const;
    QString logTail(int maxLines = 200) const;

    // 供界面 / 排障显示用。
    QString executablePath() const;   // 当前后端对应的 exe 路径
    QString modelPath() const;        // 当前模型绝对路径
    static QString installRoot();
    static QString llmModelsDir();

    // 把「设置里存的模型文件名」翻成绝对路径。空串原样返回空串（不是"第一个模型"）：
    // 调用方要区分「用户没选」和「选的那个不在了」。
    static QString pathForName(const QString &fileName);

    // 显示用短名："model.qwen2.5-0.5b-instruct.q4km" -> "qwen2.5-0.5b"。
    // 纯粹是界面文案，不参与任何路径拼接。
    static QString shortModelName(const QString &fileName);

private:
    std::unique_ptr<studious::runtime::LlamaServer> m_server;
    QString m_backend;       // 当前服务实际用的后端（"vulkan" / "cpu"）
    QString m_modelFile;     // 当前服务加载的模型文件名
    QString m_modelPath;     // 当前服务加载的模型绝对路径
    // 拉起当前进程时调用方给的那份配置。这里存原样、不预先折算：
    // 等价性判断统一走 sameLaunchConfig()（内部折算生效值），
    // 于是「0 与 2048 算同一个配置」这种语义只有一处，不会出现存的和比的口径不一致。
    // shutdown() 会把它清回默认值，保证下次 ensureStarted 必然重新拉起。
    LocalEngineConfig m_runningCfg;
    qint64 m_lastStartupMs = 0;
    // 启动闸门：ensureStarted() 里 exchange(true)，RAII 在返回时清回。
    // 用原子量是因为「自动拉起」这条路上可能同时从 GUI 线程和一个定时器进来。
    std::atomic_bool m_starting{false};
    // 孤儿兜底：正常退出走 shutdown()，被强杀时只有这个能让系统顺手收掉子进程。
    // 实现全在 platform/ProcessGuard.cpp，本文件不含任何平台头。
    studious::platform::ProcessGuard m_guard;
};
