#pragma once

#include "asset/Manifest.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace studious::asset {

// 资产在磁盘上的三种状态。
enum class AssetState {
    NotInstalled,   // 没装 / 只存在于清单里（含占位条目）
    Installed,      // 装好了且关键文件齐全
    Corrupted,      // 目录/文件在，但不完整（空目录、缺关键文件、0 字节文件）
};

struct AssetStatus {
    QString    id;
    AssetState state = AssetState::NotInstalled;
    QString    installPath;        // 绝对路径（目录或文件）；清单里没有该资产时为空
    qint64     installedBytes = 0;
    QString    detail;             // 已本地化的说明，如"缺少关键文件：llama-server.exe"
};

// 资产安装状态的唯一真源：清单 + 安装根目录 → 每个资产装没装、多大、坏没坏。
//
// 线程约束：内部有状态缓存，非线程安全。前端在主线程用、下载在工作线程用的话，
// 请给两边各自一个实例（磁盘是唯一共享点），或者把调用串行化。
class AssetRegistry {
public:
    AssetRegistry(QString installRoot, Manifest manifest);

    void setManifest(const Manifest &manifest);
    const Manifest &manifest() const;
    QString installRoot() const;

    // 目标路径：runtime → <root>/runtime/<id>；model → <root>/models/<id>。
    // 清单里没有该 id 时返回空串（调用方据此报"清单中不存在该资产"）。
    QString targetPath(const QString &id) const;

    // 断点续传的半成品路径：<root>/downloads/<id>.part。
    // 放在这里而不是 Downloader 里，是为了 uninstall 能一并清掉它。
    QString partialPath(const QString &id) const;

    QVector<AssetStatus> listStatus() const;
    AssetStatus status(const QString &id) const;

    // 重新扫描磁盘，更新内部状态缓存。
    void refresh();

    // 删除资产（递归删目录或删单个文件）。幂等：清单外 id 也返回 true。
    // id 不合规（不在白名单内）时返回 false ——绝不拿一个可疑 id 去递归删路径。
    bool uninstall(const QString &id, QString *error = nullptr);

    // Downloader / 手动导入完成后登记。会重新算一次状态，
    // 登记后仍不完整（缺关键文件）就返回 false 并把原因写进 error。
    bool markInstalled(const QString &id, QString *error = nullptr);

    // 安装后校验：状态缓存只判"存在且非空"，这里按清单做内容校验。
    //   单文件资产  → 整份文件 sha256 必须等于清单里的 sha256；
    //   目录型资产  → 清单点名的每个关键文件都要存在、非空、且落在安装目录内，
    //                 运行时资产额外要求至少点名了一个原生可执行文件（.exe/.dll/.so/.dylib）。
    // 目录型资产的整包 sha256 覆盖的是压缩包，解压后无法逐字节复原，故只能做上面这些检查。
    // 注意：调用方（如 LocalEngine 的启动路径）需要自己调一次；本类不拦启动。
    bool verifyInstalled(const QString &id, QString *error = nullptr) const;

    // 所有 Installed 资产的体积之和（字节）。
    qint64 totalInstalledBytes() const;

private:
    AssetStatus computeStatus(const AssetEntry &entry) const;
    void ensureFresh() const;

    // targetPath 的实现：清单查找 + id 白名单 + cleanPath 前缀断言，任何一道不过都返回空串，
    // 并把中文原因写进 *error（可空）。
    QString resolveTarget(const QString &id, QString *error) const;

    QString m_root;
    Manifest m_manifest;
    mutable QHash<QString, AssetStatus> m_cache;
    mutable bool m_dirty = true;
};

} // namespace studious::asset
