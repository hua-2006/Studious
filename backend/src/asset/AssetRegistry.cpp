#include "asset/AssetRegistry.h"

#include "i18n/L10n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

} // namespace

AssetRegistry::AssetRegistry(QString installRoot, Manifest manifest)
    : m_root(std::move(installRoot))
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

QString AssetRegistry::targetPath(const QString &id) const
{
    const AssetEntry *entry = m_manifest.find(id);
    if (entry == nullptr) {
        return QString();
    }
    const QString category = entry->isRuntime() ? QStringLiteral("runtime")
                                                : QStringLiteral("models");
    return joinPath(joinPath(m_root, category), id);
}

QString AssetRegistry::partialPath(const QString &id) const
{
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
    st.installPath = targetPath(entry.id);

    if (st.installPath.isEmpty()) {
        st.state = AssetState::NotInstalled;
        st.detail = i18n::s("清单中不存在该资产", "This asset is not listed in the manifest");
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
    for (const QString &relative : entry.files) {
        const QString normalized = normalizeRelative(relative);
        if (normalized.isEmpty()) {
            continue;
        }
        if (!QFileInfo::exists(joinPath(st.installPath, normalized))) {
            missing << normalized;
        }
    }

    st.installedBytes = directoryBytes(st.installPath);
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

    const QString target = targetPath(id);
    bool ok = true;

    if (!target.isEmpty() && QFileInfo::exists(target)) {
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
    if (error != nullptr) {
        error->clear();
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
    return true;
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
