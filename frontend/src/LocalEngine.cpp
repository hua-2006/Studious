#include "LocalEngine.h"

#include "AppSettings.h"
#include "AssetPaths.h"
#include "platform/IPlatform.h"

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

// 运行时 id 由平台层拼出来：Windows 是 runtime.llama.{cpu,vulkan}.win-x64，
// Linux 是 ...linux-x64。以前这里写死 win-x64，Linux 上永远报"缺运行时"。
// 两个运行时是同一份 llama.cpp，只有编译进去的 ggml 后端不同（vulkan 版多带
// ggml-vulkan.dll / libggml-vulkan.so），所以"选后端"= 选 exe 路径 + 决定要不要 -ngl。
QString cpuRuntimeId()
{
    return QStringLiteral("runtime.llama.cpu.") + studious::platform::IPlatform::runtimeAssetSuffix();
}

QString vulkanRuntimeId()
{
    return QStringLiteral("runtime.llama.vulkan.")
        + studious::platform::IPlatform::runtimeAssetSuffix();
}

// 服务端可执行名：Windows "llama-server.exe"，其它平台 "llama-server"。
QString serverExeName()
{
    return studious::platform::IPlatform::serverExecutableName();
}

// 注意：以前这里硬给 -ngl 99（「整个模型上卡」），在 1.5B 上被 llama.cpp 判为
// 「塞不进空闲显存」并 abort，直接把机器拖死。现在「自动」一律不传 -ngl，
// 由 llama.cpp 的 common_fit_params 按实际显存挑层数 —— 少卸载几层也不会崩。

// 一次只翻几句，2048 够用且省内存。界面「0 = 自动」时落到这个值，
// 用户显式填的数不去夹逼——分配失败是用户自己的选择，我们不替他猜。
constexpr int kDefaultContextSize = 2048;

QString runtimeExePath(const QString &runtimeId)
{
    return QDir(LocalEngine::installRoot())
        .filePath(QStringLiteral("runtime/%1/%2").arg(runtimeId, serverExeName()));
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

// 实际会用哪个运行时：只有一个判据，任何地方要问「这次是 Vulkan 还是 CPU」都走这里，
// 免得 ensureStarted 里算一套、比较里算另一套，两边口径一歪就变成永远重启或永远不重启。
QString resolveBackend(bool preferGpu)
{
    return (preferGpu && LocalEngine::isGpuAvailable()) ? QStringLiteral("vulkan")
                                                        : QStringLiteral("cpu");
}

// -ngl 的语义分三种，这里一次说清：
//   负数（-1）= 自动：**不传 -ngl**，让 llama.cpp 自己的 common_fit_params 按空闲显存挑层数。
//                以前这里在 Vulkan 下硬给 99，结果 1.5B 模型上 llama.cpp 直接报
//                「failed to fit params to free device memory: n_gpu_layers already set by
//                user to 99, abort」，把整机显存撑到卡死。自动就交给它自己算。
//   0          = 用户明确要求「一层都不卸载」——Vulkan exe 也照样不传 -ngl（纯 CPU 算，
//                这是 LlamaServerOptions 的既有语义，不是取巧）；
//   正数       = 用户要的层数原样传下去（放不下就让 llama-server 自己报错，别偷偷改）。
int resolvedGpuLayers(int requested, const QString &backend)
{
    Q_UNUSED(backend);
    if (requested >= 0) {
        return requested;
    }
    return 0;
}

// 「用户写的配置」折算成「进程真正吃到的参数」。
// 热切换比较和真正起进程都用这一份，比较的就是实际会变的东西。
struct EffectiveLaunchConfig {
    QString backend;        // "vulkan" / "cpu"
    QString modelPath;      // 绝对路径
    int     contextSize = 0;
    int     threads = 0;
    int     gpuLayers = 0;
    QString extraArgs;
};

EffectiveLaunchConfig effectiveConfig(const LocalEngineConfig &cfg)
{
    EffectiveLaunchConfig out;
    out.backend = resolveBackend(cfg.preferGpu);
    out.modelPath = resolveModelPath(cfg.modelPath);
    out.contextSize = cfg.contextSize > 0 ? cfg.contextSize : kDefaultContextSize;
    out.threads = cfg.threads > 0 ? cfg.threads : preferredThreads(out.backend);
    out.gpuLayers = resolvedGpuLayers(cfg.gpuLayers, out.backend);
    // 附加参数按去掉首尾空白后的串比较：界面在输入框里多敲一个空格不该触发一次十几秒的重启。
    out.extraArgs = cfg.extraArgs.trimmed();
    return out;
}

// 「这次要的配置」与「当前跑着的那份」是否等价——热切换的唯一判据。
// 比的是折算后的生效值，所以 contextSize=0 与 2048、gpuLayers=-1 与 0（都是「不传 -ngl」）算同一个配置，
// 不会因为「自动」和「恰好等于默认值」的写法不同就白重启一次。
// 四项运行参数必须全在比较里：漏一项，用户改完点「应用」界面上就毫无反应。
bool sameLaunchConfig(const LocalEngineConfig &a, const LocalEngineConfig &b)
{
    const EffectiveLaunchConfig ea = effectiveConfig(a);
    const EffectiveLaunchConfig eb = effectiveConfig(b);
    return ea.backend == eb.backend && ea.modelPath == eb.modelPath
        && ea.contextSize == eb.contextSize && ea.threads == eb.threads
        && ea.gpuLayers == eb.gpuLayers && ea.extraArgs == eb.extraArgs;
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
    // 与资产页 / AssetRegistry 用同一个根（含设置在「迁移资产根」后改过的情况）。
    // 以前这里又拼了一遍 %APPDATA%\studious\assets，两边各说各话。
    return studious::ui::paths::assetsRoot();
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

    // 同一个 models/ 目录还躺着视觉模型的单文件资产（ocr.det/rec 的 .onnx、字典 .txt）。
    // 它们是给 OcrEngine 用的，不是 llama 权重，选了只会让 llama-server 启动失败，
    // 所以这里按扩展名挡掉。
    const QStringList nonWeightSuffixes = { QStringLiteral("onnx"),
                                            QStringLiteral("txt"),
                                            QStringLiteral("json"),
                                            QStringLiteral("spm") };

    QStringList names;
    names.reserve(entries.size());
    for (const QFileInfo &entry : entries) {
        if (!entry.isFile())
            continue;
        if (entry.fileName().startsWith(QLatin1String("ocr.")))
            continue;
        if (nonWeightSuffixes.contains(entry.suffix().toLower()))
            continue;
        names << entry.fileName();
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
    return QFileInfo::exists(runtimeExePath(vulkanRuntimeId()));
}

bool LocalEngine::isCpuAvailable()
{
    return QFileInfo::exists(runtimeExePath(cpuRuntimeId()));
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



// ---------------------------------------------------------------- 生命周期

bool LocalEngine::ensureStarted(const LocalEngineConfig &cfg, QString *error)
{
    using studious::i18n::s;
    using studious::i18n::sf;

    // 启动闸门：同一时刻只允许一次真正拉起。
    //
    // 现场教训：实时翻译切到本地 LLM 时，QuickTranslate 每 600 ms 帧、ScreenFlow 每
    // 1 s 重试，每次都会看到一个「没在跑」的引擎 → 各自请求启动。以前没有闸门，
    // 这些请求会排队，每个都先 shutdown() 再重新加载权重、各挑一个端口（日志里
    // 18090…18095 一秒内连开六个）—— 显存被反复占用，最后整机卡死。
    // 现在后到的调用直接拿到「正在启动」，让调用方等这一轮结束再判断。
    if (m_starting.exchange(true)) {
        if (error != nullptr) {
            *error = s("本地引擎正在启动，等它起来再试。",
                       "The local engine is already starting; wait for it to finish.");
        }
        return false;
    }
    struct StartingReset {
        std::atomic_bool &flag;
        ~StartingReset() { flag.store(false); }
    } startingReset{m_starting};

    // 先把「界面给的原样配置」折算成「实际生效的参数」，后面的比较和起进程都用这一份，
    // 保证「判等时以为要跑的」和「真正跑起来的」不可能出现分歧。
    const EffectiveLaunchConfig effective = effectiveConfig(cfg);
    const QString backend = effective.backend;
    const QString exe = runtimeExePath(backend == QLatin1String("vulkan") ? vulkanRuntimeId()
                                                                          : cpuRuntimeId());
    const QString model = effective.modelPath;
    const QString modelFile = QFileInfo(model).fileName();

    // 幂等：配置等价 + 服务还活着 -> 直接复用（不重新加载权重，省十几秒）。
    // 判据全在 sameLaunchConfig 里，含模型、后端、上下文、线程、GPU 层数、附加参数；
    // 任何一项不同都会走到下面的 shutdown() 再重启。
    if (m_server != nullptr && m_server->isRunning() && sameLaunchConfig(m_runningCfg, cfg)) {
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
    // 目录也算"没有模型"：ui 里选「自动（第一个可用）」时，如果上游把 modelsDir 当成
    // 文件路径传下来，llama-server 只会回一句 gguf_init_from_file failed，用户看不懂。
    const QFileInfo modelInfo(model);
    if (!modelInfo.exists() || !modelInfo.isFile()) {
        return fail(sf("本地引擎缺少模型：%1\n请在「资产」页下载这个模型。",
                       "The local engine model is missing: %1\n"
                       "Please download this model on the Assets page.",
                       {QDir::toNativeSeparators(model)}));
    }
    if (!QFileInfo::exists(exe)) {
        return fail(sf("本地引擎缺少运行时：%1\n请在「资产」页下载 %2 或 %3。",
                       "The local engine runtime is missing: %1\nPlease download %2 or %3 "
                       "on the Assets page.",
                       {QDir::toNativeSeparators(exe), cpuRuntimeId(), vulkanRuntimeId()}));
    }
    // 安装后复查：这是「外部字节 → 本机可执行」的唯一通道，光判存在不够 ——
    // 被替换过的 llama-server.exe 一样"存在"。清单里点名的关键文件必须齐全，
    // 运行时资产必须点名了原生可执行文件（AssetRegistry::verifyInstalled）。
    const QString runtimeAssetId =
        backend == QLatin1String("vulkan") ? vulkanRuntimeId() : cpuRuntimeId();
    QString assetError;
    if (!studious::ui::paths::verifyInstalled(runtimeAssetId, &assetError)) {
        return fail(sf("本地引擎运行时校验失败（%1）：%2\n请在「资产」页重新下载这个运行时。",
                       "The local engine runtime failed verification (%1): %2\n"
                       "Please download this runtime again on the Assets page.",
                       {runtimeAssetId, assetError}));
    }

    studious::runtime::LlamaServerOptions options;
    options.executablePath = exe;
    options.modelPath = model;
    // 全部取折算后的值：界面填 0 / -1 / 空 时这里已经换成内置默认或按后端算出来的数。
    options.contextSize = effective.contextSize;
    options.threads = effective.threads;
    // 「自动」= 不传 -ngl，llama.cpp 按空闲显存自己挑层数（显式填的数才原样传）。
    options.gpuLayers = effective.gpuLayers;
    options.extraArgs = effective.extraArgs;
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
    m_guard.attach(processId());
    m_backend = backend;
    m_modelFile = modelFile;
    m_modelPath = model;
    // 只有真正起来了才记「当前这份配置」：下次 ensureStarted 拿它比，
    // 起失败时 m_runningCfg 保持空（fail() 里已 shutdown），下一次必然重试而不是误判成已就绪。
    m_runningCfg = cfg;

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
    // 进程已经停了，孤儿守护也可以撤了（句柄一关，容器就没了）。
    m_guard.release();
    m_backend.clear();
    m_modelFile.clear();
    m_modelPath.clear();
    // 清掉「当前生效配置」：这是热切换判据的另一半，不清就会出现
    // 「进程已经停了但配置还写着旧参数」的错觉，下次 ensureStarted 可能误判成可复用。
    m_runningCfg = LocalEngineConfig{};
    m_lastStartupMs = 0;
}

bool LocalEngine::isRunning() const
{
    return m_server != nullptr && m_server->isRunning();
}

bool LocalEngine::isStarting() const
{
    return m_starting.load();
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

QString LocalEngine::logFilePath() const
{
    return m_server != nullptr ? m_server->logFilePath() : QString();
}

QString LocalEngine::logTail(int maxLines) const
{
    return m_server != nullptr ? m_server->logTail(maxLines) : QString();
}

QString LocalEngine::executablePath() const
{
    // 没跑过就报"如果现在按 preferGpu 会选哪个"——界面在启动前也要显示点东西。
    const QString backend = isRunning()
                                ? m_backend
                                : (isGpuAvailable() ? QStringLiteral("vulkan")
                                                    : QStringLiteral("cpu"));
    return runtimeExePath(backend == QLatin1String("vulkan") ? vulkanRuntimeId() : cpuRuntimeId());
}

QString LocalEngine::modelPath() const
{
    return isRunning() ? m_modelPath : QString();
}
