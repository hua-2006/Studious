#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

#include <studious/asset/AssetRegistry.h>
#include <studious/asset/Manifest.h>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;

// 一次资产操作（下载 / 导入 / 删除）的结果：工作线程只回传这份纯数据。
struct AssetOpResult {
    bool ok = false;
    bool cancelled = false;   // 用户点了「取消」（后端会保留 .part，下次续传）
    QString error;            // 已由后端本地化，直接显示，前端不加工
    QString installedPath;
};

// 下载进度的跨线程共享区：工作线程写，主线程 QTimer 轮询读。
//
// 为什么不用 signal：Downloader 的进度回调发生在工作线程、每 64 KiB 一次，
// 直接 emit 到主线程会被 Windows 消息泵放大成风暴；轮询 200 ms 一次足够，
// 且回调里绝不碰任何 QObject / UI。
struct AssetProgressShare {
    std::atomic<qint64> received{0};
    std::atomic<qint64> total{0};
    std::atomic<int> sourceIndex{0};
    std::atomic<int> sourceCount{1};
};

// 「资产」对话框：列清单里的 runtime / model，下载 / 取消 / 删除 / 手动导入。
//
// 线程模型（照抄 TranslateJob 的约定）：
//   * 本类与 m_registry 只活在主线程，用于列表状态 / 刷新 / 总占用；
//   * 下载 / 导入 / 删除各自在 QtConcurrent 工作线程里**另建一个** AssetRegistry
//     （installRoot + manifest 副本）和 QtHttpClient —— 两者都线程亲和，
//     跨线程共享 registry 会踩内部状态缓存的竞态；
//   * 跨线程只搬 AssetOpResult（出）与 AssetProgressShare（进，原子量）。
class AssetDialog : public QDialog {
    Q_OBJECT

public:
    explicit AssetDialog(QWidget *parent = nullptr);
    ~AssetDialog() override;

private slots:
    void onBrowse();
    void onRescan();
    void onPollProgress();
    void onOperationFinished();

private:
    // 当前正在跑的操作种类（同一时刻只允许一个：磁盘是唯一共享点，串行最省心）。
    enum class OpKind { None, Download, Import, Uninstall };

    void buildUi();
    void setUiTexts();

    // 读文件 + parseManifest；showErrors=true 时解析失败弹 QMessageBox（错误原文）。
    bool loadManifest(const QString &path, bool showErrors);
    // 用当前 manifest 重建主线程 registry（会重新扫盘）。
    void rebuildRegistry();
    void rebuildTable();
    void updateTotals();
    void updateInstallRootLabel();

    QWidget *buildOpsWidget(const studious::asset::AssetStatus &status);
    void startOperation(const QString &assetId, OpKind kind, const QString &localPath = QString());
    void onCancelOperation();

    void setStatus(const QString &text, bool ok);
    void setStatusNeutral(const QString &text);

    QString defaultManifestPath() const;
    // 当前清单里占位（urls 为空、未发布）的条数，用于状态行统计。
    int placeholderCount() const;

    // ---------- 数据 ----------
    studious::asset::Manifest m_manifest;
    bool m_hasManifest = false;
    QString m_manifestPath;
    QString m_installRoot;

    // 主线程专用：列表状态 / 总占用 / refresh()。
    std::unique_ptr<studious::asset::AssetRegistry> m_registry;

    // ---------- 异步操作 ----------
    QFutureWatcher<AssetOpResult> *m_watcher = nullptr;
    QTimer *m_pollTimer = nullptr;
    std::shared_ptr<AssetProgressShare> m_progress;      // 与工作线程共享
    std::shared_ptr<std::atomic_bool> m_cancelFlag;      // 与工作线程共享（对话框先关也不悬空）

    bool m_busy = false;
    QString m_busyAssetId;
    OpKind m_busyKind = OpKind::None;

    // ---------- 控件 ----------
    QLabel *m_manifestLabel = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_rescanButton = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_totalLabel = nullptr;
    QLabel *m_rootLabel = nullptr;
    QPushButton *m_closeButton = nullptr;
};
