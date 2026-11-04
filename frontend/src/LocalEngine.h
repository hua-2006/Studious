#pragma once

#include <QString>
#include <QStringList>

#include <memory>

namespace studious::runtime {
class LlamaServer;
}

// 启动本地引擎时的一次性参数快照（值语义，调用方可以随手构造 / 丢弃）。
struct LocalEngineConfig {
    // <root>/models/<file>。也可以只给文件名（相对路径），内部会拼到 models 目录下。
    // 空 = 用 availableModels() 的第一个（与界面「空 = 第一个可用模型」的约定一致）。
    QString modelPath;
    // true 且 Vulkan 运行时存在 -> 用 Vulkan（带 -ngl 99）；否则回落 CPU（不传 -ngl）。
    bool    preferGpu = true;
};

// 本地 llama.cpp 引擎的轻量管理器（**主线程持有**）。
//
// 定位：把「选哪个运行时 / 用哪个模型 / 什么时候重启 / 起来后地址是多少」收在一处，
// 让 MainWindow 与 --smoke / --switch-demo 用同一套逻辑，不必各自拼路径。
//
// 资产布局：
//   runtime: <installRoot>/runtime/runtime.llama.{cpu,vulkan}.win-x64/llama-server.exe
//   model  : <installRoot>/models/<任意普通文件>（我们的资产就是无扩展名文件）
//
// 线程约束（重要）：ensureStarted() 会阻塞数百毫秒到数秒（GPU 冷启动实测约 2.2 秒），
// 且内部是 QProcess（主线程亲和），**只能在主线程调用**；
// 翻译本身仍然在工作线程里跑，靠把 baseUrl 快照传进去解耦。
//
// 热切换：ensureStarted(cfg) 发现模型或后端与当前运行的不同时，内部先 shutdown() 再重启。
// 旧进程一定会被 terminate/kill 掉（QProcess 析构前 waitForFinished），不留孤儿。
class LocalEngine {
public:
    LocalEngine();
    ~LocalEngine();   // 停进程：退出应用绝不能留下 llama-server

    LocalEngine(const LocalEngine &) = delete;
    LocalEngine &operator=(const LocalEngine &) = delete;

    // 扫描 <installRoot>/models/ 下的普通文件，返回文件名（不含目录）。
    // 排序按文件名做稳定排序（大小写不敏感，同名再按原始串兜底）：
    // 不用修改时间是因为重下一次资产就会让下拉框顺序整体乱跳，用户点错模型代价是几秒重启。
    static QStringList availableModels();

    // 运行时目录里有 llama-server.exe 就算可用（不探测显卡 / 驱动，扫描保持零成本）。
    static bool isGpuAvailable();   // runtime.llama.vulkan.win-x64\llama-server.exe
    static bool isCpuAvailable();   // runtime.llama.cpu.win-x64\llama-server.exe

    // 幂等启动：模型与后端都没变且服务还活着 -> 什么都不做（lastStartupMs()==0）。
    // 变了 / 进程已死 -> 先停旧的，再按新配置起一个新的。失败时 error 可直接展示。
    bool ensureStarted(const LocalEngineConfig &cfg, QString *error = nullptr);

    void shutdown();
    bool isRunning() const;
    QString baseUrl() const;        // 就绪后 http://127.0.0.1:<port>/v1；未就绪为空串
    QString currentBackend() const; // "vulkan" / "cpu" / ""（未运行）
    QString currentModel() const;   // 正在跑的模型文件名（不是路径）；未运行为空

    // 最近一次 ensureStarted 真正拉起服务花的时间（毫秒）；
    // 0 = 本次没有启动新进程（复用了已经在跑的服务）。
    qint64 lastStartupMs() const;
    int port() const;
    qint64 processId() const;   // 当前 llama-server 的 pid；未运行为 0

    // 供界面 / 排障显示用。
    QString executablePath() const;   // 当前后端对应的 exe 路径
    QString modelPath() const;        // 当前模型绝对路径
    static QString installRoot();
    static QString modelsDir();

    // 显示用短名："model.qwen2.5-0.5b-instruct.q4km" -> "qwen2.5-0.5b"。
    // 纯粹是界面文案，不参与任何路径拼接。
    static QString shortModelName(const QString &fileName);
    // "vulkan" -> "Vulkan"；"cpu" -> "CPU"；其余原样返回。
    static QString backendLabel(const QString &backend);

private:
    std::unique_ptr<studious::runtime::LlamaServer> m_server;
    QString m_backend;       // 当前服务实际用的后端（"vulkan" / "cpu"）
    QString m_modelFile;     // 当前服务加载的模型文件名
    QString m_modelPath;     // 当前服务加载的模型绝对路径
    qint64 m_lastStartupMs = 0;
};
