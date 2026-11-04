#include "LocalEngine.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

#include <studious/i18n/L10n.h>
#include <studious/runtime/LlamaServer.h>

#include <algorithm>
#include <utility>

namespace {

// 固定资产 id：与 backend/asset/manifest.json 里的 id 一一对应。
// 两个运行时是同一份 llama.cpp b12102，只有编译进去的 ggml 后端不同（vulkan 版多带
// ggml-vulkan.dll），所以"选后端"= 选 exe 路径 + 决定要不要 -ngl。
const char *const kCpuRuntimeId = "runtime.llama.cpu.win-x64";
const char *const kVulkanRuntimeId = "runtime.llama.vulkan.win-x64";
const char *const kServerExeName = "llama-server.exe";

// 实测结论：Vulkan 构建必须显式给 -ngl 99（整个模型上卡）。
// 只把 exe 换成 vulkan 版而不传 -ngl，它照样用 CPU 算——启动日志里也不会出现 device Vulkan0。
constexpr int kVulkanGpuLayers = 99;

QString runtimeExePath(const QString &runtimeId)
{
    return QDir(LocalEngine::installRoot())
        .filePath(QStringLiteral("runtime/%1/%2")
                      .arg(runtimeId, QString::fromLatin1(kServerExeName)));
}

// 线程数：CPU 实测 -t 6（12 线程机器留余量给界面与系统）。
// GPU 上线程只影响采样 / prompt 处理，给到 8 也不抢显卡，但仍夹在物理核数以内。
int preferredThreads(const QString &backend)
{
    const int cores = QThread::idealThreadCount();
    const int cap = (backend == QLatin1String("vulkan")) ? 8 : 6;
    if (cores <= 0) {
        return cap;   // 探测失败就按实测值来
    }
    return qBound(1, cores, cap);
}

// 把"用户给的模型"解析成绝对路径。
//   空            -> availableModels() 的第一个（界面「空 = 第一个可用模型」）
//   文件名 / 相对 -> <root>/models/<name>（不按 cwd 解析：快捷方式启动时 cwd 不可控）
//   绝对路径      -> 原样用
QString resolveModelPath(const QString &requested)
{
    const QString trimmed = requested.trimmed();
    if (!trimmed.isEmpty()) {
        const QFileInfo info(trimmed);
        return info.isAbsolute() ? info.absoluteFilePath()
                                 : QDir(LocalEngine::modelsDir()).filePath(trimmed);
    }
    const QStringList models = LocalEngine::availableModels();
    if (models.isEmpty()) {
        return QString();
    }
    return QDir(LocalEngine::modelsDir()).filePath(models.first());
}

} // namespace

LocalEngine::LocalEngine() = default;

LocalEngine::~LocalEngine()
{
    shutdown();
}

// ---------------------------------------------------------------- 静态：资产查询

QString LocalEngine::installRoot()
{
    // 与资产页 / AssetRegistry 用的是同一个根：%APPDATA%\studious\assets
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("assets"));
}

QString LocalEngine::modelsDir()
{
    return QDir(installRoot()).filePath(QStringLiteral("models"));
}

QStringList LocalEngine::availableModels()
{
    const QDir dir(modelsDir());
    if (!dir.exists()) {
        return {};
    }

    // 只收普通文件：我们的资产就是 models/ 下的无扩展名文件，
    // 目录（比如下载中的临时目录）不能进下拉框，否则选中就会启动失败。
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);

    QStringList names;
    names.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        if (entry.isFile()) {
            names << entry.fileName();
        }
    }

    // 稳定且确定的顺序：先大小写不敏感，再按原始串兜底（避免只差大小写时顺序随平台变）。
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int cmp = QString::compare(a, b, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a < b;
    });
    return names;
}

bool LocalEngine::isGpuAvailable()
{
    return QFileInfo::exists(runtimeExePath(QString::fromLatin1(kVulkanRuntimeId)));
}

bool LocalEngine::isCpuAvailable()
{
    return QFileInfo::exists(runtimeExePath(QString::fromLatin1(kCpuRuntimeId)));
}

QString LocalEngine::shortModelName(const QString &fileName)
{
    QString name = fileName.trimmed();
    if (name.startsWith(QLatin1String("model."), Qt::CaseInsensitive)) {
        name.remove(0, 6);
    }
    for (const char *suffix : {".q4km", ".q4_k_m", ".gguf", ".bin"}) {
        const QString tail = QString::fromLatin1(suffix);
        if (name.endsWith(tail, Qt::CaseInsensitive)) {
            name.chop(tail.size());
            break;
        }
    }
    if (name.endsWith(QLatin1String("-instruct"), Qt::CaseInsensitive)) {
        name.chop(9);
    }
    // 兜底：文件名不符合我们的命名习惯时原样显示，别把名字切没了。
    return name.isEmpty() ? fileName : name;
}

QString LocalEngine::backendLabel(const QString &backend)
{
    if (backend == QLatin1String("vulkan")) {
        return QStringLiteral("Vulkan");
    }
    if (backend == QLatin1String("cpu")) {
        return QStringLiteral("CPU");
    }
    return backend;
}

// ---------------------------------------------------------------- 生命周期

bool LocalEngine::ensureStarted(const LocalEngineConfig &cfg, QString *error)
{
    using studious::i18n::s;
    using studious::i18n::sf;

    // 后端：preferGpu 且有 vulkan 运行时 -> vulkan，否则 cpu。
    const QString backend = (cfg.preferGpu && isGpuAvailable()) ? QStringLiteral("vulkan")
                                                               : QStringLiteral("cpu");
    const QString exe = runtimeExePath(backend == QLatin1String("vulkan")
                                           ? QString::fromLatin1(kVulkanRuntimeId)
                                           : QString::fromLatin1(kCpuRuntimeId));
    const QString model = resolveModelPath(cfg.modelPath);
    const QString modelFile = QFileInfo(model).fileName();

    // 幂等：模型 + 后端 + 路径都没变，且服务还活着 -> 直接复用（不重新加载权重）。
    // 热切换的判据就是这一段：任何一项不同，下面都会先 shutdown() 再重启。
    if (m_server != nullptr && m_server->isRunning() && m_backend == backend
        && m_modelFile == modelFile && m_modelPath == model) {
        m_lastStartupMs = 0;
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }

    // 旧服务先停干净：这是"不留孤儿进程"的唯一保证。
    // 放在所有检查之前——哪怕下面因为缺文件直接失败，也不会有个旧 llama-server 还占着端口。
    shutdown();

    const auto fail = [this, error](const QString &text) {
        m_lastStartupMs = 0;
        shutdown();   // 起不来就别留残留进程
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };

    // 资产检查放在起进程之前：缺文件时给"缺哪个"，而不是让 llama-server 报个退出码。
    if (model.isEmpty()) {
        return fail(sf("本地引擎没有可用模型：%1 里没有任何文件。\n请在「资产」页下载模型。",
                       "The local engine has no model available: %1 contains no files.\n"
                       "Please download a model on the Assets page.",
                       {QDir::toNativeSeparators(modelsDir())}));
    }
    if (!QFileInfo::exists(model)) {
        return fail(sf("本地引擎缺少模型：%1\n请在「资产」页下载这个模型。",
                       "The local engine model is missing: %1\n"
                       "Please download this model on the Assets page.",
                       {QDir::toNativeSeparators(model)}));
    }
    if (!QFileInfo::exists(exe)) {
        return fail(sf("本地引擎缺少运行时：%1\n请在「资产」页下载 runtime.llama.cpu.win-x64 或 "
                       "runtime.llama.vulkan.win-x64。",
                       "The local engine runtime is missing: %1\nPlease download "
                       "runtime.llama.cpu.win-x64 or runtime.llama.vulkan.win-x64 on the "
                       "Assets page.",
                       {QDir::toNativeSeparators(exe)}));
    }

    studious::runtime::LlamaServerOptions options;
    options.executablePath = exe;
    options.modelPath = model;
    options.contextSize = 2048;   // 一次只翻几句，2048 够用且省内存
    options.threads = preferredThreads(backend);
    // CPU 不传 -ngl；Vulkan 必须给足层数，否则等于白装一个 40MB 的 DLL。
    options.gpuLayers = backend == QLatin1String("vulkan") ? kVulkanGpuLayers : 0;
    options.startupTimeoutMs = 120000;

    auto server = std::make_unique<studious::runtime::LlamaServer>(std::move(options));

    QElapsedTimer timer;
    timer.start();
    QString startError;
    if (!server->start(&startError)) {
        // start() 内部已经回收进程，这里只需把错误往上抛。
        return fail(startError.isEmpty()
                        ? s("本地引擎启动失败（未给出原因）",
                            "The local engine failed to start (no reason given).")
                        : startError);
    }

    m_lastStartupMs = timer.elapsed();
    m_server = std::move(server);
    m_backend = backend;
    m_modelFile = modelFile;
    m_modelPath = model;

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void LocalEngine::shutdown()
{
    if (m_server != nullptr) {
        // stop() 最长等 3 秒 terminate + 5 秒 kill：调用的都是主线程，
        // 调用方负责先给用户一句"正在停止…"再进来。
        m_server->stop();
        m_server.reset();
    }
    m_backend.clear();
    m_modelFile.clear();
    m_modelPath.clear();
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

QString LocalEngine::currentBackend() const
{
    return isRunning() ? m_backend : QString();
}

QString LocalEngine::currentModel() const
{
    return isRunning() ? m_modelFile : QString();
}

qint64 LocalEngine::lastStartupMs() const
{
    return m_lastStartupMs;
}

int LocalEngine::port() const
{
    return m_server != nullptr ? m_server->port() : 0;
}

qint64 LocalEngine::processId() const
{
    return m_server != nullptr ? m_server->processId() : 0;
}

QString LocalEngine::executablePath() const
{
    // 没跑过就报"如果现在按 preferGpu 会选哪个"——界面在启动前也要显示点东西。
    const QString backend = isRunning()
                                ? m_backend
                                : (isGpuAvailable() ? QStringLiteral("vulkan")
                                                    : QStringLiteral("cpu"));
    return runtimeExePath(backend == QLatin1String("vulkan")
                              ? QString::fromLatin1(kVulkanRuntimeId)
                              : QString::fromLatin1(kCpuRuntimeId));
}

QString LocalEngine::modelPath() const
{
    return isRunning() ? m_modelPath : QString();
}
