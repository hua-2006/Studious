#include "OcrModels.h"

#include "AppSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

namespace studious::ui::ocr {
namespace {

struct Item {
    const char *fileName;
    // 新版 id 在前，旧 id 兜底：装了新三件套就用新的，老用户没重下就用旧的。
    QStringList assetIds;
};

// PP-OCRv4 的识别字典只有 6623 行汉字/英文，没有假名 —— 日文识别不出来。
// PP-OCRv5 的多语种字典（18384 行）把平假名/片假名都收进去了，一套模型顶中日英。
const Item kItems[] = {
    {"det.onnx", {QStringLiteral("ocr.det.ppocrv4")}},
    {"rec.onnx", {QStringLiteral("ocr.rec.ppocrv5"), QStringLiteral("ocr.rec.ppocrv4")}},
    {"ppocr_keys_v1.txt",
     {QStringLiteral("ocr.dict.ppocrv5"), QStringLiteral("ocr.dict.ppocrv4")}},
};

} // namespace

QString stageModels()
{
    // 与 AppBackend::assetInstallRootPath() 同一套规则（settings.assetRoot 优先）。
    const AppSettings settings = AppSettings::load();
    const QString root = settings.assetRoot.isEmpty()
                             ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                   .filePath(QStringLiteral("assets"))
                             : settings.assetRoot;
    const QString modelsDir = QDir(root).filePath(QStringLiteral("models"));
    const QString stagedDir = QDir(modelsDir).filePath(QStringLiteral("ocr"));

    // 三种布局都认：models/ocr/det.onnx、models/ocr.det.ppocrv4（单文件资产）、
    // models/ocr.det.ppocrv4/det.onnx（压缩包资产）。
    bool anyStaged = false;
    for (const Item &item : kItems) {
        const QString staged = QDir(stagedDir).filePath(QString::fromLatin1(item.fileName));

        QString source;
        for (const QString &assetName : item.assetIds) {
            const QString candidates[] = {
                QDir(modelsDir).filePath(assetName),
                QDir(QDir(modelsDir).filePath(assetName)).filePath(QString::fromLatin1(item.fileName)),
            };
            for (const QString &candidate : candidates) {
                if (QFileInfo(candidate).isFile()) {
                    source = candidate;
                    break;
                }
            }
            if (!source.isEmpty())
                break;
        }
        if (source.isEmpty())
            continue;

        // 已经收拢过就跳过；但尺寸对不上说明上次留下的是坏文件（例如 Qt 的
        // QFile::link() 在 Windows 上会写成 1 KB 的 .lnk 快捷方式，不是硬链接），重来。
        const QFileInfo stagedInfo(staged);
        if (stagedInfo.isFile() && stagedInfo.size() == QFileInfo(source).size())
            continue;
        if (stagedInfo.exists())
            QFile::remove(staged);

        QDir().mkpath(stagedDir);
        if (!QFile::copy(source, staged))
            qWarning("[ocr] failed to stage %s from %s", item.fileName, qUtf8Printable(source));
        anyStaged = true;
    }

    if (anyStaged)
        qInfo("[ocr] staged models into %s", qUtf8Printable(stagedDir));

    return stagedDir;
}

} // namespace studious::ui::ocr
