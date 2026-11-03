#include "asset/Downloader.h"

#include "asset/Manifest.h"
#include "i18n/L10n.h"
#include "net/IHttpClient.h"

#include "miniz.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <utility>

namespace studious::asset {
namespace {

// 进度回调粒度：读满 64 KiB 落一次盘并报一次进度。
constexpr qint64 kChunkSize = 64 * 1024;

// 空闲超时：连续这么久没有新数据才判失败（不是整段传输超时，几百 MB 的包也能下完）。
constexpr int kIdleTimeoutMs = 30000;

QString joinPath(const QString &base, const QString &name)
{
    if (base.isEmpty()) {
        return name;
    }
    return base.endsWith(QLatin1Char('/')) ? base + name
                                           : base + QLatin1Char('/') + name;
}

bool ensureDir(const QString &path, QString *error)
{
    if (path.isEmpty()) {
        return true;
    }
    QDir dir(path);
    if (dir.exists()) {
        return true;
    }
    if (QDir().mkpath(path)) {
        return true;
    }
    if (error != nullptr) {
        *error = i18n::sf("无法创建目录：%1", "Unable to create directory: %1", {path});
    }
    return false;
}

bool removePath(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return true;
    }
    const QFileInfo info(path);
    if (info.isDir()) {
        return QDir(path).removeRecursively();
    }
    return QFile::remove(path);
}

// ---------------------------------------------------------------- zip 解压

// miniz 用回调读写，路径全程走 QFile：
// 既避免 mz_zip_reader_init_file() 在 Windows 上按 8-bit 本地编码打开（中文用户名会挂），
// 也避免把整个 zip 读进内存。
struct ZipReadCtx {
    QFile *file = nullptr;
};

size_t zipReadAt(void *opaque, mz_uint64 offset, void *buffer, size_t len)
{
    auto *ctx = static_cast<ZipReadCtx *>(opaque);
    if (ctx == nullptr || ctx->file == nullptr || !ctx->file->isOpen()) {
        return 0;
    }
    if (!ctx->file->seek(static_cast<qint64>(offset))) {
        return 0;
    }
    const qint64 read = ctx->file->read(static_cast<char *>(buffer), static_cast<qint64>(len));
    return read > 0 ? static_cast<size_t>(read) : 0;
}

struct ZipWriteCtx {
    QFile *file = nullptr;
};

size_t zipWriteAt(void *opaque, mz_uint64 offset, const void *buffer, size_t len)
{
    auto *ctx = static_cast<ZipWriteCtx *>(opaque);
    if (ctx == nullptr || ctx->file == nullptr || !ctx->file->isOpen()) {
        return 0;
    }
    if (static_cast<mz_uint64>(ctx->file->pos()) != offset
        && !ctx->file->seek(static_cast<qint64>(offset))) {
        return 0;
    }
    const qint64 wrote = ctx->file->write(static_cast<const char *>(buffer), static_cast<qint64>(len));
    if (wrote < 0 || static_cast<size_t>(wrote) != len) {
        return 0;
    }
    return len;
}

// zip-slip 的第一道闸：条目名必须是"相对、无向上跳、无盘符"的普通路径。
bool isSafeEntryName(const QString &rawName, QString *reason)
{
    QString name = rawName;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (name.isEmpty()) {
        *reason = i18n::s("压缩包里有空的条目名，拒绝解压",
                          "The archive contains an entry with an empty name; refusing to extract");
        return false;
    }
    if (name.startsWith(QLatin1Char('/'))) {
        *reason = i18n::sf("压缩包条目使用绝对路径，拒绝解压：%1",
                           "The archive contains an absolute path entry; refusing to extract: %1",
                           {rawName});
        return false;
    }
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':')) {
        *reason = i18n::sf("压缩包条目带盘符，拒绝解压：%1",
                           "The archive contains a drive-qualified entry; refusing to extract: %1",
                           {rawName});
        return false;
    }

    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QLatin1String("..")) {
            *reason = i18n::sf("压缩包条目包含 \"..\"，可能造成目录穿越（zip-slip），拒绝解压：%1",
                               "The archive entry contains \"..\" and could escape the target directory "
                               "(zip-slip); refusing to extract: %1",
                               {rawName});
            return false;
        }
    }
    return true;
}

bool extractZip(const QString &zipPath, const QString &destDir, QString *error)
{
    QFile zipFile(zipPath);
    if (!zipFile.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = i18n::sf("无法打开下载到的压缩包：%1（%2）",
                              "Unable to open the downloaded archive: %1 (%2)",
                              {zipPath, zipFile.errorString()});
        }
        return false;
    }

    ZipReadCtx readCtx{&zipFile};
    mz_zip_archive zip{};
    zip.m_pIO_opaque = &readCtx;
    zip.m_pRead = &zipReadAt;

    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(zipFile.size()), 0)) {
        if (error != nullptr) {
            *error = i18n::s("压缩包损坏或不是合法的 zip：无法读取中央目录",
                             "The archive is corrupt or not a valid ZIP file: unable to read its "
                             "central directory");
        }
        zipFile.close();
        return false;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    QString failure;

    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            failure = i18n::sf("无法读取压缩包第 %1 个条目的信息，压缩包可能已损坏",
                               "Unable to read entry #%1 of the archive; it may be corrupt",
                               {static_cast<int>(i) + 1});
            break;
        }

        const QString name = QString::fromUtf8(stat.m_filename);
        if (!isSafeEntryName(name, &failure)) {
            ok = false;
            break;
        }

        // 符号链接条目一律拒绝：即使路径合法，写出去的也可能是指向系统目录的链接。
        const mz_uint32 unixMode = (stat.m_external_attr >> 16) & 0xFFFFu;
        if ((unixMode & 0xF000u) == 0xA000u) {
            ok = false;
            failure = i18n::sf("压缩包条目是符号链接，拒绝解压：%1",
                               "The archive contains a symbolic link entry; refusing to extract: %1",
                               {name});
            break;
        }

        QString nameForPath = name;
        while (nameForPath.endsWith(QLatin1Char('/'))) {
            nameForPath.chop(1);
        }
        const QString outPath = joinPath(destDir, nameForPath);

        if (stat.m_is_directory) {
            if (!ensureDir(outPath, &failure)) {
                ok = false;
            }
            continue;
        }

        if (!ensureDir(QFileInfo(outPath).absolutePath(), &failure)) {
            ok = false;
            break;
        }

        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ok = false;
            failure = i18n::sf("无法写入解压文件：%1（%2）",
                               "Unable to write the extracted file: %1 (%2)",
                               {outPath, out.errorString()});
            break;
        }

        ZipWriteCtx writeCtx{&out};
        const mz_bool extracted =
            mz_zip_reader_extract_to_callback(&zip, i, &zipWriteAt, &writeCtx, 0);
        out.flush();
        out.close();
        if (extracted == MZ_FALSE) {
            ok = false;
            failure = i18n::sf("解压失败：%1（压缩包可能已损坏）",
                               "Extraction failed: %1 (the archive may be corrupt)",
                               {name});
        }
    }

    mz_zip_reader_end(&zip);
    zipFile.close();

    if (!ok && error != nullptr) {
        *error = failure;
    }
    return ok;
}

// ---------------------------------------------------------------- 单个源的传输

struct AttemptResult {
    enum class Kind { Ok, Canceled, RangeComplete, Failed };
    Kind kind = Kind::Failed;
    QString error;
    qint64 received = 0;
};

AttemptResult downloadOnce(net::IHttpClient &http,
                           const QUrl &url,
                           const QString &partPath,
                           qint64 existingBytes,
                           int sourceIndex,
                           int sourceCount,
                           const std::function<void(const DownloadProgress &)> &progress,
                           const std::atomic_bool *cancelFlag)
{
    AttemptResult out;

    const bool resuming = existingBytes > 0;
    QFile part(partPath);
    if (resuming) {
        if (!part.open(QIODevice::ReadWrite)) {
            out.error = i18n::sf("无法打开断点文件继续下载：%1（%2）",
                                 "Unable to reopen the partial download: %1 (%2)",
                                 {partPath, part.errorString()});
            return out;
        }
        if (!part.seek(existingBytes)) {
            out.error = i18n::sf("无法定位到断点位置 %1：%2",
                                 "Unable to seek to resume offset %1: %2",
                                 {QString::number(existingBytes), part.errorString()});
            part.close();
            return out;
        }
    } else if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.error = i18n::sf("无法创建下载临时文件：%1（%2）",
                             "Unable to create the temporary download file: %1 (%2)",
                             {partPath, part.errorString()});
        return out;
    }

    struct State {
        qint64 received = 0;        // 累计已落盘（含续传前缀）
        qint64 total = 0;           // 0 = 服务端没给 Content-Length
        bool rejected = false;      // 状态码不是 200/206/416，或本地写失败
        bool canceled = false;
        bool rangeComplete = false; // 416：.part 已经完整
        bool writeFailed = false;
        QString rejectReason;
    } st;
    st.received = existingBytes;

    QByteArray pending;

    const auto report = [&](qint64 receivedBytes) {
        if (!progress) {
            return;
        }
        DownloadProgress p;
        p.received = receivedBytes;
        p.total = st.total;
        p.sourceIndex = sourceIndex;
        p.sourceCount = sourceCount;
        progress(p);
    };

    const auto flushPending = [&]() -> bool {
        if (pending.isEmpty()) {
            return true;
        }
        const QByteArray tail = pending;
        pending.clear();
        const qint64 wrote = part.write(tail);
        if (wrote != tail.size()) {
            st.writeFailed = true;
            return false;
        }
        st.received += wrote;
        report(st.received);
        return true;
    };

    net::HttpRequest req;
    req.url = url;
    req.method = QByteArrayLiteral("GET");
    req.timeoutMs = kIdleTimeoutMs;
    // identity：别让服务端 gzip，否则 Content-Length 与实际字节对不上，断点也就废了。
    req.headers.append(qMakePair(QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")));
    if (resuming) {
        req.headers.append(qMakePair(QByteArrayLiteral("Range"),
                                     QByteArrayLiteral("bytes=") + QByteArray::number(existingBytes)
                                         + QByteArrayLiteral("-")));
    }

    net::HttpStreamCallbacks cb;
    cb.onStart = [&](int status, qint64 contentLength) -> bool {
        if (status == 416 && resuming) {
            // Range 无法满足：最可能是 .part 已经完整，直接进校验。
            st.rangeComplete = true;
            st.received = existingBytes;
            st.total = existingBytes;
            report(st.received);
            return false;
        }
        if (status != 200 && status != 206) {
            // 关键：这里就掐断，绝不能把 404/500 的 HTML 错误页写进 .part。
            st.rejected = true;
            st.rejectReason = i18n::sf("服务端返回 HTTP %1", "The server returned HTTP %1",
                                       {status});
            return false;
        }
        if (status == 206) {
            st.received = existingBytes;
            st.total = contentLength > 0 ? contentLength + existingBytes : 0;
        } else {
            // 200：服务端不支持（或忽略了）Range。必须从头重写，
            // 否则新旧字节拼在一起，校验必失败。
            if (existingBytes > 0 && (!part.resize(0) || !part.seek(0))) {
                st.rejected = true;
                st.writeFailed = true;
                st.rejectReason = i18n::s("服务端不支持断点续传，且无法重置本地断点文件",
                                          "The server does not support resuming, and the local partial "
                                          "file could not be reset");
                return false;
            }
            st.received = 0;
            st.total = contentLength > 0 ? contentLength : 0;
        }
        report(st.received);
        return true;
    };

    cb.onData = [&](const QByteArray &chunk, qint64) -> bool {
        pending += chunk;

        // 严格按 64 KiB 一块落盘 + 报进度：网络层一次给多大的块都不影响进度粒度。
        while (pending.size() >= kChunkSize) {
            // 每块检查一次取消标记：用户点"取消"后最多再多写 64 KiB。
            if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed)) {
                st.canceled = true;
                return false;
            }
            const QByteArray block = pending.left(kChunkSize);
            pending.remove(0, kChunkSize);

            const qint64 wrote = part.write(block);
            if (wrote != block.size()) {
                st.rejected = true;
                st.writeFailed = true;
                st.rejectReason = i18n::sf("写入本地文件失败：%1",
                                           "Failed to write the local file: %1",
                                           {part.errorString()});
                return false;
            }
            st.received += wrote;
            report(st.received);
        }

        if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed)) {
            st.canceled = true;
            return false;
        }
        return true;
    };

    const net::HttpResponse resp = http.sendStream(req, cb);
    part.flush();

    if (st.canceled) {
        // 已经落盘的字节全保留：这就是下一次续传的起点。
        part.close();
        out.kind = AttemptResult::Kind::Canceled;
        out.received = st.received;
        out.error = i18n::s("下载已取消（已保留断点，下次可继续）",
                            "Download cancelled. The partial file is kept so the next attempt resumes "
                            "where it stopped.");
        return out;
    }

    if (st.rangeComplete) {
        part.close();
        out.kind = AttemptResult::Kind::RangeComplete;
        out.received = st.received;
        return out;
    }

    if (st.writeFailed) {
        part.close();
        // 本地写坏了，断点不可信，删掉重来。
        QFile::remove(partPath);
        out.error = st.rejectReason.isEmpty()
                        ? i18n::s("写入本地文件失败", "Failed to write the local file")
                        : st.rejectReason;
        out.received = st.received;
        return out;
    }

    if (st.rejected) {
        part.close();
        out.error = st.rejectReason;
        out.received = st.received;
        return out;
    }

    if (!resp.ok) {
        part.close();
        out.error = resp.error.isEmpty() ? i18n::s("网络请求失败", "The network request failed")
                                         : resp.error;
        out.received = st.received;
        return out;
    }

    if (!flushPending()) {
        part.close();
        QFile::remove(partPath);
        out.error = i18n::sf("写入本地文件失败：%1", "Failed to write the local file: %1",
                             {part.errorString()});
        return out;
    }

    part.flush();
    part.close();
    out.kind = AttemptResult::Kind::Ok;
    out.received = st.received;
    return out;
}

} // namespace

Downloader::Downloader(net::IHttpClient &http, AssetRegistry &registry)
    : m_http(http)
    , m_registry(registry)
{
}

DownloadResult Downloader::download(const QString &assetId,
                                    std::function<void(const DownloadProgress &)> progress,
                                    const std::atomic_bool *cancelFlag)
{
    DownloadResult result;

    const AssetEntry *entry = m_registry.manifest().find(assetId);
    if (entry == nullptr) {
        result.error = i18n::sf("清单中不存在该资产：%1", "Asset not found in the manifest: %1",
                                {assetId});
        return result;
    }
    if (entry->urls.isEmpty()) {
        result.error = i18n::sf("资产 %1 暂无可用的下载地址（可能是尚未发布的占位条目）",
                                "Asset %1 has no download URL yet (it may be a placeholder for an "
                                "unreleased build).",
                                {assetId});
        return result;
    }

    const QString expectedSha = entry->sha256.trimmed().toLower();
    if (expectedSha.isEmpty()) {
        result.error = i18n::s("该资产尚未发布正式哈希，暂不可下载",
                               "This asset has no published checksum yet, so it cannot be "
                               "downloaded.");
        return result;
    }

    const QString partPath = m_registry.partialPath(assetId);
    QString dirError;
    if (!ensureDir(QFileInfo(partPath).absolutePath(), &dirError)) {
        result.error = dirError;
        return result;
    }

    const int sourceCount = static_cast<int>(entry->urls.size());
    QStringList sourceErrors;
    bool ready = false;

    for (int i = 0; i < sourceCount && !ready; ++i) {
        const QString urlText = entry->urls.at(i);
        const QUrl url(urlText);
        if (url.isEmpty() || !url.isValid()) {
            sourceErrors << i18n::sf("%1：下载地址无效", "%1: invalid download URL", {urlText});
            continue;
        }

        // 每个源出发前重新读一次 .part 的长度：上一个源可能已经写进去一部分。
        const QFileInfo partInfo(partPath);
        const qint64 resumeFrom = partInfo.exists() ? partInfo.size() : 0;

        const AttemptResult attempt =
            downloadOnce(m_http, url, partPath, resumeFrom, i, sourceCount, progress, cancelFlag);

        if (attempt.kind == AttemptResult::Kind::Canceled) {
            result.error = attempt.error;
            return result;
        }
        if (attempt.kind == AttemptResult::Kind::Ok
            || attempt.kind == AttemptResult::Kind::RangeComplete) {
            ready = true;
            break;
        }

        sourceErrors << i18n::sf("%1：%2", "%1: %2", {urlText, attempt.error});
    }

    if (!ready) {
        result.error = i18n::sf("所有 %1 个下载源都失败：%2",
                                "All %1 download sources failed: %2",
                                {sourceCount, sourceErrors.join(i18n::s("；", "; "))});
        return result;
    }

    // ---- 校验：清单里的 sha256 是唯一的"这份字节能不能装"的依据 ----
    QString hashError;
    const QString actualSha = sha256File(partPath, &hashError);
    if (actualSha.isEmpty()) {
        QFile::remove(partPath);
        result.error = hashError.isEmpty()
                           ? i18n::sf("无法读取下载文件以计算校验值：%1",
                                      "Unable to read the downloaded file to compute its checksum: %1",
                                      {partPath})
                           : hashError;
        return result;
    }
    if (actualSha != expectedSha) {
        QFile::remove(partPath);
        result.error = i18n::sf("sha256 校验失败：实际 %1，期望 %2。损坏的文件已被删除，请重新下载。",
                                "SHA-256 mismatch: got %1, expected %2. The corrupted file has been "
                                "deleted; please download it again.",
                                {actualSha, expectedSha});
        return result;
    }

    // ---- 原子安装 ----
    QString installError;
    const QString installedPath = finalizeInstall(*entry, partPath, true, &installError);
    if (installedPath.isEmpty()) {
        result.error = installError.isEmpty()
                           ? i18n::s("安装失败：未知原因", "Installation failed for an unknown reason")
                           : installError;
        return result;
    }

    result.installedPath = installedPath;

    QString registerError;
    if (!m_registry.markInstalled(assetId, &registerError)) {
        result.error = registerError;
        return result;
    }

    QFile::remove(partPath);
    result.ok = true;
    return result;
}

DownloadResult Downloader::installFromFile(const QString &assetId, const QString &localFilePath)
{
    DownloadResult result;

    const AssetEntry *entry = m_registry.manifest().find(assetId);
    if (entry == nullptr) {
        result.error = i18n::sf("清单中不存在该资产：%1", "Asset not found in the manifest: %1",
                                {assetId});
        return result;
    }

    const QFileInfo source(localFilePath);
    if (!source.exists() || !source.isFile()) {
        result.error = i18n::sf("找不到要导入的文件：%1",
                                "The file to import does not exist: %1", {localFilePath});
        return result;
    }

    const QString expectedSha = entry->sha256.trimmed().toLower();
    if (expectedSha.isEmpty()) {
        result.error = i18n::s("该资产尚未发布正式哈希，无法校验导入文件，暂不支持手动导入",
                               "This asset has no published checksum, so an imported file cannot be "
                               "verified; manual import is not supported for it yet.");
        return result;
    }

    QString hashError;
    const QString actualSha = sha256File(localFilePath, &hashError);
    if (actualSha.isEmpty()) {
        result.error = hashError.isEmpty()
                           ? i18n::sf("无法读取导入文件以计算校验值：%1",
                                      "Unable to read the imported file to compute its checksum: %1",
                                      {localFilePath})
                           : hashError;
        return result;
    }
    if (actualSha != expectedSha) {
        result.error = i18n::sf("导入文件与清单哈希不一致：实际 %1，期望 %2。未做任何安装。",
                                "The imported file does not match the manifest checksum: got %1, "
                                "expected %2. Nothing was installed.",
                                {actualSha, expectedSha});
        return result;
    }

    QString installError;
    const QString installedPath = finalizeInstall(*entry, localFilePath, false, &installError);
    if (installedPath.isEmpty()) {
        result.error = installError.isEmpty()
                           ? i18n::s("安装失败：未知原因", "Installation failed for an unknown reason")
                           : installError;
        return result;
    }

    result.installedPath = installedPath;

    QString registerError;
    if (!m_registry.markInstalled(assetId, &registerError)) {
        result.error = registerError;
        return result;
    }

    result.ok = true;
    return result;
}

QString Downloader::finalizeInstall(const AssetEntry &entry,
                                    const QString &sourcePath,
                                    bool moveSource,
                                    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QString target = m_registry.targetPath(entry.id);
    if (target.isEmpty()) {
        if (error != nullptr) {
            *error = i18n::sf("清单中不存在该资产：%1", "Asset not found in the manifest: %1",
                              {entry.id});
        }
        return QString();
    }

    QString dirError;
    if (!ensureDir(QFileInfo(target).absolutePath(), &dirError)) {
        if (error != nullptr) {
            *error = dirError;
        }
        return QString();
    }

    // 旧版本先清掉：rename 到一个已存在的目录在 Windows 上会直接失败。
    if (QFileInfo::exists(target) && !removePath(target)) {
        if (error != nullptr) {
            *error = i18n::sf("无法清理旧的安装内容：%1（可能被占用，请先关闭正在使用它的程序）",
                              "Unable to remove the previous installation: %1 (it may be in use; "
                              "close any program using it and try again)",
                              {target});
        }
        return QString();
    }

    const bool isZip = entry.archive.compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0;
    if (isZip) {
        // 先解到 <target>.tmp，全部成功才改名——中途失败绝不会留下"半个安装"。
        const QString tmpDir = target + QStringLiteral(".tmp");
        removePath(tmpDir);
        if (!ensureDir(tmpDir, &dirError)) {
            if (error != nullptr) {
                *error = dirError;
            }
            return QString();
        }

        QString zipError;
        if (!extractZip(sourcePath, tmpDir, &zipError)) {
            removePath(tmpDir);
            if (error != nullptr) {
                *error = zipError;
            }
            return QString();
        }

        if (!QFile::rename(tmpDir, target)) {
            removePath(tmpDir);
            if (error != nullptr) {
                *error = i18n::sf("安装失败：无法把解压结果移动到 %1",
                                  "Installation failed: unable to move the extracted files to %1",
                                  {target});
            }
            return QString();
        }
        return target;
    }

    // 单文件资产（例如 .gguf）：拷/搬到 <target>.tmp，再改名到最终位置。
    const QString tmpFile = target + QStringLiteral(".tmp");
    QFile::remove(tmpFile);

    if (moveSource) {
        if (!QFile::rename(sourcePath, tmpFile) && !QFile::copy(sourcePath, tmpFile)) {
            if (error != nullptr) {
                *error = i18n::sf("安装失败：无法把下载好的文件移动到 %1",
                                  "Installation failed: unable to move the downloaded file to %1",
                                  {target});
            }
            return QString();
        }
    } else if (!QFile::copy(sourcePath, tmpFile)) {
        if (error != nullptr) {
            *error = i18n::sf("安装失败：无法复制导入文件到 %1（磁盘空间不足或权限不足）",
                              "Installation failed: unable to copy the imported file to %1 "
                              "(not enough disk space or insufficient permissions)",
                              {target});
        }
        return QString();
    }

    if (!QFile::rename(tmpFile, target)) {
        QFile::remove(tmpFile);
        if (error != nullptr) {
            *error = i18n::sf("安装失败：无法把临时文件改名为 %1",
                              "Installation failed: unable to rename the temporary file to %1",
                              {target});
        }
        return QString();
    }
    return target;
}

} // namespace studious::asset
