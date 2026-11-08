#include "asset/AssetRegistry.h"

#include "i18n/L10n.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <utility>

namespace studious::asset {
namespace {

QString joinPath(const QString &base, const QString &name)
{
    if (base.isEmpty()) {
        return name;
    }
    return base.endsWith(QLatin1Char('/')) ? base + name
                                           : base + QLatin1Char('/') + name;
}

// 目录总大小（递归）。stat 失败就当 0，不让统计把状态判断搞崩。
qint64 directoryBytes(const QString &path)
{
    qint64 total = 0;
    QDir dir(path);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (info.isDir()) {
            total += directoryBytes(info.absoluteFilePath());
        } else {
            total += info.size();
        }
    }
    return total;
}

// zip 里可能有 "./" 前缀或反斜杠，统一后再比对关键文件。
QString normalizeRelative(const QString &relative)
{
    QString out = relative;
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (out.startsWith(QLatin1String("./"))) {
        out.remove(0, 2);
    }
    while (out.endsWith(QLatin1Char('/'))) {
        out.chop(1);
    }
    return out;
}

// 原生可执行/可加载二进制：这一类文件"装着就可能被执行"，校验时最不能放过。
bool isNativeBinary(const QString &relative)
{
    const QString suffix = QFileInfo(relative).suffix().toLower();
    return suffix == QLatin1String("exe") || suffix == QLatin1String("dll")
           || suffix == QLatin1String("so") || suffix == QLatin1String("dylib")
           || suffix == QLatin1String("com") || suffix == QLatin1String("bat")
           || suffix == QLatin1String("cmd") || suffix == QLatin1String("scr")
           || suffix == QLatin1String("msi") || suffix == QLatin1String("pif");
}

} // namespace

AssetRegistry::AssetRegistry(QString installRoot, Manifest manifest)
    : m_root(QDir(installRoot).absolutePath())
    , m_manifest(std::move(manifest))
{
    // 去掉末尾斜杠，后面拼路径只处理一次。
    while (m_root.size() > 1 && m_root.endsWith(QLatin1Char('/'))) {
        m_root.chop(1);
    }
}

void AssetRegistry::setManifest(const Manifest &manifest)
{
    m_manifest = manifest;
    m_dirty = true;
}

const Manifest &AssetRegistry::manifest() const
{
    return m_manifest;
}

QString AssetRegistry::installRoot() const
{
    return m_root;
}

QString AssetRegistry::resolveTarget(const QString &id, QString *error) const
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return QString();
    };

    const AssetEntry *entry = m_manifest.find(id);
    if (entry == nullptr) {
        return fail(i18n::s("清单中不存在该资产", "This asset is not listed in the manifest"));
    }

    // 第一道闸：id 白名单。id 直接来自清单，不净化就会被 "../../.." 写到根外。
    QString idError;
    if (!isSafeAssetId(id, &idError)) {
        return fail(i18n::sf("资产 id \"%1\" 不安全，拒绝把它当路径使用：%2",
                             "Unsafe asset id \"%1\"; refusing to use it as a path: %2",
                             {id, idError}));
    }

    // 布局：
    //   <root>/runtime/<id>         llama-server / onnxruntime
    //   <root>/models/llm/<id>      本地 LLM 的 GGUF
    //   <root>/models/mt/<id>       本地 MT 的 opus-mt 语言包
    //   <root>/models/ocr/<id>      OCR 三件套原文（运行时副本在 models/ocr/run/）
    // 以前全平铺在 models/ 下，16 个条目混在一起，光看名字分不清谁能被 llama-server 打开。
    QString category;
    if (entry->isRuntime()) {
        category = QStringLiteral("runtime");
    } else if (id.startsWith(QLatin1String("mt."))) {
        category = QStringLiteral("models/mt");
    } else if (id.startsWith(QLatin1String("ocr."))) {
        category = QStringLiteral("models/ocr");
    } else {
        category = QStringLiteral("models/llm");
    }

    const QString categoryDir = QDir::cleanPath(joinPath(m_root, category));
    const QString target = QDir::cleanPath(joinPath(categoryDir, id));

    // 第二道闸：cleanPath 之后必须仍在分类目录里。白名单哪天被放宽，这里还在。
    if (!isPathInside(target, categoryDir)) {
        return fail(i18n::sf("资产 %1 的安装路径越出安装目录（疑似清单被篡改），已拒绝：%2",
                             "The install path for asset %1 escapes the install directory (the "
                             "manifest looks tampered with); refusing: %2",
                             {id, target}));
    }

    if (error != nullptr) {
        error->clear();
    }
    return target;
}

QString AssetRegistry::targetPath(const QString &id) const
{
    return resolveTarget(id, nullptr);
}

QString AssetRegistry::partialPath(const QString &id) const
{
    // 文件名逐字符清洗会把恶意 id "洗成"一个合法名字，两个不同 id 因而可能撞到同一个
    // .part 上（续传就会续到别人的数据里）。所以先过白名单：不合规的 id 干脆不给路径。
    if (!isSafeAssetId(id, nullptr)) {
        return QString();
    }

    // 白名单已保证只剩 [A-Za-z0-9._-]，这段清洗留着当第二层（将来放宽 id 规则时的兜底）。
    QString safe = id;
    for (QChar &c : safe) {
        const bool keep = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
                          || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                          || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                          || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.');
        if (!keep) {
            c = QLatin1Char('_');
        }
    }
    return joinPath(joinPath(m_root, QStringLiteral("downloads")), safe + QStringLiteral(".part"));
}

AssetStatus AssetRegistry::computeStatus(const AssetEntry &entry) const
{
    AssetStatus st;
    st.id = entry.id;

    QString pathError;
    st.installPath = resolveTarget(entry.id, &pathError);
    if (st.installPath.isEmpty()) {
        st.state = AssetState::NotInstalled;
        st.detail = pathError.isEmpty()
                        ? i18n::s("清单中不存在该资产", "This asset is not listed in the manifest")
                        : pathError;
        return st;
    }

    const QFileInfo target(st.installPath);
    if (!target.exists()) {
        st.state = AssetState::NotInstalled;
        st.detail = i18n::s("未安装", "Not installed");
        return st;
    }

    // 单文件资产（.gguf 之类）：整份文件就是一个原子安装单位，不看 files 列表。
    if (target.isFile()) {
        st.installedBytes = target.size();
        if (st.installedBytes <= 0) {
            st.state = AssetState::Corrupted;
            st.detail = i18n::s("安装文件为空（0 字节）", "The installed file is empty (0 bytes)");
        } else {
            st.state = AssetState::Installed;
            st.detail = i18n::s("已安装", "Installed");
        }
        return st;
    }

    // 目录型资产（zip 解压）：先看空不空，再核对清单里列出的关键文件。
    const QDir dir(st.installPath);
    const QStringList entries =
        dir.entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        st.state = AssetState::Corrupted;
        st.detail = i18n::s("安装目录为空", "The install directory is empty");
        return st;
    }

    QStringList missing;
    QString unsafeFile;
    for (const QString &relative : entry.files) {
        const QString normalized = normalizeRelative(relative);
        if (normalized.isEmpty()) {
            continue;
        }
        // 清单里的相对路径也要过白名单 + 前缀断言：否则恶意清单靠
        // "files": ["../../../Windows/System32/x.dll"] 就能把校验点挪到安装目录外。
        const QString full = QDir::cleanPath(joinPath(st.installPath, normalized));
        if (!isSafeRelativeEntryPath(normalized, nullptr) || !isPathInside(full, st.installPath)) {
            unsafeFile = normalized;
            continue;
        }
        if (!QFileInfo::exists(full)) {
            missing << normalized;
        }
    }

    st.installedBytes = directoryBytes(st.installPath);
    if (!unsafeFile.isEmpty()) {
        st.state = AssetState::Corrupted;
        st.detail = i18n::sf("清单声明的关键文件路径不安全，拒绝校验：%1",
                             "The manifest lists an unsafe required-file path; refusing to verify "
                             "this install: %1",
                             {unsafeFile});
        return st;
    }
    if (!missing.isEmpty()) {
        st.state = AssetState::Corrupted;
        st.detail = i18n::sf("缺少关键文件：%1",
                             "Missing required files: %1",
                             {missing.join(QStringLiteral(", "))});
        return st;
    }

    st.state = AssetState::Installed;
    st.detail = i18n::s("已安装", "Installed");
    return st;
}

void AssetRegistry::ensureFresh() const
{
    if (m_dirty) {
        const_cast<AssetRegistry *>(this)->refresh();
    }
}

void AssetRegistry::refresh()
{
    m_cache.clear();
    m_cache.reserve(m_manifest.assets.size());
    for (const AssetEntry &entry : m_manifest.assets) {
        m_cache.insert(entry.id, computeStatus(entry));
    }
    m_dirty = false;
}

QVector<AssetStatus> AssetRegistry::listStatus() const
{
    ensureFresh();
    QVector<AssetStatus> out;
    out.reserve(m_manifest.assets.size());
    for (const AssetEntry &entry : m_manifest.assets) {
        out.push_back(m_cache.value(entry.id));
    }
    return out;
}

AssetStatus AssetRegistry::status(const QString &id) const
{
    ensureFresh();
    if (m_cache.contains(id)) {
        return m_cache.value(id);
    }
    // 清单外 id：给一个能解释清楚的状态，而不是空白结构。
    const AssetEntry *entry = m_manifest.find(id);
    if (entry != nullptr) {
        return computeStatus(*entry);
    }
    AssetStatus st;
    st.id = id;
    st.state = AssetState::NotInstalled;
    st.detail = i18n::s("清单中不存在该资产", "This asset is not listed in the manifest");
    return st;
}

bool AssetRegistry::uninstall(const QString &id, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    // 清单外的 id：没有可删的东西，保持幂等（调用方常靠它做"确保不存在"）。
    if (m_manifest.find(id) == nullptr) {
        refresh();
        return true;
    }

    // 这里对 target 做递归删除，所以路径必须来自 resolveTarget（白名单 + 前缀断言）。
    // 空串 = id 可疑，宁可不删，也绝不"猜一个路径"删下去。
    QString pathError;
    const QString target = resolveTarget(id, &pathError);
    if (target.isEmpty()) {
        if (error != nullptr) {
            *error = pathError;
        }
        return false;
    }

    bool ok = true;

    if (QFileInfo::exists(target)) {
        const QFileInfo info(target);
        if (info.isDir()) {
            // removeRecursively 对不存在的路径也返回 true，天然幂等。
            if (!QDir(target).removeRecursively()) {
                ok = false;
                if (error != nullptr) {
                    *error = i18n::sf("删除安装目录失败：%1（可能被占用，请先关闭正在使用它的程序）",
                                      "Failed to delete the install directory: %1 (it may be in use; "
                                      "close any program using it and try again)",
                                      {target});
                }
            }
        } else if (!QFile::remove(target)) {
            ok = false;
            if (error != nullptr) {
                *error = i18n::sf("删除安装文件失败：%1（可能被占用，请先关闭正在使用它的程序）",
                                  "Failed to delete the installed file: %1 (it may be in use; "
                                  "close any program using it and try again)",
                                  {target});
            }
        }
    }

    // 顺手清掉半成品：卸载后还留着 .part 会让下次下载"续传"到一个过期文件上。
    const QString part = partialPath(id);
    if (!part.isEmpty()) {
        QFile::remove(part);
    }

    refresh();
    return ok;
}

bool AssetRegistry::markInstalled(const QString &id, QString *error)
{
    return markInstalled(id, AssetOrigin::Download, error);
}

bool AssetRegistry::markInstalled(const QString &id, AssetOrigin origin, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    QString idError;
    if (!isSafeAssetId(id, &idError)) {
        if (error != nullptr) {
            *error = i18n::sf("资产 id \"%1\" 不安全，拒绝登记：%2",
                              "Unsafe asset id \"%1\"; refusing to register it: %2",
                              {id, idError});
        }
        return false;
    }

    const AssetEntry *entry = m_manifest.find(id);
    if (entry == nullptr) {
        if (error != nullptr) {
            *error = i18n::sf("无法登记资产：清单中不存在 %1",
                              "Cannot register asset %1: it is not listed in the manifest",
                              {id});
        }
        return false;
    }

    const AssetStatus st = computeStatus(*entry);
    m_cache.insert(id, st);
    m_dirty = false;

    if (st.state != AssetState::Installed) {
        if (error != nullptr) {
            *error = i18n::sf("资产 %1 安装后仍不完整：%2",
                              "Asset %1 is still incomplete after install: %2",
                              {id, st.detail});
        }
        return false;
    }

    // 来源决定以后启动前要不要做内容校验：Import 的资产没有可信哈希可比。
    if (origin != AssetOrigin::Unknown) {
        saveOrigin(id, origin);
    }
    return true;
}

QString AssetRegistry::originsPath() const
{
    return joinPath(joinPath(m_root, QStringLiteral("state")),
                    QStringLiteral("asset-origins.json"));
}

void AssetRegistry::ensureOriginsLoaded() const
{
    if (m_originsLoaded) {
        return;
    }
    m_originsLoaded = true;
    QFile file(originsPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        // 文件坏了就当没有记录：所有资产退回 Unknown（按下载安装处理）。
        return;
    }
    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QString value = it.value().toString();
        if (value == QLatin1String("import")) {
            m_origins.insert(it.key(), AssetOrigin::Import);
        } else if (value == QLatin1String("download")) {
            m_origins.insert(it.key(), AssetOrigin::Download);
        }
    }
}

void AssetRegistry::saveOrigin(const QString &id, AssetOrigin origin)
{
    ensureOriginsLoaded();
    m_origins.insert(id, origin);

    QJsonObject obj;
    for (auto it = m_origins.constBegin(); it != m_origins.constEnd(); ++it) {
        obj.insert(it.key(),
                   it.value() == AssetOrigin::Import ? QStringLiteral("import")
                                                     : QStringLiteral("download"));
    }

    const QString path = originsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        // 写不进去也不该让安装失败：大不了下次按「下载安装」处理（多校验一次）。
        qWarning("AssetRegistry: cannot write %s", qUtf8Printable(path));
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.commit();
}

AssetOrigin AssetRegistry::originOf(const QString &id) const
{
    ensureOriginsLoaded();
    return m_origins.value(id, AssetOrigin::Unknown);
}

bool AssetRegistry::verifyInstalled(const QString &id, QString *error) const
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    const auto pass = [error]() {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    };

    const AssetEntry *entry = m_manifest.find(id);
    if (entry == nullptr) {
        return fail(i18n::sf("无法校验资产 %1：清单中不存在该资产",
                             "Cannot verify asset %1: it is not listed in the manifest", {id}));
    }

    QString pathError;
    const QString target = resolveTarget(id, &pathError);
    if (target.isEmpty()) {
        return fail(pathError);
    }

    const QFileInfo targetInfo(target);
    if (!targetInfo.exists()) {
        return fail(i18n::sf("资产 %1 尚未安装，没有可校验的内容：%2",
                             "Asset %1 is not installed, so there is nothing to verify: %2",
                             {id, target}));
    }

    // —— 单文件资产（.gguf 之类）：整份文件就是唯一真值，直接对清单里的 sha256 ——
    if (targetInfo.isFile()) {
        if (targetInfo.size() <= 0) {
            return fail(i18n::sf("资产 %1 的安装文件为空（0 字节）：%2",
                                 "The installed file for asset %1 is empty (0 bytes): %2",
                                 {id, target}));
        }
        // 用户自己导入的文件没有可信来源，只确认"还在、不是空文件"。
        // 下载安装的资产没有这条豁免：下面必须逐字节对上清单里的 sha256。
        if (originOf(id) == AssetOrigin::Import) {
            return pass();
        }
        const QString expected = entry->sha256.trimmed().toLower();
        if (expected.isEmpty()) {
            return fail(i18n::sf(
                "清单没有 %1 的 sha256，无法校验已安装文件；拒绝使用未校验的可执行内容",
                "The manifest carries no sha256 for %1, so the installed file cannot be verified; "
                "refusing to use unverified executable content",
                {id}));
        }
        QString hashError;
        if (!verifyFileSha256(target, expected, &hashError)) {
            return fail(i18n::sf("资产 %1 的已安装文件与清单哈希不符：%2",
                                 "The installed file for asset %1 does not match the manifest hash: "
                                 "%2",
                                 {id, hashError}));
        }
        return pass();
    }

    if (!targetInfo.isDir()) {
        return fail(i18n::sf("资产 %1 的安装位置既不是文件也不是目录：%2",
                             "The install location for asset %1 is neither a file nor a directory: %2",
                             {id, target}));
    }

    // —— 目录型资产（zip 解压）——
    // 清单里的 sha256 覆盖的是压缩包，解压后无法逐字节复原，所以只能校验"点名文件都在"。
    if (entry->files.isEmpty()) {
        // 一个文件都没点名 = 没法校验内容。运行时资产会被直接启动，绝不能放行。
        if (entry->isRuntime()) {
            return fail(i18n::sf(
                "清单没有为运行时资产 %1 声明任何关键文件，无法校验安装内容，拒绝使用",
                "The manifest declares no required files for runtime asset %1, so the install cannot "
                "be verified; refusing to use it",
                {id}));
        }
        if (directoryBytes(target) <= 0) {
            return fail(i18n::sf("资产 %1 的安装目录为空：%2",
                                 "The install directory for asset %1 is empty: %2", {id, target}));
        }
        return pass();
    }

    QStringList missing;
    QStringList emptyOrOdd;
    QString unsafe;
    int nativeBinaries = 0;

    for (const QString &relative : entry->files) {
        const QString normalized = normalizeRelative(relative);
        if (normalized.isEmpty()) {
            continue;
        }

        // 校验点必须在安装目录内：这一步是"清单说校验谁"与"实际读谁"的一致性保证。
        const QString full = QDir::cleanPath(joinPath(target, normalized));
        if (!isSafeRelativeEntryPath(normalized, nullptr) || !isPathInside(full, target)) {
            unsafe = normalized;
            continue;
        }

        const QFileInfo info(full);
        if (!info.exists()) {
            missing << normalized;
            continue;
        }
        if (!info.isFile() || info.size() <= 0) {
            emptyOrOdd << normalized;
            continue;
        }
        if (isNativeBinary(normalized)) {
            ++nativeBinaries;
        }
    }

    if (!unsafe.isEmpty()) {
        return fail(i18n::sf("资产 %1 的清单声明了不安全的文件路径，拒绝校验：%2",
                             "Asset %1 lists an unsafe file path in its manifest; refusing to verify: "
                             "%2",
                             {id, unsafe}));
    }
    if (!missing.isEmpty()) {
        return fail(i18n::sf("资产 %1 缺少清单要求的关键文件：%2",
                             "Asset %1 is missing required files: %2",
                             {id, missing.join(QStringLiteral(", "))}));
    }
    if (!emptyOrOdd.isEmpty()) {
        return fail(i18n::sf("资产 %1 的关键文件为空或不是普通文件：%2",
                             "The required files of asset %1 are empty or not regular files: %2",
                             {id, emptyOrOdd.join(QStringLiteral(", "))}));
    }
    if (entry->isRuntime() && nativeBinaries == 0) {
        return fail(i18n::sf(
            "清单没有为运行时资产 %1 点名任何可执行文件（.exe/.dll/.so），"
            "无法证明该目录里跑的是预期程序，拒绝使用",
            "The manifest pins no executable (.exe/.dll/.so) for runtime asset %1, so it cannot be "
            "shown that the expected program is what will run; refusing to use it",
            {id}));
    }
    return pass();
}

qint64 AssetRegistry::totalInstalledBytes() const
{
    qint64 total = 0;
    for (const AssetStatus &st : listStatus()) {
        if (st.state == AssetState::Installed) {
            total += st.installedBytes;
        }
    }
    return total;
}

} // namespace studious::asset
