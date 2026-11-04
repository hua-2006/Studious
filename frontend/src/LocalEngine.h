#pragma once

#include <QString>

#include <memory>

namespace studious::runtime {
class LlamaServer;
}

// 本地 llama.cpp 引擎的轻量管理器（**主线程持有**）。
//
// 定位：把「资产在哪 / 什么时候拉起 / 起来后地址是多少」这三件事收在一处，
// 让 MainWindow 与 --smoke 用同一套逻辑，不必各自拼路径。
//
// 资产布局（固定 id，不做可配置——离线翻译只支持这一个 CPU 运行时 + 这一个模型）：
//   runtime: <installRoot>/runtime/runtime.llama.cpu.win-x64/llama-server.exe
//   model  : <installRoot>/models/model.qwen2.5-0.5b-instruct.q4km
//
// 线程约束（重要）：ensureStarted() 会阻塞约 1 秒且内部是 QProcess（主线程亲和），
// **只能在主线程调用**；翻译本身仍然在工作线程里跑，靠把 baseUrl 快照传进去解耦。
class LocalEngine {
public:
    LocalEngine();
    ~LocalEngine();   // 停进程：退出应用绝不能留下 llama-server

    LocalEngine(const LocalEngine &) = delete;
    LocalEngine &operator=(const LocalEngine &) = delete;

    // 懒启动：第一次调用才拉进程，之后直接复用。失败时写 error（已本地化，可直接展示）。
    bool ensureStarted(QString *error = nullptr);

    void shutdown();
    bool isRunning() const;
    QString baseUrl() const;   // 就绪后 http://127.0.0.1:<port>/v1；未就绪为空串

    // 最近一次 ensureStarted 真正拉起服务花的时间（毫秒）；
    // 0 = 本次没有启动新进程（复用了已经在跑的服务）。
    qint64 lastStartupMs() const;
    int port() const;

    // 供界面 / 排障显示用。
    QString executablePath() const;
    QString modelPath() const;
    static QString installRoot();

private:
    std::unique_ptr<studious::runtime::LlamaServer> m_server;
    qint64 m_lastStartupMs = 0;
};
