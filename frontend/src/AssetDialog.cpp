#include "AssetDialog.h"

#include "AssetFormat.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <studious/asset/Downloader.h>
#include <studious/i18n/L10n.h>
#include <studious/net/QtHttpClient.h>

namespace {

// 界面文字一律走 i18n：跟随「设置 → 界面语言」（重启生效）。
QString t(const char *zh, const char *en)
{
    return studious::i18n::s(zh, en);
}

QString tf(const char *zh, const char *en, const QVariantList &args)
{
    return studious::i18n::sf(zh, en, args);
}

const char *const kColorOk = "#1e8449";
const char *const kColorError = "#c0392b";
const char *const kColorNeutral = "#7f8c8d";

const char *const kStyleOk = "color: #1e8449; font-weight: 600;";
const char *const kStyleError = "color: #c0392b; font-weight: 600;";
const char *const kStyleNeutral = "color: #7f8c8d;";
const char *const kStyleSmall = "color: #7f8c8d; font-size: 11px;";

// 一次异步操作的完整入参（QtConcurrent::run 只接受可拷贝的参数，打包成一个结构最稳）。
//
// 注意：manifest 是**副本**。工作线程用它现造一个 AssetRegistry，
// 与主线程那个 registry 完全独立——磁盘是它们唯一的共享点。
struct AssetOpRequest {
    enum class Kind { Download, Import, Uninstall };

    Kind kind = Kind::Download;
    studious::asset::Manifest manifest;
    QString installRoot;
    QString assetId;
    QString localPath;                                  // 仅 Import 用
    std::shared_ptr<AssetProgressShare> progress;        // 仅 Download 用
    std::shared_ptr<std::atomic_bool> cancel;            // 仅 Download 用
};

// 工作线程入口：registry / http / downloader 全部在本线程内构造、使用、销毁。
AssetOpResult runAssetOperation(AssetOpRequest req)
{
    AssetOpResult out;

    studious::asset::AssetRegistry registry(req.installRoot, req.manifest);
    registry.refresh();

    if (req.kind == AssetOpRequest::Kind::Uninstall) {
        QString error;
        if (!registry.uninstall(req.assetId, &error)) {
            out.error = error;
            return out;
        }
        out.ok = true;
        return out;
    }

    studious::net::QtHttpClient http;   // 必须与 downloader 同线程
    studious::asset::Downloader downloader(http, registry);

    if (req.kind == AssetOpRequest::Kind::Import) {
        const studious::asset::DownloadResult result =
            downloader.installFromFile(req.assetId, req.localPath);
        out.ok = result.ok;
        out.error = result.error;
        out.installedPath = result.installedPath;
        return out;
    }

    // ---- 下载：回调里只写原子量，绝不碰 UI / QObject ----
    const std::shared_ptr<AssetProgressShare> share = req.progress;
    const std::shared_ptr<std::atomic_bool> cancel = req.cancel;

    const auto onProgress = [share](const studious::asset::DownloadProgress &progress) {
        if (!share) {
            return;
        }
        share->received.store(progress.received, std::memory_order_relaxed);
        share->total.store(progress.total, std::memory_order_relaxed);
        share->sourceIndex.store(progress.sourceIndex, std::memory_order_relaxed);
        share->sourceCount.store(progress.sourceCount, std::memory_order_relaxed);
    };

    const studious::asset::DownloadResult result =
        downloader.download(req.assetId, onProgress, cancel ? cancel.get() : nullptr);

    out.ok = result.ok;
    out.error = result.error;
    out.installedPath = result.installedPath;
    out.cancelled = !result.ok && cancel && cancel->load(std::memory_order_relaxed);
    return out;
}

QColor stateColor(studious::asset::AssetState state)
{
    switch (state) {
    case studious::asset::AssetState::Installed:
        return QColor(QString::fromLatin1(kColorOk));
    case studious::asset::AssetState::Corrupted:
        return QColor(QString::fromLatin1(kColorError));
    case studious::asset::AssetState::NotInstalled:
        break;
    }
    return QColor(QString::fromLatin1(kColorNeutral));
}

} // namespace

AssetDialog::AssetDialog(QWidget *parent)
    : QDialog(parent)
    , m_installRoot(QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                    + QStringLiteral("/assets")))
    , m_watcher(new QFutureWatcher<AssetOpResult>(this))
    , m_pollTimer(new QTimer(this))
    , m_cancelFlag(std::make_shared<std::atomic_bool>(false))
{
    setModal(true);
    resize(1080, 620);

    buildUi();
    setUiTexts();

    connect(m_watcher, &QFutureWatcher<AssetOpResult>::finished,
            this, &AssetDialog::onOperationFinished);
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout, this, &AssetDialog::onPollProgress);

    updateInstallRootLabel();

    const QString defaultPath = defaultManifestPath();
    if (defaultPath.isEmpty()) {
        setStatusNeutral(t("未找到默认清单文件，请点「浏览…」选择 manifest.json",
                           "No default manifest found. Use “Browse…” to pick a manifest.json."));
        rebuildTable();
    } else {
        // 首次自动加载失败时静默（只在状态行里留原因），避免一开对话框就弹框。
        loadManifest(defaultPath, false);
    }
}

AssetDialog::~AssetDialog()
{
    // 关窗时若还有下载在飞：先置取消标记再等它退出。
    // 这样可避免工作线程在 QApplication 销毁后仍在摸 Qt 网络栈 / 写磁盘。
    if (m_watcher->isRunning()) {
        m_cancelFlag->store(true, std::memory_order_relaxed);
        m_watcher->waitForFinished();
    }
}

void AssetDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    // ---------- 顶部：清单文件 ----------
    auto *pathRow = new QHBoxLayout;
    pathRow->setSpacing(6);

    m_pathEdit = new QLineEdit;
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setMinimumWidth(420);

    m_browseButton = new QPushButton;
    m_rescanButton = new QPushButton;

    m_manifestLabel = new QLabel;   // 文字在 setUiTexts() 里统一设置
    pathRow->addWidget(m_manifestLabel, 0);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(m_browseButton);
    pathRow->addWidget(m_rescanButton);
    root->addLayout(pathRow);

    // ---------- 表格 ----------
    m_table = new QTableWidget(0, 6);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setColumnWidth(0, 300);

    QHeaderView *header = m_table->horizontalHeader();
    // 1~3 列用 Fixed：ResizeToContents 在带颜色的 item 上会把「未安装」这类中文裁掉半个字。
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Fixed);
    header->setSectionResizeMode(2, QHeaderView::Fixed);
    header->setSectionResizeMode(3, QHeaderView::Fixed);
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    header->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->setColumnWidth(1, 80);
    m_table->setColumnWidth(2, 100);
    m_table->setColumnWidth(3, 100);
    root->addWidget(m_table, 1);

    // ---------- 底部：进度条 + 状态文本 + 已安装总计 ----------
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 1000);
    m_progressBar->setValue(0);
    m_progressBar->setMinimumWidth(240);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_totalLabel = new QLabel;
    m_totalLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_totalLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    statusRow->addWidget(m_progressBar, 0);
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_totalLabel, 0);
    root->addLayout(statusRow);

    // ---------- 安装根目录（小字）+ 关闭 ----------
    auto *footRow = new QHBoxLayout;
    footRow->setSpacing(10);

    m_rootLabel = new QLabel;
    m_rootLabel->setStyleSheet(QString::fromLatin1(kStyleSmall));
    m_rootLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_rootLabel->setWordWrap(true);

    m_closeButton = new QPushButton;
    m_closeButton->setDefault(true);

    footRow->addWidget(m_rootLabel, 1);
    footRow->addWidget(m_closeButton, 0);
    root->addLayout(footRow);

    connect(m_browseButton, &QPushButton::clicked, this, &AssetDialog::onBrowse);
    connect(m_rescanButton, &QPushButton::clicked, this, &AssetDialog::onRescan);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void AssetDialog::setUiTexts()
{
    setWindowTitle(t("资产 — 模型 / 运行时管理", "Assets — Models / Runtimes"));

    if (m_manifestLabel != nullptr) {
        m_manifestLabel->setText(t("清单文件", "Manifest"));
    }

    m_pathEdit->setPlaceholderText(t("尚未选择清单文件", "No manifest selected"));
    m_browseButton->setText(t("浏览…", "Browse…"));
    m_rescanButton->setText(t("重新扫描", "Rescan"));
    m_closeButton->setText(t("关闭", "Close"));

    m_browseButton->setToolTip(t("选择一个 manifest.json（示例清单未签名，按开发模式解析）",
                                 "Pick a manifest.json (the sample manifest is unsigned, parsed in dev "
                                 "mode)"));
    m_rescanButton->setToolTip(t("重新扫描安装目录，刷新每个资产的状态与占用体积",
                                 "Rescan the install root and refresh every asset status and size"));

    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("ID"),
        t("类型", "Type"),
        t("大小", "Size"),
        t("状态", "Status"),
        t("说明", "Notes"),
        t("操作", "Actions"),
    });

    if (!m_busy) {
        setStatusNeutral(t("就绪", "Ready"));
    }
}

QString AssetDialog::defaultManifestPath() const
{
    // frontend/build/studious.exe -> ../../backend/asset/manifest.json
    // = <repo>/backend/asset/manifest.json（构建产物在 frontend/build 下）。
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString candidate =
        QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../../backend/asset/manifest.json")));
    return QFileInfo::exists(candidate) ? candidate : QString();
}

bool AssetDialog::loadManifest(const QString &path, bool showErrors)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString error = tf("打开清单失败：%1", "Failed to open the manifest: %1",
                                 {file.errorString()});
        if (showErrors) {
            QMessageBox::warning(this, t("清单解析失败", "Manifest error"), error);
        }
        setStatus(error, false);
        return false;
    }

    const QByteArray bytes = file.readAll();
    file.close();

    // 仓库里的 backend/asset/manifest.json 是给人看的示例清单：
    // 没有 sig（不验签），sha256 全是发布前才填的占位空串（允许缺失）。
    // 发布路径永远不要打开这两个开关。
    studious::asset::ManifestVerifyOptions opts;
    opts.requireSignature = false;
    opts.allowMissingSha256 = true;
    opts.maxSchemaVersion = 1;

    const studious::asset::ManifestLoadResult result =
        studious::asset::parseManifest(bytes, opts);
    if (!result.ok) {
        // 后端的 error 已经本地化，原样显示，前端不拼前缀不改写。
        if (showErrors) {
            QMessageBox::warning(this, t("清单解析失败", "Manifest error"), result.error);
        }
        setStatus(result.error, false);
        return false;
    }

    m_manifest = result.manifest;
    m_hasManifest = true;
    m_manifestPath = path;
    m_pathEdit->setText(QDir::toNativeSeparators(path));
    m_pathEdit->setToolTip(QDir::toNativeSeparators(path));

    rebuildRegistry();
    setStatusNeutral(tf("已载入清单：共 %1 条（其中占位 %2 条）",
                        "Manifest loaded: %1 entries (%2 placeholders)",
                        {QString::number(m_manifest.assets.size()), QString::number(placeholderCount())}));
    return true;
}

void AssetDialog::rebuildRegistry()
{
    if (!m_hasManifest) {
        m_registry.reset();
        rebuildTable();
        return;
    }

    m_registry = std::make_unique<studious::asset::AssetRegistry>(m_installRoot, m_manifest);
    m_registry->refresh();
    rebuildTable();
}

void AssetDialog::rebuildTable()
{
    m_table->clearContents();
    m_table->setRowCount(0);

    if (!m_registry) {
        updateTotals();
        return;
    }

    // listStatus() 顺序 = 清单顺序，正好与 m_manifest.assets 对齐（这里仍按 id 查，防将来变序）。
    const QVector<studious::asset::AssetStatus> statuses = m_registry->listStatus();
    m_table->setRowCount(statuses.size());

    for (int row = 0; row < statuses.size(); ++row) {
        const studious::asset::AssetStatus &status = statuses.at(row);
        const studious::asset::AssetEntry *entry = m_manifest.find(status.id);
        const bool placeholder = entry != nullptr && entry->isPlaceholder();

        auto *idItem = new QTableWidgetItem(status.id);
        idItem->setToolTip(status.installPath.isEmpty()
                               ? t("尚未安装", "Not installed")
                               : QDir::toNativeSeparators(status.installPath));
        m_table->setItem(row, 0, idItem);

        m_table->setItem(row, 1, new QTableWidgetItem(entry != nullptr ? entry->type : QString()));

        const qint64 size = entry != nullptr ? entry->size : status.installedBytes;
        auto *sizeItem = new QTableWidgetItem(studious::frontend::humanBytes(size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, 2, sizeItem);

        auto *stateItem =
            new QTableWidgetItem(studious::frontend::assetStateText(status.state));
        stateItem->setForeground(stateColor(status.state));
        if (!status.detail.isEmpty()) {
            stateItem->setToolTip(status.detail);   // 后端给的本地化细节（如缺哪个文件）
        }
        m_table->setItem(row, 3, stateItem);

        auto *noteItem = new QTableWidgetItem(
            placeholder ? t("未发布", "Not published") : (entry != nullptr ? entry->note : QString()));
        if (entry != nullptr && !entry->note.isEmpty()) {
            noteItem->setToolTip(entry->note);   // 占位行也能看到后端给的原始说明
        }
        m_table->setItem(row, 4, noteItem);

        m_table->setCellWidget(row, 5, buildOpsWidget(status));
    }

    updateTotals();
}

QWidget *AssetDialog::buildOpsWidget(const studious::asset::AssetStatus &status)
{
    auto *host = new QWidget;
    auto *layout = new QHBoxLayout(host);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(4);

    const QString id = status.id;

    // 正在跑的这个资产 -> 只给「取消」；其余行整行禁用（磁盘是唯一共享点，必须串行）。
    if (m_busy && m_busyAssetId == id) {
        if (m_busyKind == OpKind::Download) {
            auto *cancel = new QPushButton(t("取消", "Cancel"));
            cancel->setToolTip(t("取消下载，已下载的部分会保留，下次可续传",
                                 "Cancel the download; the partial file is kept and resumes next time"));
            connect(cancel, &QPushButton::clicked, this, [this, id]() {
                Q_UNUSED(id);
                onCancelOperation();
            });
            layout->addWidget(cancel);
        } else {
            // 导入 / 删除是后端的一次性同步动作，没有可中断的语义，只显示进度文案。
            auto *working = new QPushButton(m_busyKind == OpKind::Import ? t("导入中…", "Importing…")
                                                                        : t("删除中…", "Deleting…"));
            working->setEnabled(false);
            layout->addWidget(working);
        }
        layout->addStretch(1);
        return host;
    }

    if (status.state == studious::asset::AssetState::Installed) {
        auto *remove = new QPushButton(t("删除", "Delete"));
        remove->setEnabled(!m_busy);
        connect(remove, &QPushButton::clicked, this, [this, id]() {
            // 删除不可撤销（目录递归删，连 .part 一起清），必须先问一句。
            // 手写 QMessageBox 而不用 QMessageBox::question：标准按钮的文案来自 Qt 自带翻译，
            // 没装 qt_zh_CN 时会显示 Yes/No，与整个界面的语言不一致。
            QMessageBox box(this);
            box.setIcon(QMessageBox::Question);
            box.setWindowTitle(t("删除资产", "Delete asset"));
            box.setText(tf("确定删除资产 %1 吗？\n安装文件与未完成的下载都会被删除，无法恢复。",
                           "Delete asset %1?\nThe installed files and any partial download are "
                           "removed for good.",
                           {id}));
            QPushButton *yes = box.addButton(t("删除", "Delete"), QMessageBox::AcceptRole);
            QPushButton *cancel = box.addButton(t("取消", "Cancel"), QMessageBox::RejectRole);
            box.setDefaultButton(cancel);   // 默认落在「取消」，避免手滑回车误删
            box.setEscapeButton(cancel);
            box.exec();
            if (box.clickedButton() != yes) {
                return;
            }
            startOperation(id, OpKind::Uninstall);
        });
        layout->addWidget(remove);
    } else {
        auto *download = new QPushButton(t("下载", "Download"));
        download->setEnabled(!m_busy);
        connect(download, &QPushButton::clicked, this, [this, id]() { startOperation(id, OpKind::Download); });

        auto *import = new QPushButton(t("导入…", "Import…"));
        import->setEnabled(!m_busy);
        connect(import, &QPushButton::clicked, this, [this, id]() {
            const QString path = QFileDialog::getOpenFileName(
                this,
                t("选择本地资产文件", "Choose a local asset file"),
                QString(),
                t("所有文件 (*.*)", "All files (*.*)"));
            if (path.isEmpty()) {
                return;
            }
            startOperation(id, OpKind::Import, path);
        });

        layout->addWidget(download);
        layout->addWidget(import);
    }

    layout->addStretch(1);
    return host;
}

void AssetDialog::updateTotals()
{
    const qint64 total = m_registry ? m_registry->totalInstalledBytes() : 0;
    m_totalLabel->setText(tf("已安装总计: %1", "Installed total: %1",
                             {studious::frontend::humanBytes(total)}));
}

void AssetDialog::updateInstallRootLabel()
{
    m_rootLabel->setText(tf("安装根目录：%1", "Install root: %1",
                            {QDir::toNativeSeparators(m_installRoot)}));
    m_rootLabel->setToolTip(m_rootLabel->text());
}

int AssetDialog::placeholderCount() const
{
    int count = 0;
    for (const studious::asset::AssetEntry &entry : m_manifest.assets) {
        if (entry.isPlaceholder()) {
            ++count;
        }
    }
    return count;
}

void AssetDialog::setStatus(const QString &text, bool ok)
{
    m_status->setStyleSheet(QString::fromLatin1(ok ? kStyleOk : kStyleError));
    m_status->setText(text);
}

void AssetDialog::setStatusNeutral(const QString &text)
{
    m_status->setStyleSheet(QString::fromLatin1(kStyleNeutral));
    m_status->setText(text);
}

void AssetDialog::onBrowse()
{
    if (m_busy) {
        setStatusNeutral(t("当前操作尚未结束，请稍候再切换清单",
                           "An operation is still running; wait for it to finish before switching "
                           "manifests"));
        return;
    }

    const QString start = m_manifestPath.isEmpty()
                              ? QCoreApplication::applicationDirPath()
                              : QFileInfo(m_manifestPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this,
                                                      t("选择资产清单", "Choose an asset manifest"),
                                                      start,
                                                      t("清单文件 (*.json);;所有文件 (*.*)",
                                                        "Manifest (*.json);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    loadManifest(path, true);
}

void AssetDialog::onRescan()
{
    if (m_busy) {
        setStatusNeutral(t("当前操作尚未结束，请稍候再重新扫描",
                           "An operation is still running; wait for it to finish before rescanning"));
        return;
    }

    if (!m_registry) {
        setStatusNeutral(t("尚未载入清单文件", "No manifest loaded yet"));
        return;
    }

    m_registry->refresh();
    rebuildTable();
    setStatusNeutral(tf("已重新扫描：共 %1 条，已安装总计 %2",
                        "Rescanned: %1 entries, %2 installed",
                        {QString::number(m_table->rowCount()),
                         studious::frontend::humanBytes(m_registry->totalInstalledBytes())}));
}

void AssetDialog::startOperation(const QString &assetId, OpKind kind, const QString &localPath)
{
    if (m_busy || !m_registry || !m_hasManifest || m_watcher->isRunning()) {
        return;
    }

    // 「取消」标记按操作重置：下载成功后它必须回到 false，否则下一次一启动就被取消。
    m_cancelFlag->store(false, std::memory_order_relaxed);
    m_progress = std::make_shared<AssetProgressShare>();

    m_busy = true;
    m_busyAssetId = assetId;
    m_busyKind = kind;
    rebuildTable();   // 该行换成「取消」，其它行禁用

    m_progressBar->setRange(0, 0);   // 未知总量 -> 不确定态
    m_progressBar->setValue(0);

    AssetOpRequest req;
    req.manifest = m_manifest;   // 副本：工作线程自建 registry，不与主线程共享
    req.installRoot = m_installRoot;
    req.assetId = assetId;
    req.localPath = localPath;
    req.progress = m_progress;
    req.cancel = m_cancelFlag;

    switch (kind) {
    case OpKind::Download:
        req.kind = AssetOpRequest::Kind::Download;
        setStatusNeutral(tf("下载中… %1", "Downloading… %1", {assetId}));
        break;
    case OpKind::Import:
        req.kind = AssetOpRequest::Kind::Import;
        setStatusNeutral(tf("导入中… %1（来源：%2）",
                            "Importing… %1 (from %2)",
                            {assetId, QDir::toNativeSeparators(localPath)}));
        break;
    case OpKind::Uninstall:
        req.kind = AssetOpRequest::Kind::Uninstall;
        m_progressBar->setRange(0, 1000);
        m_progressBar->setValue(0);
        setStatusNeutral(tf("删除中… %1", "Deleting… %1", {assetId}));
        break;
    case OpKind::None:
        return;
    }

    m_pollTimer->start();
    m_watcher->setFuture(QtConcurrent::run(&runAssetOperation, req));
}

void AssetDialog::onCancelOperation()
{
    if (!m_busy || m_busyKind != OpKind::Download) {
        return;
    }
    m_cancelFlag->store(true, std::memory_order_relaxed);
    setStatusNeutral(t("正在取消…", "Cancelling…"));
}

void AssetDialog::onPollProgress()
{
    if (!m_busy || m_busyKind != OpKind::Download || !m_progress) {
        return;
    }

    const qint64 received = m_progress->received.load(std::memory_order_relaxed);
    const qint64 total = m_progress->total.load(std::memory_order_relaxed);
    const int sourceIndex = m_progress->sourceIndex.load(std::memory_order_relaxed);
    const int sourceCount = m_progress->sourceCount.load(std::memory_order_relaxed);

    if (total > 0) {
        m_progressBar->setRange(0, 1000);
        const qint64 perMille = qMin<qint64>(1000, received * 1000 / total);
        m_progressBar->setValue(static_cast<int>(perMille));
    } else {
        m_progressBar->setRange(0, 0);
    }

    const QString totalText = total > 0 ? studious::frontend::humanBytes(total)
                                        : t("未知", "unknown");
    setStatusNeutral(tf("下载中… %1 / %2（源 %3/%4）",
                        "Downloading… %1 / %2 (source %3/%4)",
                        {studious::frontend::humanBytes(received),
                         totalText,
                         QString::number(sourceIndex + 1),
                         QString::number(qMax(sourceCount, 1))}));
}

void AssetDialog::onOperationFinished()
{
    m_pollTimer->stop();

    const AssetOpResult result = m_watcher->result();
    const OpKind kind = m_busyKind;

    m_busy = false;
    m_busyAssetId.clear();
    m_busyKind = OpKind::None;
    m_progress.reset();

    m_progressBar->setRange(0, 1000);
    m_progressBar->setValue(0);

    // 磁盘已经变了：主线程 registry 的缓存必须重新扫。
    if (m_registry) {
        m_registry->refresh();
    }
    rebuildTable();

    if (result.cancelled) {
        setStatusNeutral(t("已取消（已下载部分保留，可继续）",
                           "Cancelled (the partial download is kept and can be resumed)"));
        m_status->setToolTip(result.error);   // 后端原文（含"保留断点"措辞）放 tooltip
        return;
    }

    if (!result.ok) {
        // 后端 error 已本地化，原样显示，前端不加工。
        m_status->setToolTip(QString());
        setStatus(result.error.isEmpty()
                      ? t("操作失败（后端未给出原因）", "Operation failed (no reason given by the backend)")
                      : result.error,
                  false);
        return;
    }

    m_status->setToolTip(QString());
    switch (kind) {
    case OpKind::Download:
        setStatus(tf("下载完成：%1", "Download complete: %1",
                     {QDir::toNativeSeparators(result.installedPath)}),
                  true);
        break;
    case OpKind::Import:
        setStatus(tf("导入完成：%1", "Import complete: %1",
                     {QDir::toNativeSeparators(result.installedPath)}),
                  true);
        break;
    case OpKind::Uninstall:
        setStatus(t("已删除", "Deleted"), true);
        break;
    case OpKind::None:
        break;
    }
}
