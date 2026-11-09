#include "asset/DataRoots.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace studious::asset {
namespace {

// 向上找 version.json：开发期 exe 在 frontend/build（隔两层），打包后一般就在旁边。
QString resolveDataRoot()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 4; ++depth) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral("version.json")))) {
            return QDir::cleanPath(dir.filePath(QStringLiteral("data")));
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return QDir::cleanPath(QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("data")));
}

}   // namespace

QString dataRoot()
{
    static const QString resolved = resolveDataRoot();
    return resolved;
}

QString assetsRoot()
{
    return QDir(dataRoot()).filePath(QStringLiteral("assets"));
}

QString modelsRoot(const QString &kind)
{
    const QDir base(assetsRoot());
    if (kind.isEmpty()) {
        return base.filePath(QStringLiteral("models"));
    }
    return base.filePath(QStringLiteral("models/") + kind);
}

}   // namespace studious::asset
