#include "LocalEngine.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

#include <studious/i18n/L10n.h>
#include <studious/runtime/LlamaServer.h>

#include <utility>

namespace {

// 固定资产 id：与 backend/asset/manifest.json 里的 id 一一对应。
// 不做"扫目录挑一个"的模糊匹配——扫出来的可能是别家的 gguf，
// 而 llama-server 只认我们实测过的这一套（Qwen2.5-0.5B-Instruct Q4_K_M）。
const char *const kRuntimeAssetId = "runtime.llama.cpu.win-x64";
const char *const kModelAssetId = "model.qwen2.5-0.5b-instruct.q4km";
const char *const kServerExeName = "llama-server.exe";

// 实测命令行用的是 -t 6（12 线程机器上留出余量给界面与系统）。
// 线程更少的机器上夹到可用核数，避免 llama.cpp 开一堆空转线程。
int preferredThreads()
{
    const int cores = QThread::idealThreadCount();
    if (cores <= 0) {
        return 6;   // 探测失败就按实测值来
    }
    return qBound(1, cores, 6);
}

} // namespace

LocalEngine::LocalEngine() = default;

LocalEngine::~LocalEngine()
{
    shutdown();
}

QString LocalEngine::installRoot()
{
    // 与资产页 / AssetRegistry 用的是同一个根：%APPDATA%\studious\assets
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("assets"));
}

QString LocalEngine::executablePath() const
{
    return QDir(installRoot())
        .filePath(QString::fromLatin1("runtime/%1/%2")
                      .arg(QString::fromLatin1(kRuntimeAssetId),
                           QString::fromLatin1(kServerExeName)));
}

QString LocalEngine::modelPath() const
{
    return QDir(installRoot())
        .filePath(QStringLiteral("models/%1").arg(QString::fromLatin1(kModelAssetId)));
}

bool LocalEngine::ensureStarted(QString *error)
{
    // 复用：进程还在 + 上次已经探测就绪，直接返回，不重复加载模型。
    if (m_server != nullptr && m_server->isRunning()) {
        m_lastStartupMs = 0;
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    m_lastStartupMs = 0;
    m_server.reset();   // 陈旧实例（被外部杀掉 / 上次启动失败）一律丢掉重建

    // 资产检查放在起进程之前：缺文件时给"缺哪个"，而不是让 llama-server 报个退出码。
    const QString exe = executablePath();
    if (!QFileInfo::exists(exe)) {
        const QString text = studious::i18n::sf(
            "本地引擎缺少运行时：%1\n请在「资产」页下载 %2。",
            "The local engine runtime is missing: %1\nPlease download %2 on the Assets page.",
            {QDir::toNativeSeparators(exe), QString::fromLatin1(kRuntimeAssetId)});
        if (error != nullptr) {
            *error = text;
        }
        return false;
    }

    const QString model = modelPath();
    if (!QFileInfo::exists(model)) {
        const QString text = studious::i18n::sf(
            "本地引擎缺少模型：%1\n请在「资产」页下载 %2。",
            "The local engine model is missing: %1\nPlease download %2 on the Assets page.",
            {QDir::toNativeSeparators(model), QString::fromLatin1(kModelAssetId)});
        if (error != nullptr) {
            *error = text;
        }
        return false;
    }

    studious::runtime::LlamaServerOptions options;
    options.executablePath = exe;
    options.modelPath = model;
    options.contextSize = 2048;   // 一次只翻几句，2048 够用且省内存
    options.threads = preferredThreads();
    options.startupTimeoutMs = 120000;

    auto server = std::make_unique<studious::runtime::LlamaServer>(std::move(options));

    QElapsedTimer timer;
    timer.start();
    QString startError;
    if (!server->start(&startError)) {
        // start() 内部已经回收进程，这里只需把错误往上抛。
        if (error != nullptr) {
            *error = startError.isEmpty()
                         ? studious::i18n::s("本地引擎启动失败（未给出原因）",
                                             "The local engine failed to start (no reason given).")
                         : startError;
        }
        return false;
    }

    m_lastStartupMs = timer.elapsed();
    m_server = std::move(server);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void LocalEngine::shutdown()
{
    if (m_server != nullptr) {
        m_server->stop();
        m_server.reset();
    }
    m_lastStartupMs = 0;
}

bool LocalEngine::isRunning() const
{
    return m_server != nullptr && m_server->isRunning();
}

QString LocalEngine::baseUrl() const
{
    return m_server != nullptr ? m_server->baseUrl() : QString();
}

qint64 LocalEngine::lastStartupMs() const
{
    return m_lastStartupMs;
}

int LocalEngine::port() const
{
    return m_server != nullptr ? m_server->port() : 0;
}
