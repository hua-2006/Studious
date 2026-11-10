#include "LocalEngine.h"

#include "AppSettings.h"
#include "AssetPaths.h"
#include "platform/IPlatform.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <studious/asset/AssetRegistry.h>
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
    const QString direct = LocalEngine::pathForName(requested);
    if (!direct.isEmpty()) {
        return direct;
    }
    const QStringList models = LocalEngine::availableModels();
    if (models.isEmpty()) {
        return QString();
    }
    return QDir(LocalEngine::llmModelsDir()).filePath(models.first());
}

// 实际会用哪个运行时：只有一个判据，任何地方要问「这次是 Vulkan 还是 CPU」都走这里，
// 免得 ensureStarted 里算一套、比较里算另一套，两边口径一歪就变成永远重启或永远不重启。
QString resolveBackend(bool preferGpu)
{
    // 只装了 Vulkan 运行时（没有 CPU 版 exe）时，preferGpu=false 只是让引擎起不来，
    // 不是省资源：这里按实际装了什么兜底，保证"有得跑"永远优先于"用户偏好"。
    // 想真正走纯 CPU，用「GPU 层数 = 0」表达（Vulkan exe 也照样不卸载层）。
    if (!preferGpu && !LocalEngine::isCpuAvailable() && LocalEngine::isGpuAvailable()) {
        return QStringLiteral("vulkan");
    }
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

QString LocalEngine::llmModelsDir()
{
    // 只有本地 LLM 的 GGUF 在这里；模型目录按用途分了三格（llm / mt / ocr），
    // 平铺在一起时 llama-server、opus-mt 和 OCR 的条目互相看起来都像"模型"。
    return studious::ui::paths::modelsLlmDir();
}

QString LocalEngine::userLlmModelsDir()
{
    // 用户导入的模型单独一格：清单资产是"可校验的官方件"，这里是"自担的民间件"，
    // 混在一个目录里会互相顶掉（同名就装不进去），也没法一眼看出哪条是自己放的。
    return QDir(studious::ui::paths::assetsRoot()).filePath(QStringLiteral("models/llm-user"));
}

QStringList LocalEngine::userModelFiles()
{
    QStringList names;
    const QFileInfoList files =
        QDir(userLlmModelsDir()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : files) {
        if (info.size() > 0) {
            names << info.fileName();
        }
    }
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int cmp = QString::compare(a, b, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a < b;
    });
    return names;
}

QStringList LocalEngine::availableModels()
{
    // 单一真源：清单 + AssetRegistry::isPresent()（存在且非空，不哈希）。
    //
    // 以前这里自己扫 <root>/models/llm，再用后缀白名单（onnx/txt/json/spm）挡掉
    // OCR 的伴随文件 —— 那套只有"目录里躺着的都是 llama 权重"时才成立：
    //   · 用户手放一个别的 .gguf 也算"可用"，选中后 llama-server 直接启动失败；
    //   · 目录里出现新的非权重文件（比如 tokenizer 副本）就得再往白名单里加一条。
    // 现在只认清单里 type=="model" 的条目，且 AssetRegistry 认为它真的装好了。
    studious::asset::Manifest manifest;
    if (!studious::ui::paths::loadManifest(&manifest)) {
        // 清单读不到就当作"没有模型"：宁可让界面报"缺资产"，也不凭文件名猜一个能启动的东西。
        return {};
    }

    studious::asset::AssetRegistry registry(installRoot(), manifest);
    const QString llmDir = QDir::cleanPath(llmModelsDir());

    QStringList names;
    const QVector<studious::asset::AssetStatus> statuses = registry.listStatus();
    names.reserve(statuses.size());
    for (const studious::asset::AssetStatus &status : statuses) {
        if (!registry.isPresent(status.id)) {
            continue;
        }

        const studious::asset::AssetEntry *entry = manifest.find(status.id);
        if (entry == nullptr || !entry->isModel()) {
            continue;
        }

        // 清单里 type=="model" 的不只有 LLM：本地 MT（models/mt/<id>，目录）与 OCR 三件套
        // （models/ocr/...）也是 model。交给 llama-server 只会启动失败，所以按布局筛：
        // LLM 一律在 models/llm 下（见 AssetRegistry::resolveTarget），不靠 id 前缀猜。
        const QFileInfo info(status.installPath);
        if (!info.isFile() || QDir::cleanPath(info.absolutePath()) != llmDir) {
            continue;
        }
        names << info.fileName();
    }

    // 用户导入的模型住自己的目录（models/llm-user，与清单资产物理分开，同名也不会互相顶掉）。
    // 那个目录里的文件都是用户亲手放的，不需要登记表来"证明"；只用 size>0 挡掉写了一半的。
    for (const QString &name : userModelFiles()) {
        if (!names.contains(name)) {
            names << name;
        }
    }

    // 兼容老版本：导入件曾经直接放在 models/llm，登记表里还记着、文件也还在的继续认
    // （新导入一律进 llm-user，见 importModelFile）。
    for (const QString &name : importedModelNames()) {
        if (names.contains(name)) {
            continue;
        }
        const QFileInfo info(QDir(llmDir).filePath(name));
        if (!info.isFile() || info.size() <= 0) {
            continue;
        }
        names << name;
    }

    // 稳定且确定的顺序：先大小写不敏感，再按原始串兜底（避免只差大小写时顺序随平台变）。
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        const int cmp = QString::compare(a, b, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : a < b;
    });
    return names;
}

namespace {

// 导入模型的登记表：assets/state/imported-llm.json。
// 它不参与清单校验 —— 清单仍是官方资产的单一真源；这张表只补一个缺口：
// 清单外的 gguf 也得能在下拉里出现并真的被 llama-server 加载。
QString importedRegistryPath()
{
    return QDir(studious::ui::paths::assetsRoot()).filePath(QStringLiteral("state/imported-llm.json"));
}

// 小于 1 MB 的"模型"只可能是坏文件或下了一半的重定向页面，不必交给 llama-server 丢人。
constexpr qint64 kMinImportedModelBytes = 1024 * 1024;

}  // namespace

QStringList LocalEngine::importedModelNames()
{
    QFile file(importedRegistryPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    QStringList names;
    const QJsonArray files = doc.object().value(QStringLiteral("files")).toArray();
    names.reserve(files.size());
    for (const QJsonValue &value : files) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString().trimmed();
        // 只接受纯文件名：登记表被手改成 "../x.gguf" 之类不应该能决定路径。
        if (!name.isEmpty() && name == QFileInfo(name).fileName()) {
            names << name;
        }
    }
    return names;
}

bool LocalEngine::importModelFile(const QString &sourcePath, QString *destName, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const QFileInfo source(sourcePath.trimmed());
    if (!source.exists() || !source.isFile()) {
        return fail(studious::i18n::s("找不到要导入的模型文件。",
                                      "The model file to import does not exist."));
    }
    if (source.size() < kMinImportedModelBytes) {
        return fail(studious::i18n::s("文件太小，不像是 GGUF 模型（至少 1 MB）。",
                                      "The file is too small to be a GGUF model (needs at least 1 MB)."));
    }

    // 只看文件头：GGUF 魔数 + 版本号。再往下校验就等于自己实现一遍 llama.cpp 的
    // 模型加载器，索性把"这个权重能不能跑"交给 llama-server 自己回答。
    {
        QFile probe(source.absoluteFilePath());
        if (!probe.open(QIODevice::ReadOnly)) {
            return fail(studious::i18n::s("读不了这个文件（可能被占用或权限不足）。",
                                          "Cannot read the file (in use or permission denied)."));
        }
        const QByteArray head = probe.read(8);
        probe.close();
        if (head.size() < 8 || !head.startsWith(QByteArrayLiteral("GGUF"))) {
            return fail(studious::i18n::s("这不是 GGUF 文件（文件头不是 GGUF）。本地 LLM 只认 .gguf。",
                                          "Not a GGUF file (missing the GGUF header). Local LLM only takes .gguf."));
        }
    }

    const QString name = source.fileName();
    const QDir dir(userLlmModelsDir());
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        return fail(studious::i18n::sf("无法创建模型目录：%1", "Cannot create the model directory: %1",
                                       {dir.absolutePath()}));
    }

    const QString dest = dir.filePath(name);
    const bool sameFile = QFileInfo(dest).absoluteFilePath() == source.absoluteFilePath();
    if (!sameFile) {
        // 复制，不移动：用户的原始文件留在下载目录里，删掉应用目录也不会把他的东西搞丢。
        if (QFileInfo::exists(dest)) {
            return fail(studious::i18n::sf(
                "用户模型目录里已经有同名文件「%1」，先删掉或用别的名字。",
                "A user model with the same name already exists: %1. Remove it or rename the file.",
                {name}));
        }
        if (!QFile::copy(source.absoluteFilePath(), dest)) {
            return fail(studious::i18n::sf("复制失败（磁盘空间不足或目标文件被占用）：%1",
                                           "Copy failed (out of disk space or the target is locked): %1",
                                           {dest}));
        }
    }

    // 登记（幂等：同名条目覆盖）。写临时文件再提交，掉电不会留下半个 JSON。
    QJsonArray previous;
    {
        QFile existing(importedRegistryPath());
        if (existing.open(QIODevice::ReadOnly)) {
            previous = QJsonDocument::fromJson(existing.readAll())
                           .object()
                           .value(QStringLiteral("files"))
                           .toArray();
            existing.close();
        }
    }

    QJsonArray files;
    for (const QJsonValue &value : previous) {
        if (value.toObject().value(QStringLiteral("name")).toString() != name) {
            files.append(value);
        }
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("bytes"), static_cast<double>(QFileInfo(dest).size()));
    entry.insert(QStringLiteral("source"), source.absoluteFilePath());
    entry.insert(QStringLiteral("importedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    files.append(entry);

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("files"), files);

    QDir().mkpath(QFileInfo(importedRegistryPath()).absolutePath());
    QSaveFile registry(importedRegistryPath());
    if (!registry.open(QIODevice::WriteOnly)) {
        return fail(studious::i18n::s("记录导入结果失败（写不了 assets/state）。",
                                      "Could not record the import (cannot write assets/state)."));
    }
    registry.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!registry.commit()) {
        return fail(studious::i18n::s("记录导入结果失败（写入未完成）。",
                                      "Could not record the import (write did not finish)."));
    }

    if (destName != nullptr) {
        *destName = name;
    }
    qInfo().noquote() << QStringLiteral("[local] 导入模型 %1 <- %2").arg(name, source.absoluteFilePath());
    return true;
}

bool LocalEngine::removeUserModel(const QString &fileName, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const QString name = fileName.trimmed();
    // 只接受纯文件名：界面传上来的就是文件名，带路径的一律当脏数据拒掉。
    if (name.isEmpty() || name != QFileInfo(name).fileName()) {
        return fail(studious::i18n::s("模型文件名不合法。", "That model file name is not valid."));
    }

    const QString path = QDir(userLlmModelsDir()).filePath(name);
    if (!QFileInfo::exists(path)) {
        return fail(studious::i18n::s("这个模型文件已经不在了。", "That model file is already gone."));
    }
    if (!QFile::remove(path)) {
        // 最常见的原因：llama-server 正拿着它（正在用这个模型翻译）。
        return fail(studious::i18n::sf("删不掉「%1」：可能正被本地引擎占用，先停下本地 LLM 再试。",
                                       "Cannot delete %1: the local engine may be using it. Stop "
                                       "local LLM first.",
                                       {name}));
    }

    // 登记表里还留着这个名字就摘掉；写失败不影响"文件已删"这个事实，记一条日志即可。
    QStringList registered = importedModelNames();
    if (registered.contains(name)) {
        QJsonArray files;
        {
            QFile existing(importedRegistryPath());
            if (existing.open(QIODevice::ReadOnly)) {
                const QJsonArray previous = QJsonDocument::fromJson(existing.readAll())
                                                .object()
                                                .value(QStringLiteral("files"))
                                                .toArray();
                for (const QJsonValue &value : previous) {
                    if (value.toObject().value(QStringLiteral("name")).toString() != name) {
                        files.append(value);
                    }
                }
                existing.close();
            }
        }
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("files"), files);
        QSaveFile registry(importedRegistryPath());
        if (registry.open(QIODevice::WriteOnly)
            && registry.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0) {
            registry.commit();
        } else {
            qWarning().noquote() << QStringLiteral("[local] 登记表没更新（文件已删）：%1")
                                        .arg(importedRegistryPath());
        }
    }

    qInfo().noquote() << QStringLiteral("[local] 删除用户模型 %1").arg(name);
    return true;
}

QString LocalEngine::pathForName(const QString &fileName)
{
    const QString trimmed = fileName.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const QFileInfo info(trimmed);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    // 用户目录优先：同名时用户自己的那份更可能是他想跑的那个
    // （清单资产的文件名都是 model.* 开头，实际很难撞上，这里只是给个确定答案）。
    const QFileInfo user(QDir(userLlmModelsDir()).filePath(trimmed));
    if (user.isFile()) {
        return user.absoluteFilePath();
    }
    return QDir(llmModelsDir()).filePath(trimmed);
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
        // 空名 = 「自动（第一个可用）」，而 availableModels() 现在只认清单里装好的资产，
        // 所以这里的原因不是"目录里没文件"，而是"清单里没有一条可用的 model 资产"。
        return fail(sf("本地引擎没有可用模型：%1 里没有清单登记且已安装的模型。\n"
                       "请在「资产」页下载模型（或导入一个）。",
                       "The local engine has no usable model: %1 holds no installed model listed in "
                       "the manifest.\nPlease download a model on the Assets page (or import one).",
                       {QDir::toNativeSeparators(llmModelsDir())}));
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
    // 0 字节与非空是两回事：explicit 传进来的路径不经过 availableModels()，
    // 而空的 .gguf 一样"存在"，llama-server 只会给一句看不懂的加载失败。
    if (modelInfo.size() <= 0) {
        return fail(sf("本地引擎的模型文件是空的（0 字节）：%1\n请重新下载这个模型。",
                       "The local engine model file is empty (0 bytes): %1\n"
                       "Please download this model again.",
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
