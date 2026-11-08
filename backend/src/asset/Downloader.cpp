#include "asset/Downloader.h"

#include "asset/Manifest.h"
#include "i18n/L10n.h"
#include "net/IHttpClient.h"

#include "miniz.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRandomGenerator>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace studious::asset {
namespace {

// 进度回调粒度：读满 64 KiB 落一次盘并报一次进度。
constexpr qint64 kChunkSize = 64 * 1024;

// 空闲超时：连续这么久没有新数据才判失败（不是整段传输超时，几百 MB 的包也能下完）。
constexpr int kIdleTimeoutMs = 30000;

// 解压上限地板：清单没声明 size 时，一个资产最多解出 200 MiB。
constexpr qint64 kExtractFloorBytes = 200LL * 1024 * 1024;

// 条目数上限：正常运行时包几十~几百个文件、模型包 1 个，2 万条已远超任何真实资产。
constexpr mz_uint kMaxExtractEntries = 20000;

// 算哈希时每次喂进去的块大小（续传时要把已有前缀重新读一遍）。
constexpr qint64 kHashChunk = 1 << 20;

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
    qint64 totalWritten = 0;   // 本压缩包累计写出（跨条目）
    qint64 itemWritten = 0;    // 当前条目累计写出
    qint64 totalLimit = 0;     // 累计上限（字节，0 = 不限）
    qint64 itemLimit = 0;      // 单条目上限（字节，0 = 不限）
    QString overflow;          // 非空 = 撞上限的原因；回调返回 0 让 miniz 立刻中止
};

size_t zipWriteAt(void *opaque, mz_uint64 offset, const void *buffer, size_t len)
{
    auto *ctx = static_cast<ZipWriteCtx *>(opaque);
    if (ctx == nullptr || ctx->file == nullptr || !ctx->file->isOpen()) {
        return 0;
    }

    // 先记账再写：宁可少写这一块，也不能"先写满磁盘再报超限"。
    // 用减法比较，避免 totalWritten + want 溢出成负数后绕过检查。
    const qint64 want = static_cast<qint64>(len);
    if (ctx->totalLimit > 0 && (ctx->totalWritten >= ctx->totalLimit
                                || want > ctx->totalLimit - ctx->totalWritten)) {
        ctx->overflow = i18n::sf(
            "解压后累计大小将超过上限（已写 %1 字节，本块还要 %2 字节，上限 %3 字节），"
            "疑似压缩炸弹，已中止",
            "Extracting would exceed the size limit (%1 bytes written, %2 more requested, limit %3 "
            "bytes); this looks like a zip bomb, aborting",
            {QString::number(ctx->totalWritten), QString::number(want),
             QString::number(ctx->totalLimit)});
        return 0;
    }
    if (ctx->itemLimit > 0 && (ctx->itemWritten >= ctx->itemLimit
                               || want > ctx->itemLimit - ctx->itemWritten)) {
        ctx->overflow = i18n::sf(
            "单个条目解压后超过上限（该条目已写 %1 字节，本块还要 %2 字节，上限 %3 字节），"
            "疑似压缩炸弹，已中止",
            "A single entry exceeds the size limit (%1 bytes written, %2 more requested, limit %3 "
            "bytes); this looks like a zip bomb, aborting",
            {QString::number(ctx->itemWritten), QString::number(want),
             QString::number(ctx->itemLimit)});
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
    ctx->totalWritten += wrote;
    ctx->itemWritten += wrote;
    return len;
}

// 解压总量上限：max(200 MiB, 清单声明的 size × 1.5)。
// 清单里的 size 是压缩包本身的大小，正常压缩包的解压率远到不了 1.5 倍；
// 真超了 1.5 倍基本只剩"压缩炸弹"一种解释。
qint64 extractLimitFor(qint64 declaredSize)
{
    if (declaredSize <= 0) {
        return kExtractFloorBytes;
    }
    const double scaled = static_cast<double>(declaredSize) * 1.5;
    if (scaled >= static_cast<double>(std::numeric_limits<qint64>::max())) {
        return std::numeric_limits<qint64>::max();
    }
    return std::max<qint64>(kExtractFloorBytes, static_cast<qint64>(scaled));
}

// declaredSize：清单为这个资产声明的 size（压缩包字节数），0 = 清单没声明。
bool extractZip(const QString &zipPath,
                const QString &destDir,
                qint64 declaredSize,
                QString *error)
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

    // 条目数上限：几万条目录/空文件本身就是一种攻击面（元数据遍历、inode 打满）。
    if (count > kMaxExtractEntries) {
        if (error != nullptr) {
            *error = i18n::sf("压缩包条目数 %1 超过上限 %2，疑似压缩炸弹，拒绝解压",
                              "The archive has %1 entries, above the limit of %2; this looks like a "
                              "zip bomb, refusing to extract",
                              {static_cast<int>(count), static_cast<int>(kMaxExtractEntries)});
        }
        mz_zip_reader_end(&zip);
        zipFile.close();
        return false;
    }

    const qint64 totalLimit = extractLimitFor(declaredSize);
    ZipWriteCtx writeCtx{};
    writeCtx.totalLimit = totalLimit;

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

        // zip-slip 闸门与清单 files 用的是同一套原语：绝对路径 / 盘符 / ".." /
        // NTFS ADS（a.txt:evil）/ Windows 保留名（CON/NUL/COM1…）/ 尾随 '.' 或空格 全部拒绝。
        if (!isSafeRelativeEntryPath(name, &failure)) {
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
        while (nameForPath.startsWith(QLatin1String("./"))) {
            nameForPath.remove(0, 2);
        }
        while (nameForPath.endsWith(QLatin1Char('/'))) {
            nameForPath.chop(1);
        }
        if (nameForPath.isEmpty() || nameForPath == QLatin1String(".")) {
            continue;   // 压缩包根条目（"./"），没有实体要写
        }

        const QString outPath = QDir::cleanPath(joinPath(destDir, nameForPath));

        // 拼完路径再断言一次：白名单漏了什么，这一道还在。
        if (!isPathInside(outPath, destDir)) {
            ok = false;
            failure = i18n::sf("压缩包条目会写到目标目录之外（zip-slip），拒绝解压：%1",
                               "The archive entry would be written outside the target directory "
                               "(zip-slip); refusing to extract: %1",
                               {name});
            break;
        }

        if (stat.m_is_directory) {
            if (!ensureDir(outPath, &failure)) {
                ok = false;
            }
            continue;
        }

        // 中央目录里的"解压后大小"先看一眼：炸弹通常在落盘前就露馅。
        const qint64 declaredItem = static_cast<qint64>(stat.m_uncomp_size);
        if (declaredItem > totalLimit - writeCtx.totalWritten) {
            ok = false;
            failure = i18n::sf(
                "压缩包条目 %1 解压后 %2 字节，会使解压总量超过上限 %3 字节，"
                "疑似压缩炸弹，拒绝解压",
                "Archive entry %1 would expand to %2 bytes, pushing the total above the %3-byte "
                "limit; this looks like a zip bomb, refusing to extract",
                {name, QString::number(declaredItem), QString::number(totalLimit)});
            break;
        }
        if (declaredSize > 0 && declaredItem > declaredSize && declaredItem > kExtractFloorBytes) {
            // 单条目比"整包声明大小"还大、且已经超过地板值：清单与实际严重不符。
            ok = false;
            failure = i18n::sf("压缩包条目 %1 解压后 %2 字节，超过清单声明的大小 %3 字节，"
                               "清单与实际内容不符，拒绝解压",
                               "Archive entry %1 expands to %2 bytes, more than the %3 bytes the "
                               "manifest declares; the manifest does not match the content, refusing "
                               "to extract",
                               {name, QString::number(declaredItem), QString::number(declaredSize)});
            break;
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

        writeCtx.file = &out;
        writeCtx.itemWritten = 0;
        writeCtx.itemLimit = totalLimit;   // 单条目同样受整包上限约束
        writeCtx.overflow.clear();

        const mz_bool extracted =
            mz_zip_reader_extract_to_callback(&zip, i, &zipWriteAt, &writeCtx, 0);
        out.flush();
        out.close();
        writeCtx.file = nullptr;

        if (extracted == MZ_FALSE) {
            ok = false;
            failure = writeCtx.overflow.isEmpty()
                          ? i18n::sf("解压失败：%1（压缩包可能已损坏）",
                                     "Extraction failed: %1 (the archive may be corrupt)", {name})
                          : writeCtx.overflow;
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
    // 边写边算出来的 sha256（仅 Ok / RangeComplete 有效）。
    // 有了它，"算哈希"和"写文件"是同一遍 I/O：不存在"算完之后文件还能被换掉"的窗口。
    QString digestHex;
};

// 把 .part 里已有的前缀（续传）喂进哈希：否则算出来只是后半段的哈希。
bool hashExistingPrefix(QFile &file, qint64 bytes, QCryptographicHash *hasher, QString *error)
{
    if (bytes <= 0) {
        return true;
    }
    if (!file.seek(0)) {
        if (error != nullptr) {
            *error = i18n::sf("无法读取断点文件的开头：%1", "Unable to read the start of the partial "
                                                          "file: %1",
                              {file.errorString()});
        }
        return false;
    }

    QByteArray buffer;
    buffer.resize(static_cast<int>(kHashChunk));
    qint64 left = bytes;
    while (left > 0) {
        const qint64 want = std::min<qint64>(left, kHashChunk);
        const qint64 got = file.read(buffer.data(), want);
        if (got <= 0) {
            if (error != nullptr) {
                *error = i18n::sf("断点文件不完整（读 %1 字节后中断）：%2",
                                  "The partial file is truncated (read stopped after %1 bytes): %2",
                                  {QString::number(bytes - left), file.errorString()});
            }
            return false;
        }
        hasher->addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
        left -= got;
    }

    if (!file.seek(bytes)) {
        if (error != nullptr) {
            *error = i18n::sf("无法回到断点位置继续写入：%1",
                              "Unable to seek back to the resume offset: %1", {file.errorString()});
        }
        return false;
    }
    return true;
}

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
    // 边写边算：下面每个写盘点都同步喂进这个哈希，写完的那一刻校验值就已经完整。
    QCryptographicHash hasher(QCryptographicHash::Sha256);

    if (resuming) {
        if (!part.open(QIODevice::ReadWrite)) {
            out.error = i18n::sf("无法打开断点文件继续下载：%1（%2）",
                                 "Unable to reopen the partial download: %1 (%2)",
                                 {partPath, part.errorString()});
            return out;
        }
        // 续传：已有前缀也要进哈希（顺带把读写位置定回断点处）。
        QString prefixError;
        if (!hashExistingPrefix(part, existingBytes, &hasher, &prefixError)) {
            part.close();
            // 断点本身不可信了：删掉，下一轮从头下。
            QFile::remove(partPath);
            out.error = i18n::sf("断点文件不可用，已删除，请重新下载：%1",
                                 "The partial download is unusable and has been deleted; please "
                                 "download it again: %1",
                                 {prefixError});
            return out;
        }
    } else {
        // NewOnly：这个文件必须由我们创建——不跟随别人预置的同名文件 / 符号链接。
        // 0 字节残留（上次打开就失败留下的空壳）先清掉，否则 NewOnly 必然失败。
        if (QFileInfo::exists(partPath) && QFileInfo(partPath).size() == 0) {
            QFile::remove(partPath);
        }
        if (!part.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            out.error = i18n::sf("无法创建下载临时文件：%1（%2）",
                                 "Unable to create the temporary download file: %1 (%2)",
                                 {partPath, part.errorString()});
            return out;
        }
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
        hasher.addData(QByteArrayView(tail.constData(), tail.size()));
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
            // 从零重写：哈希也必须从零开始，否则会算出"旧前缀 + 新内容"的混合值。
            hasher.reset();
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
            hasher.addData(QByteArrayView(block.constData(), block.size()));
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
        out.digestHex = QString::fromLatin1(hasher.result().toHex());
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
    // 写完即算完：不再"事后重新读一遍整个文件再算哈希"。
    out.digestHex = QString::fromLatin1(hasher.result().toHex());
    return out;
}

// 拷贝 + 边拷边算哈希：拷完立刻比对。用于"跨盘安装"与"手动导入"，
// 保证交给 rename 的字节就是刚校验过的那一批，中间没有替换窗口。
bool copyFileHashed(const QString &from,
                    const QString &to,
                    const QString &expectedHex,
                    QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) {
        return fail(i18n::sf("无法打开源文件：%1（%2）", "Unable to open the source file: %1 (%2)",
                             {from, in.errorString()}));
    }

    QFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(i18n::sf("无法写入临时文件：%1（%2）", "Unable to write the temporary file: %1 (%2)",
                             {to, out.errorString()}));
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    QByteArray buffer;
    buffer.resize(static_cast<int>(kChunkSize));

    for (;;) {
        const qint64 got = in.read(buffer.data(), kChunkSize);
        if (got < 0) {
            out.close();
            QFile::remove(to);
            return fail(i18n::sf("读取源文件失败：%1（%2）", "Failed to read the source file: %1 (%2)",
                                 {from, in.errorString()}));
        }
        if (got == 0) {
            break;
        }
        const qint64 wrote = out.write(buffer.constData(), got);
        if (wrote != got) {
            out.close();
            QFile::remove(to);
            return fail(i18n::sf("写入临时文件失败：%1（%2）",
                                 "Failed to write the temporary file: %1 (%2)",
                                 {to, out.errorString()}));
        }
        hasher.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(got)));
    }

    out.flush();
    out.close();
    in.close();

    if (!expectedHex.isEmpty()) {
        const QString actual = QString::fromLatin1(hasher.result().toHex());
        if (actual != expectedHex.trimmed().toLower()) {
            QFile::remove(to);
            return fail(i18n::sf("安装前校验失败：拷贝到临时文件的内容 sha256 为 %1，期望 %2，已放弃安装",
                                 "Pre-install verification failed: the bytes written to the staging "
                                 "file hash to %1 but %2 was expected; the install was abandoned",
                                 {actual, expectedHex}));
        }
    }
    return true;
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

    // id 直接来自调用方：不净化就会被拼成 <root>/models/<id>（卸载时还会被递归删除），
    // 所以先过白名单，任何一道不过都不往磁盘上拼路径。
    QString idError;
    if (!isSafeAssetId(assetId, &idError)) {
        result.error = i18n::sf("资产 id \"%1\" 不安全，拒绝安装：%2",
                                "Unsafe asset id \"%1\"; refusing to install: %2",
                                {assetId, idError});
        return result;
    }

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
    if (partPath.isEmpty()) {
        result.error = i18n::sf("资产 id \"%1\" 无法映射到断点文件路径，拒绝下载",
                                "Asset id \"%1\" cannot be mapped to a partial-file path; refusing to "
                                "download",
                                {assetId});
        return result;
    }

    QString dirError;
    if (!ensureDir(QFileInfo(partPath).absolutePath(), &dirError)) {
        result.error = dirError;
        return result;
    }

    const int sourceCount = static_cast<int>(entry->urls.size());
    QStringList sourceErrors;
    bool ready = false;
    QString actualSha;          // 由写入过程同步算出，不再事后重读文件
    qint64 writtenBytes = 0;    // 我们实际写进 .part 的字节数

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
            actualSha = attempt.digestHex;
            writtenBytes = attempt.received;
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
    // 哈希是在写入过程中同步累积的（downloadOnce 的 hasher），这里绝不重新读一遍再算：
    // "算哈希"与"rename 成正式文件"之间因此没有可以被外部替换的时间窗口（TOCTOU）。
    QString hashError;
    if (actualSha.isEmpty()) {
        // 兜底分支（理论上 Ok/RangeComplete 都带 digest）：宁可退化成老写法，也不能跳过校验。
        actualSha = sha256File(partPath, &hashError);
    }
    if (actualSha.isEmpty()) {
        QFile::remove(partPath);
        result.error = hashError.isEmpty()
                           ? i18n::sf("无法读取下载文件以计算校验值：%1",
                                      "Unable to read the downloaded file to compute its checksum: %1",
                                      {partPath})
                           : hashError;
        return result;
    }

    // 落地大小必须等于"我们写进去的字节数"：中途被截断/追加都能在这一步暴露。
    if (writtenBytes > 0) {
        const qint64 landedBytes = QFileInfo(partPath).size();
        if (landedBytes != writtenBytes) {
            QFile::remove(partPath);
            result.error = i18n::sf(
                "下载文件的大小与写入量不一致（写入 %1 字节，实际 %2 字节），文件可能被并发改写，"
                "已删除，请重新下载。",
                "The downloaded file is %2 bytes on disk but %1 bytes were written; it may have been "
                "modified concurrently. The file has been deleted; please download it again.",
                {QString::number(writtenBytes), QString::number(landedBytes)});
            return result;
        }
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
    // 把刚算出来的哈希一起交下去：finalizeInstall 会在 rename 之前再验一次落地字节。
    QString installError;
    const QString installedPath =
        finalizeInstall(*entry, partPath, true, actualSha, &installError);
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

    QString idError;
    if (!isSafeAssetId(assetId, &idError)) {
        result.error = i18n::sf("资产 id \"%1\" 不安全，拒绝安装：%2",
                                "Unsafe asset id \"%1\"; refusing to install: %2",
                                {assetId, idError});
        return result;
    }

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

    // 用户导入的文件不要求先发布哈希：清单里有哈希就顺手比一次（防止选错文件），
    // 没有就照装，并把这条资产登记成 Import —— 启动前的 verifyInstalled() 不再对它做
    // 内容校验（用户自己的文件，没有可信来源可比）。
    const QString expectedSha = entry->sha256.trimmed().toLower();
    if (expectedSha.isEmpty()) {
        qInfo("[asset] %s 没有清单哈希，按「用户导入」安装，不做内容校验",
              qUtf8Printable(assetId));
    }

    QString actualSha;
    if (!expectedSha.isEmpty()) {
        QString hashError;
        actualSha = sha256File(localFilePath, &hashError);
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
    }

    QString installError;
    const QString installedPath =
        finalizeInstall(*entry, localFilePath, false, actualSha, &installError);
    if (installedPath.isEmpty()) {
        result.error = installError.isEmpty()
                           ? i18n::s("安装失败：未知原因", "Installation failed for an unknown reason")
                           : installError;
        return result;
    }

    result.installedPath = installedPath;

    QString registerError;
    if (!m_registry.markInstalled(assetId, AssetOrigin::Import, &registerError)) {
        result.error = registerError;
        return result;
    }

    result.ok = true;
    return result;
}

QString Downloader::finalizeInstall(const AssetEntry &entry,
                                    const QString &sourcePath,
                                    bool moveSource,
                                    const QString &sourceSha256,
                                    QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return QString();
    };

    QString idError;
    if (!isSafeAssetId(entry.id, &idError)) {
        return fail(i18n::sf("资产 id \"%1\" 不安全，拒绝安装：%2",
                             "Unsafe asset id \"%1\"; refusing to install: %2",
                             {entry.id, idError}));
    }

    const QString target = m_registry.targetPath(entry.id);
    if (target.isEmpty()) {
        return fail(i18n::sf("清单中不存在该资产：%1", "Asset not found in the manifest: %1",
                             {entry.id}));
    }

    QString dirError;
    if (!ensureDir(QFileInfo(target).absolutePath(), &dirError)) {
        return fail(dirError);
    }

    // 旧版本先清掉：rename 到一个已存在的目录在 Windows 上会直接失败。
    if (QFileInfo::exists(target) && !removePath(target)) {
        return fail(i18n::sf("无法清理旧的安装内容：%1（可能被占用，请先关闭正在使用它的程序）",
                             "Unable to remove the previous installation: %1 (it may be in use; "
                             "close any program using it and try again)",
                             {target}));
    }

    // 崩溃留下的 staging 目录/文件（只会是我们自己命名的 "<target>.tmp-<hex>"）顺手清掉，
    // 避免越攒越多；不碰任何别的东西。
    const QFileInfo targetInfo(target);
    const QDir targetDir(targetInfo.absolutePath());
    const QStringList stale = targetDir.entryList(
        {targetInfo.fileName() + QStringLiteral(".tmp-*")},
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString &name : stale) {
        removePath(joinPath(targetInfo.absolutePath(), name));
    }

    // 临时名带随机后缀：并发两个安装不会互相踩，也堵掉"预先放好一个同名临时文件/链接"。
    const QString staging =
        target + QStringLiteral(".tmp-")
        + QString::number(QRandomGenerator::global()->generate64(), 16);

    const bool isZip = entry.archive.compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0;
    if (isZip) {
        // 先解到 <target>.tmp-<rand>，全部成功才改名——中途失败绝不会留下"半个安装"。
        removePath(staging);
        if (!ensureDir(staging, &dirError)) {
            return fail(dirError);
        }

        QString zipError;
        if (!extractZip(sourcePath, staging, entry.size, &zipError)) {
            removePath(staging);
            return fail(zipError);
        }

        if (!QFileInfo(staging).isDir()) {
            removePath(staging);
            return fail(i18n::s("安装失败：解压结果不是目录", "Installation failed: the extraction "
                                                         "result is not a directory"));
        }

        if (!QFile::rename(staging, target)) {
            removePath(staging);
            return fail(i18n::sf("安装失败：无法把解压结果移动到 %1",
                                 "Installation failed: unable to move the extracted files to %1",
                                 {target}));
        }
        return target;
    }

    // 单文件资产（例如 .gguf）：拷/搬到 staging，再改名到最终位置。
    QFile::remove(staging);

    // 同盘 rename 最快且原子；跨盘时退回"边拷边算哈希"，拷完立刻比对。
    // 无论哪条路，交给 rename 的都是刚刚校验过的那份字节。
    QString copyError;
    bool staged = false;
    if (moveSource && QFile::rename(sourcePath, staging)) {
        staged = true;
    } else if (copyFileHashed(sourcePath, staging, sourceSha256, &copyError)) {
        staged = true;
    }

    if (!staged) {
        QFile::remove(staging);
        return fail(copyError.isEmpty()
                        ? i18n::sf("安装失败：无法把下载好的文件移动到 %1",
                                   "Installation failed: unable to move the downloaded file to %1",
                                   {target})
                        : copyError);
    }

    if (!QFile::rename(staging, target)) {
        QFile::remove(staging);
        return fail(i18n::sf("安装失败：无法把临时文件改名为 %1",
                             "Installation failed: unable to rename the temporary file to %1",
                             {target}));
    }
    return target;
}

} // namespace studious::asset
