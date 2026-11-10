#pragma once

#include "asset/AssetRegistry.h"

#include <QString>

#include <atomic>
#include <functional>

namespace studious::net {
class IHttpClient;
}

namespace studious::asset {

// 下载进度快照。进度回调可能很频繁（每 64 KiB 一次），前端别在回调里干重活。
struct DownloadProgress {
    qint64 received = 0;      // 已下载字节（含续传前已有的）
    qint64 total = 0;         // 0 = 服务端未告知
    int    sourceIndex = 0;   // 当前用的第几个 url（0 起）
    int    sourceCount = 1;
    // 阶段名：download / verify / extract / install（done、failed 由 AppBackend 追加）。
    // 解压和安装没有字节进度可报，received/total 保持下载完成时的值，
    // 前端只看 phase 换文案——否则"下载完 100%"会一直停在那儿，看起来像卡住了。
    QString phase = QStringLiteral("download");
};

struct DownloadResult {
    bool ok = false;
    QString error;            // 已本地化，可直接显示
    QString installedPath;    // 成功后的安装位置
};

// 资产下载 / 手动导入。
//
// 关键设计：
//   * 多源 failover：按 AssetEntry.urls 顺序试，某源失败换下一个，全失败才报错（带上每个源的原因）；
//   * 断点续传：先写 <root>/downloads/<id>.part，下次用 Range 续上；服务端不支持 Range（200）就从头重写；
//   * 边写边算 sha256：校验值与落盘是同一遍 I/O，写完整即校验，不存在"算完哈希再 rename"的窗口；
//   * 原子安装：staging 名带随机后缀（<target>.tmp-<hex>），全部成功才 rename 到最终位置；
//   * 解压上限：条目数 ≤ 20000，累计/单条目解压后大小 ≤ max(200 MiB, 清单 size×1.5)；
//   * 同步执行：调用方必须在工作线程调用（内部会起局部事件循环等网络）。
class Downloader {
public:
    Downloader(net::IHttpClient &http, AssetRegistry &registry);

    // 同步下载 + 校验 + 安装。progress 可为空；cancelFlag 可为空（非空时每块检查）。
    // 被取消时保留 .part，下次可续传。
    DownloadResult download(const QString &assetId,
                            std::function<void(const DownloadProgress &)> progress,
                            const std::atomic_bool *cancelFlag = nullptr);

    // 安装本地文件（等价于"手动导入"，比如用户自己下载的 .gguf / .zip）。
    // 同样走 sha256 校验 + 原子落盘，绝不跳过校验。
    DownloadResult installFromFile(const QString &assetId, const QString &localFilePath);

private:
    // 把已校验的源文件安装到目标位置。moveSource=true 表示源文件可以搬走（下载的 .part），
    // false 表示是用户文件，必须复制。sourceSha256 是刚算出的整包哈希：跨盘拷贝时会边拷边比对，
    // 保证交给 rename 的字节就是校验过的那一批。成功返回安装绝对路径，失败返回空串并写 *error。
    // onPhase 可选：安装分几步（解压 / 落盘），调用方据此推进度文案。
    QString finalizeInstall(const AssetEntry &entry,
                            const QString &sourcePath,
                            bool moveSource,
                            const QString &sourceSha256,
                            QString *error,
                            const std::function<void(const QString &)> &onPhase = {});

    net::IHttpClient &m_http;
    AssetRegistry &m_registry;
};

} // namespace studious::asset
