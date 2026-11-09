#include "AssetPaths.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <studious/asset/AssetRegistry.h>
#include <studious/asset/DataRoots.h>
#include <studious/asset/Manifest.h>
#include <studious/asset/TrustKeys.h>

namespace studious::ui::paths {

QString dataRoot()
{
    // 推导只有一份实现，在 backend/include/studious/asset/DataRoots.h —— 本地 MT、
    // CLI、自测程序也用同一份，别再在这里抄一遍向上查找。
    return studious::asset::dataRoot();
}

QString assetsRoot()
{
    // 用户可以在设置里把安装根挪到别的盘（键 settings/asset/root）。空 = 默认位置。
    // 每次调用都读一次 QSettings：调用点全是低频操作（刷资产表 / 下载 / 迁移 / 起引擎），
    // 换来的好处是「改完立刻全链路生效」。
    const AppSettings settings = AppSettings::load();
    if (!settings.assetRoot.isEmpty()) {
        return QDir::cleanPath(settings.assetRoot);
    }
    return QDir(dataRoot()).filePath(QStringLiteral("assets"));
}

QString modelsLlmDir()
{
    return QDir(assetsRoot()).filePath(QStringLiteral("models/llm"));
}

QString modelsMtDir()
{
    return QDir(assetsRoot()).filePath(QStringLiteral("models/mt"));
}

QString modelsOcrDir()
{
    return QDir(assetsRoot()).filePath(QStringLiteral("models/ocr"));
}

QString logsDir()
{
    return QDir(dataRoot()).filePath(QStringLiteral("logs"));
}

QString recordsDir()
{
    return QDir(dataRoot()).filePath(QStringLiteral("records"));
}

QString cacheDbPath()
{
    return QDir(dataRoot()).filePath(QStringLiteral("cache.db"));
}

QString manifestPath()
{
    const QString bundled = QStringLiteral(":/assets/manifest.json");
    if (QFileInfo::exists(bundled)) {
        return bundled;
    }

    if (qEnvironmentVariableIsSet("STUDIOUS_DEV_MANIFEST")) {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            QDir(appDir).filePath(QStringLiteral("manifest.json")),
            QDir(appDir).filePath(QStringLiteral("../backend/asset/manifest.json")),
            QDir(appDir).filePath(QStringLiteral("../../backend/asset/manifest.json")),
        };
        for (const QString &candidate : candidates) {
            if (QFileInfo::exists(candidate)) {
                qWarning("[assets] using writable manifest from disk (dev mode): %s",
                         qUtf8Printable(candidate));
                return candidate;
            }
        }
    }
    return QString();
}

bool loadManifest(studious::asset::Manifest *out)
{
    if (out == nullptr) {
        return false;
    }
    const QString path = manifestPath();
    if (path.isEmpty()) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    studious::asset::ManifestVerifyOptions opts;
    // 公钥编译在二进制里：清单**只要带 sig 就必须验过**（parseManifest 拿到公钥就会验，
    // 不受下面的 requireSignature 影响）。仓库里这份清单目前还没有签名 —— 发布流水线用
    // `manifest_tool sign` 生成带 sig 的信封之后，这里会自动开始校验。
    opts.trustedPublicKey = QByteArray::fromHex(studious::asset::kManifestPublicKeyHex);
    // 发版 / 验收时设 STUDIOUS_REQUIRE_SIGNED_MANIFEST=1：没有签名直接拒绝加载。
    opts.requireSignature = qEnvironmentVariableIsSet("STUDIOUS_REQUIRE_SIGNED_MANIFEST");
    // 只在开发期喊「清单未验签」：那时读的是仓库里这份示例清单，被改过没人知道。
    // 正式包内置的清单同样没签名，但那是已知状态，不该每次启动刷一行告警。
    opts.warnWhenUnsigned = qEnvironmentVariableIsSet("STUDIOUS_DEV_MANIFEST");
    opts.allowMissingSha256 = true;
    opts.maxSchemaVersion = 1;

    const studious::asset::ManifestLoadResult parsed =
        studious::asset::parseManifest(bytes, opts);
    if (!parsed.ok) {
        return false;
    }

    *out = parsed.manifest;
    return true;
}

bool verifyInstalled(const QString &assetId, QString *error)
{
    studious::asset::Manifest manifest;
    if (!loadManifest(&manifest)) {
        if (error != nullptr) {
            *error = QStringLiteral("no manifest available");
        }
        return false;
    }

    studious::asset::AssetRegistry registry(assetsRoot(), manifest);
    registry.refresh();
    return registry.verifyInstalled(assetId, error);
}

}   // namespace studious::ui::paths
