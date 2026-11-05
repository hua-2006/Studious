#pragma once

#include <QString>
#include <QStringList>

#include <memory>

class QProcess;

namespace studious::runtime {

// 本地 llama.cpp 服务（llama-server.exe）的生命周期管理。
//
// 为什么单独成类而不是塞进前端管理器：
//   「选端口 / 拉进程 / 等就绪 / 读日志尾巴 / 强制回收」这几件事各自都有坑，
//   收在一个类里才能保证「析构即停进程」这条硬约束不被别处破坏——
//   泄漏一个 llama-server 就是十几 MB 常驻内存 + 一个占着端口的孤儿进程。
//
// 线程约束：QProcess 是线程亲和的。本类必须**在构造它的线程内使用和销毁**
// （前端固定在主线程持有）。start() 里有阻塞轮询（最长 startupTimeoutMs，实测约 1s），
// 调用方要自己接受这段阻塞。
struct LlamaServerOptions {
    QString executablePath;     // llama-server.exe 绝对路径
    QString modelPath;          // .gguf 绝对路径
    int     contextSize = 2048;
    int     threads = 0;        // 0 = 交给 llama.cpp 自己决定
    // 卸载到 GPU 的层数（命令行 -ngl）。0 = 不传这个参数，完全走 CPU。
    // Vulkan 构建必须给一个足够大的数（实测 99 才能把整个模型放上显卡）：
    // 只换 Vulkan 版 exe 而不传 -ngl，它照样用 CPU 算，日志里也不会出现 device Vulkan0。
    int     gpuLayers = 0;
    int     startupTimeoutMs = 120000;
    QString extraArgs;          // 可选，空格分隔
};

class LlamaServer {
public:
    explicit LlamaServer(LlamaServerOptions options);
    ~LlamaServer();   // 必须 stop()：不留残留进程

    LlamaServer(const LlamaServer &) = delete;
    LlamaServer &operator=(const LlamaServer &) = delete;

    // 启动并阻塞等待 /v1/models 就绪；失败写 error（中文，双语用 i18n::s）。
    // 失败时进程一定已经被回收，不会留半死的 llama-server。
    bool start(QString *error = nullptr);

    // 先 terminate()，3 秒不退再 kill()，并 waitForFinished()。可重复调用。
    void stop();

    bool isRunning() const;   // 进程在跑且 /v1/models 已经探测成功过
    QString baseUrl() const;  // 就绪后形如 http://127.0.0.1:12345/v1；未就绪为空串
    QString lastError() const;
    int port() const;         // 实际选中的端口；未启动为 0
    qint64 processId() const; // 子进程 pid；未启动为 0（热切换时用来证明进程真的换了）

private:
    bool pickPort(QString *error);
    bool prepareLogFile(QString *logPath, QString *error);
    bool launch(QString *error);
    bool waitUntilReady(QString *error);

    QStringList arguments() const;
    QString installRoot() const;
    QString logFilePath() const;
    QString logTail(int maxLines) const;
    void recordError(const QString &text);

    LlamaServerOptions m_options;
    std::unique_ptr<QProcess> m_process;
    QString m_lastError;
    QString m_baseUrl;
    int m_port = 0;
    bool m_ready = false;
};

} // namespace studious::runtime
