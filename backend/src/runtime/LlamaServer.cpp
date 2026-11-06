#include "runtime/LlamaServer.h"

#include "i18n/L10n.h"
#include "net/QtHttpClient.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QThread>
#include <QUrl>

#include <utility>

namespace studious::runtime {

namespace {

// 端口从 18080 起往后找，最多试 64 个。
// 为什么不直接用 0（系统随机分配）：llama-server 的端口是我们通过命令行告诉它的，
// 必须先自己挑一个再传进去，没法让它回报"系统给了哪个"。
constexpr int kFirstPort = 18080;
constexpr int kPortScanCount = 64;

// 就绪探测节奏：300ms 一次。实测冷启动约 750ms，所以正常情况下第 3 次就能成功。
constexpr int kPollIntervalMs = 300;

// 单次探测的 HTTP 超时。比轮询间隔大：还在加载权重时端口已经 listen 但不应答，
// 超时太短会频繁取消请求，反而拖慢探测。
constexpr int kProbeTimeoutMs = 5000;

// 失败时带出日志尾部的行数：只要最后几十行就够看出是"模型加载失败"还是"端口占用"。
constexpr int kLogTailLines = 24;

// 日志文件超过这个大小就在下次启动时清空，避免长期使用无限膨胀。
constexpr qint64 kLogRotateBytes = 8 * 1024 * 1024;

#ifdef Q_OS_WIN
// CREATE_NO_WINDOW = 0x08000000。
// 这里手写常量，不引任何平台头文件：backend 保持零平台头依赖
// （验收要求 backend 自身源码里扫不到平台头文件）。
// QProcess 的 modifier 只需要把这一位或进 flags，用不到任何 Windows 类型。
constexpr unsigned long kCreateNoWindow = 0x08000000UL;
#endif

} // namespace

LlamaServer::LlamaServer(LlamaServerOptions options)
    : m_options(std::move(options))
{
}

LlamaServer::~LlamaServer()
{
    stop();
}

// ---------------------------------------------------------------- 启动

bool LlamaServer::start(QString *error)
{
    // 幂等：已经在跑就直接返回成功（前端懒启动会反复调到这里）。
    if (isRunning()) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    // 上一次的残留（半死进程 / 上次失败留下的实例）先清干净再重来。
    stop();
    m_lastError.clear();

    const auto fail = [this, error](const QString &text) {
        recordError(text);
        stop();   // 起不来就别留残留进程
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    // 路径检查放最前面：资产没下载时用户该看到"缺哪个文件"，
    // 而不是一个语焉不详的"进程退出码 1"。
    if (m_options.executablePath.trimmed().isEmpty()
        || !QFileInfo::exists(m_options.executablePath)) {
        return fail(i18n::sf(
            "本地引擎缺少运行时：找不到 %1\n请在「资产」页下载对应的 llama.cpp 运行时。",
            "The local engine runtime is missing: %1 was not found.\nPlease download the "
            "matching llama.cpp runtime on the Assets page.",
            {m_options.executablePath}));
    }
    if (m_options.modelPath.trimmed().isEmpty() || !QFileInfo::exists(m_options.modelPath)) {
        return fail(i18n::sf("本地引擎缺少模型：找不到 %1\n请在「资产」页下载模型文件。",
                             "The local engine model is missing: %1 was not found.\nPlease "
                             "download a model file on the Assets page.",
                             {m_options.modelPath}));
    }

    if (!pickPort(error)) {
        // pickPort 已写好错误文案，这里只补一条记录 + 回收。
        recordError(error != nullptr ? *error : QString());
        return false;
    }
    m_baseUrl = QStringLiteral("http://127.0.0.1:%1/v1").arg(m_port);

    if (!launch(error)) {
        recordError(error != nullptr ? *error : QString());
        stop();
        return false;
    }
    if (!waitUntilReady(error)) {
        // waitUntilReady 内部已带日志尾巴；这里仍要回收进程，否则 5xx 就是常驻内存。
        recordError(error != nullptr ? *error : QString());
        stop();
        return false;
    }

    m_ready = true;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool LlamaServer::pickPort(QString *error)
{
    for (int offset = 0; offset < kPortScanCount; ++offset) {
        const int candidate = kFirstPort + offset;
        // listen 成功 = 这个端口当前没人占（TCP 层验证，比"猜常用端口"靠谱）；
        // 立刻 close 让给 llama-server —— 中间这几十微秒的竞态是已知且可接受的。
        QTcpServer probe;
        if (probe.listen(QHostAddress::LocalHost, candidate)) {
            probe.close();
            m_port = candidate;
            return true;
        }
    }

    m_port = 0;
    const QString text = i18n::sf("本地引擎找不到可用端口（从 %1 起连续 %2 个都被占用）",
                                  "No free port for the local engine (tried %2 ports from %1)",
                                  {QString::number(kFirstPort), QString::number(kPortScanCount)});
    if (error != nullptr) {
        *error = text;
    }
    return false;
}

bool LlamaServer::prepareLogFile(QString *logPath, QString *error)
{
    const QString path = logFilePath();
    const QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        const QString text = i18n::sf("无法创建本地引擎日志目录：%1",
                                      "Could not create the local engine log directory: %1",
                                      {dir.absolutePath()});
        if (error != nullptr) {
            *error = text;
        }
        return false;
    }

    // 超限就清空，再追加本次启动的头部信息（命令行 + 时间），
    // 这样失败时读到的日志尾部一定属于**这一次**启动。
    if (QFileInfo(path).size() > kLogRotateBytes) {
        QFile::remove(path);
    }

    QFile log(path);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        const QString text = i18n::sf("无法写入本地引擎日志：%1（%2）",
                                      "Could not write the local engine log: %1 (%2)",
                                      {path, log.errorString()});
        if (error != nullptr) {
            *error = text;
        }
        return false;
    }
    log.write(QStringLiteral("[studious] %1  start: %2 %3\n")
                  .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                       QDir::toNativeSeparators(m_options.executablePath),
                       arguments().join(QLatin1Char(' ')))
                  .toUtf8());
    log.flush();

    if (logPath != nullptr) {
        *logPath = path;
    }
    return true;
}

bool LlamaServer::launch(QString *error)
{
    QString logPath;
    if (!prepareLogFile(&logPath, error)) {
        return false;
    }

    m_process = std::make_unique<QProcess>();
    m_process->setProgram(m_options.executablePath);
    // 工作目录设成 exe 所在目录：llama-server 会按相对路径找同目录的 DLL / 模板文件。
    m_process->setWorkingDirectory(QFileInfo(m_options.executablePath).absolutePath());
    m_process->setArguments(arguments());

    // 输出一律进日志文件，原因有两条：
    //   1. llama.cpp 往 stderr 刷几百行加载日志，混进我们的控制台输出会把 --smoke 结果淹掉；
    //   2. 启动失败时日志就是唯一线索，读文件比想办法抓管道可靠。
    // 用 MergedChannels + 单文件：合并成一串写进同一个句柄，避免 stdout/stderr 两个句柄
    // 抢着 append 互相覆盖。
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setStandardOutputFile(logPath, QIODevice::Append);

#ifdef Q_OS_WIN
    // 双保险：控制台子系统会继承父进程的控制台窗口，GUI 模式下则可能闪一个黑框。
    // 显式加 CREATE_NO_WINDOW，无论父进程有没有控制台都不弹窗。
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= kCreateNoWindow;
    });
#endif

    m_process->start();
    if (!m_process->waitForStarted(10000)) {
        const QString text =
            i18n::sf("本地引擎无法启动：%1（日志：%2）",
                     "The local engine could not be started: %1 (log: %2)",
                     {m_process->errorString().isEmpty()
                          ? i18n::s("未知原因", "unknown reason")
                          : m_process->errorString(),
                      QDir::toNativeSeparators(logPath)});
        if (error != nullptr) {
            *error = text;
        }
        return false;
    }
    return true;
}

bool LlamaServer::waitUntilReady(QString *error)
{
    // 探测复用现成的 QtHttpClient（同步语义，自带局部事件循环），不再手写一套 HTTP。
    net::QtHttpClient http;
    const QUrl probeUrl(m_baseUrl + QStringLiteral("/models"));

    const int timeoutMs = m_options.startupTimeoutMs > 0 ? m_options.startupTimeoutMs : 120000;
    QElapsedTimer timer;
    timer.start();

    int lastStatus = 0;
    QString lastTransportError;

    while (true) {
        // 进程提前死掉要立刻失败，不能傻等到 startupTimeoutMs。
        // 典型原因：模型文件损坏、命令行参数不认识、端口被别人抢走。
        if (m_process == nullptr || m_process->state() == QProcess::NotRunning) {
            const int code = m_process != nullptr ? m_process->exitCode() : -1;
            // 完整日志只进日志文件/stderr：界面要的是「谁错了、下一步做什么」，
            // 不是二十几行 llama.cpp 的 C 风格输出。
            qWarning("[local] llama-server exited early (code %d), log: %s\n%s",
                     code,
                     qUtf8Printable(logFilePath()),
                     qUtf8Printable(logTail(kLogTailLines)));
            const QString text = i18n::sf(
                "本地引擎启动失败：llama-server 提前退出（退出码 %1）。\n"
                "详细日志见配置页「本地 LLM」的日志面板（%2）。",
                "The local engine failed to start: llama-server exited early (code %1).\n"
                "See the log panel in Configuration → Local LLM (%2) for details.",
                {QString::number(code), QDir::toNativeSeparators(logFilePath())});
            if (error != nullptr) {
                *error = text;
            }
            return false;
        }

        net::HttpRequest req;
        req.url = probeUrl;
        req.method = QByteArrayLiteral("GET");
        req.timeoutMs = kProbeTimeoutMs;
        const net::HttpResponse resp = http.send(req);
        lastStatus = resp.status;
        lastTransportError = resp.error;

        if (resp.ok && resp.status == 200) {
            return true;
        }

        if (timer.elapsed() >= timeoutMs) {
            qWarning("[local] llama-server did not become ready within %lld ms (last probe: HTTP %d %s), log: %s\n%s",
                     static_cast<long long>(timer.elapsed()),
                     lastStatus,
                     qUtf8Printable(lastTransportError),
                     qUtf8Printable(logFilePath()),
                     qUtf8Printable(logTail(kLogTailLines)));
            const QString text = i18n::sf(
                "本地引擎启动超时（%1 ms 内没有就绪，最后一次探测：HTTP %2）。\n"
                "详细日志见配置页「本地 LLM」的日志面板（%3）。",
                "The local engine did not become ready within %1 ms (last probe: HTTP %2).\n"
                "See the log panel in Configuration → Local LLM (%3) for details.",
                {QString::number(timer.elapsed()),
                 QString::number(lastStatus),
                 QDir::toNativeSeparators(logFilePath())});
            if (error != nullptr) {
                *error = text;
            }
            return false;
        }

        QThread::msleep(kPollIntervalMs);
    }
}

// ---------------------------------------------------------------- 停止

void LlamaServer::stop()
{
    m_ready = false;

    if (m_process != nullptr) {
        if (m_process->state() != QProcess::NotRunning) {
            // terminate() 在 Windows 上是发 WM_CLOSE：控制台程序没有窗口，
            // 多半不会自己退，所以 3 秒后必须 kill()。这段时间是有意的等待，
            // 给那些能优雅退出的构建一个体面收场的机会。
            m_process->terminate();
            if (!m_process->waitForFinished(3000)) {
                m_process->kill();
                m_process->waitForFinished(5000);
            }
        }
        m_process.reset();
    }

    m_port = 0;
    m_baseUrl.clear();
}

// ---------------------------------------------------------------- 查询

bool LlamaServer::isRunning() const
{
    return m_ready && m_process != nullptr && m_process->state() != QProcess::NotRunning;
}

QString LlamaServer::baseUrl() const
{
    return m_ready ? m_baseUrl : QString();
}

QString LlamaServer::lastError() const
{
    return m_lastError;
}

int LlamaServer::port() const
{
    return m_port;
}

qint64 LlamaServer::processId() const
{
    // processId() 在未启动时返回 0，正好可以当"没有进程"的哨兵值。
    return m_process != nullptr ? m_process->processId() : 0;
}

// ---------------------------------------------------------------- 内部工具

QStringList LlamaServer::arguments() const
{
    // 与已经实测跑通的命令行严格一致：
    //   llama-server.exe -m <model> --host 127.0.0.1 --port <port> -c 2048 -t 6 --no-webui
    // GPU（Vulkan 运行时）时额外追一个 -ngl <n>：
    //   llama-server.exe -m <model> ... -ngl 99 ...
    QStringList args;
    args << QStringLiteral("-m") << QDir::toNativeSeparators(m_options.modelPath);
    args << QStringLiteral("--host") << QStringLiteral("127.0.0.1");
    args << QStringLiteral("--port") << QString::number(m_port);
    args << QStringLiteral("-c")
         << QString::number(m_options.contextSize > 0 ? m_options.contextSize : 2048);
    if (m_options.threads > 0) {
        args << QStringLiteral("-t") << QString::number(m_options.threads);
    }
    // 0 = 不传：CPU 构建不认这个参数也无所谓，但少一个参数就少一处变量。
    if (m_options.gpuLayers != 0) {
        args << QStringLiteral("-ngl") << QString::number(m_options.gpuLayers);
    }
    // 内置 Web UI 对一个只当后端用的服务毫无用处，还会拖慢启动。
    args << QStringLiteral("--no-webui");

    const QString extra = m_options.extraArgs.trimmed();
    if (!extra.isEmpty()) {
        // 只做最朴素的分隔：带空格的单个参数（例如路径）请用引号自行处理，
        // 这一栏是给排障 / 临时试参用的，不值得引入完整的分词器。
        args << extra.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }
    return args;
}

QString LlamaServer::installRoot() const
{
    // 资产布局是固定的：
    //   <installRoot>/runtime/<runtimeId>/llama-server.exe
    //   <installRoot>/models/<modelId>
    // 优先从模型路径推（模型一定在 <installRoot>/models/ 下，最可靠）；
    // 推不出来再从可执行文件路径推（runtime/<id>/ 上两级）；最后才落到 AppData。
    if (!m_options.modelPath.trimmed().isEmpty()) {
        QDir modelsDir = QFileInfo(m_options.modelPath).absoluteDir();
        if (modelsDir.dirName().compare(QStringLiteral("models"), Qt::CaseInsensitive) == 0) {
            if (modelsDir.cdUp()) {
                return modelsDir.absolutePath();
            }
        }
    }

    if (!m_options.executablePath.trimmed().isEmpty()) {
        QDir runtimeDir = QFileInfo(m_options.executablePath).absoluteDir();   // <root>/runtime/<id>
        if (runtimeDir.cdUp() && runtimeDir.cdUp()) {                          // -> <root>
            return runtimeDir.absolutePath();
        }
    }

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!appData.isEmpty()) {
        return QDir(appData).filePath(QStringLiteral("assets"));
    }
    return QDir::tempPath();
}

QString LlamaServer::logFilePath() const
{
    return QDir(installRoot()).filePath(QStringLiteral("logs/llama-server.log"));
}

QString LlamaServer::logTail(int maxLines) const
{
    QFile log(logFilePath());
    if (!log.open(QIODevice::ReadOnly)) {
        return {};
    }

    // 只读尾部 64 KiB：模型加载日志动辄上千行，整份读进来纯属浪费。
    constexpr qint64 kTailBytes = 64 * 1024;
    const qint64 size = log.size();
    if (size > kTailBytes) {
        log.seek(size - kTailBytes);
    }
    const QString text = QString::fromUtf8(log.readAll());
    log.close();

    QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (maxLines > 0 && lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

void LlamaServer::recordError(const QString &text)
{
    if (!text.isEmpty()) {
        m_lastError = text;
    }
}

} // namespace studious::runtime
