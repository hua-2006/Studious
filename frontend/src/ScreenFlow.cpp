#include "ScreenFlow.h"

#include "AppSettings.h"
#include "LocalEngine.h"
#include "QuickTranslate.h"

#include "platform/PlatformFactory.h"
#include "platform/Types.h"

#include <studious/ocr/OcrEngine.h>

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace studious::ui {
namespace {

constexpr const char *kTempDirName = "studious-capture";

// 整屏位图与 QML 的 Image 之间只能走文件/URL，所以每次截屏落一个临时 PNG。
// 用固定目录 + 递增序号而不是 QTemporaryDir：QML 的 Image 是异步加载的，
// 目录一旦在加载前被删就会白屏，所以清理交给系统的临时目录策略。
QString shotFilePath()
{
    static quint64 counter = 0;
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                            .filePath(QString::fromLatin1(kTempDirName));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("shot-%1-%2.png")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(++counter));
}

QString ocrModelsDir()
{
    // 与 AppBackend::assetInstallRootPath() 同一套规则（settings.assetRoot 优先）。
    const AppSettings settings = AppSettings::load();
    const QString root = settings.assetRoot.isEmpty()
                             ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                   .filePath(QStringLiteral("assets"))
                             : settings.assetRoot;
    return QDir(QDir(root).filePath(QStringLiteral("models"))).filePath(QStringLiteral("ocr"));
}

// OCR 的 box 是「裁剪图」坐标；浮层要按「相对选区左上角」画，两者是同一套物理像素。
QVariantMap wordToMap(const studious::platform::OcrWord &w)
{
    QVariantMap m;
    m.insert(QStringLiteral("text"), w.text);
    m.insert(QStringLiteral("x"), w.box.x());
    m.insert(QStringLiteral("y"), w.box.y());
    m.insert(QStringLiteral("w"), w.box.width());
    m.insert(QStringLiteral("h"), w.box.height());
    m.insert(QStringLiteral("confidence"), w.confidence);
    return m;
}

} // namespace

ScreenFlow::ScreenFlow(studious::platform::IPlatform *platform, QObject *parent)
    : QObject(parent)
    , m_platform(platform)
{
    if (!m_platform) {
        m_ownedPlatform = studious::platform::createPlatform();
        m_platform = m_ownedPlatform.get();
    }
    m_qt = new QuickTranslate(this);
    connect(m_qt, &QuickTranslate::finished, this, [this](const QuickTranslateResult &r) {
        m_busy = false;
        m_engineLabel = r.engineLabel;
        if (!r.ok) {
            fail(r.error.isEmpty() ? tr("翻译失败。") : r.error);
            return;
        }
        const AppSettings settings = AppSettings::load();
        const QStringList dstList = settings.dstLang.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const QString dst = dstList.value(0, QStringLiteral("zh"));
        const QStringList lines = r.byLang.value(dst);
        // 段落与 OCR 行是一一对应的（QuickTranslate 按空行切段，这里传的就是每行一块），
        // 数量对不上时以块为准：少补空串、多截断，绝不把错位的译文贴到别的行上。
        for (int i = 0; i < m_blocks.size(); ++i) {
            QVariantMap block = m_blocks.at(i).toMap();
            block.insert(QStringLiteral("text"), lines.value(i));
            block.insert(QStringLiteral("untranslated"), lines.value(i).isEmpty());
            m_blocks[i] = block;
        }
        setStatusText(QStringLiteral("%1 · %2 ms").arg(m_engineLabel).arg(r.elapsedMs));
        setPhase(QStringLiteral("Showing"));
        // 设置里的「翻译后自动复制」：只复制第一目标语言的那一份。
        if (AppSettings::load().captureAutoCopy) {
            QStringList copied;
            for (const QVariant &block : std::as_const(m_blocks)) {
                const QString text = block.toMap().value(QStringLiteral("text")).toString();
                if (!text.isEmpty())
                    copied.append(text);
            }
            if (!copied.isEmpty())
                QGuiApplication::clipboard()->setText(copied.join(QLatin1Char('\n')));
        }
        emit blocksChanged();
        emit finished();
    });
    connect(m_qt, &QuickTranslate::errorOccurred, this, [this](const QString &message) {
        m_busy = false;
        fail(message);
    });
}

ScreenFlow::~ScreenFlow() = default;

studious::platform::IPlatform *ScreenFlow::platform() const
{
    return m_platform;
}

QRect ScreenFlow::desktopRect() const
{
    const QRect physical = m_platform ? m_platform->virtualDesktopRect() : QRect();
    if (!physical.isValid())
        return QRect();
    // 物理像素 → 逻辑像素。优先用抓屏实测到的比例（多屏混合 DPI 时它才准），
    // 还没抓过屏就退回主屏 DPR。
    qreal scale = m_pixelScale;
    if (scale <= 0.0 || qFuzzyCompare(scale, 1.0)) {
        const QScreen *primary = QGuiApplication::primaryScreen();
        if (primary && primary->devicePixelRatio() > 0.0)
            scale = primary->devicePixelRatio();
    }
    const QRect logical(QPoint(qRound(physical.x() / scale), qRound(physical.y() / scale)),
                        QSize(qRound(physical.width() / scale), qRound(physical.height() / scale)));
    return logical;
}

bool ScreenFlow::ocrAvailable() const
{
    auto *self = const_cast<ScreenFlow *>(this);
    if (!self->m_ocr)
        self->m_ocr = std::make_unique<studious::ocr::OcrEngine>(ocrModelsDir());
    return self->m_ocr->isAvailable();
}

QString ScreenFlow::ocrMissingText() const
{
    auto *self = const_cast<ScreenFlow *>(this);
    if (!self->m_ocr)
        self->m_ocr = std::make_unique<studious::ocr::OcrEngine>(ocrModelsDir());
    const QStringList missing = self->m_ocr->missingFiles();
    if (missing.isEmpty())
        return self->m_ocr->lastError();
    return tr("缺少 OCR 模型：%1").arg(missing.join(QStringLiteral("、")));
}

void ScreenFlow::setPhase(const QString &phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged();
}

void ScreenFlow::setStatusText(const QString &text)
{
    if (m_statusText == text)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

void ScreenFlow::fail(const QString &message)
{
    m_busy = false;
    m_errorText = message;
    setPhase(QStringLiteral("Error"));
    emit errorTextChanged();
    emit finished();
}

bool ScreenFlow::begin()
{
    if (m_busy)
        return false;

    m_errorText.clear();
    emit errorTextChanged();
    m_blocks.clear();
    m_lastWords.clear();
    emit blocksChanged();
    m_selectionLogical = QRect();
    emit selectionChanged();

    setPhase(QStringLiteral("Capturing"));
    const studious::platform::CapabilityReport caps = platform()->capabilities();
    if (!caps.capture.available) {
        fail(caps.capture.reason.isEmpty() ? tr("这个平台不支持截屏。") : caps.capture.reason);
        return false;
    }

    // 「先抓整屏、再显示遮罩」：顺序反了遮罩会被拍进截图。
    const studious::platform::CaptureResult shot = platform()->captureInteractive();
    if (shot.status != studious::platform::PlatformStatus::Ok || shot.image.isNull()) {
        fail(shot.error.isEmpty() ? tr("抓屏失败。") : shot.error);
        return false;
    }

    m_shot = shot.image;
    m_virtualLogical = QGuiApplication::primaryScreen()
                            ? QGuiApplication::primaryScreen()->virtualGeometry()
                            : QRect();
    // 抓屏是物理像素、QML 是逻辑像素：记下比例，界面里所有 OCR 坐标都按它折回去。
    m_pixelScale = (m_virtualLogical.width() > 0 && m_shot.width() > 0)
                       ? qreal(m_shot.width()) / qreal(m_virtualLogical.width())
                       : 1.0;
    ++m_shotToken;
    setPhase(QStringLiteral("Selecting"));
    setStatusText(tr("%1 × %2 · 拖拽框选要翻译的区域").arg(m_shot.width()).arg(m_shot.height()));
    emit screenChanged();
    emit screenCaptured(m_shot.size());
    return true;
}

void ScreenFlow::confirmSelection(const QRect &region)
{
    if (region.width() < 2 || region.height() < 2) {
        fail(tr("选区宽或高为 0，请重新框选。"));
        return;
    }
    if (m_shot.isNull()) {
        fail(tr("没有抓到屏幕内容，请重新开始。"));
        return;
    }

    // QML 给的是逻辑像素；整屏图是物理像素。按「整屏图宽 / 虚拟桌面逻辑宽」等比放大。
    const qreal scaleX = m_pixelScale;
    const qreal scaleY = m_pixelScale;

    m_selectionLogical = region.normalized();
    QRect physical(qRound(region.x() * scaleX),
                   qRound(region.y() * scaleY),
                   qRound(region.width() * scaleX),
                   qRound(region.height() * scaleY));
    physical = physical.intersected(QRect(QPoint(0, 0), m_shot.size()));
    if (physical.isEmpty()) {
        fail(tr("选区不在屏幕内。"));
        return;
    }
    emit selectionChanged();

    if (!ocrAvailable()) {
        fail(ocrMissingText());
        return;
    }

    setPhase(QStringLiteral("Recognizing"));
    setStatusText(tr("识别中…"));

    const QImage crop = m_shot.copy(physical);
    const quint64 token = m_shotToken;
    // recognize() 的实际签名是 QVector<OcrWord>：空 vector 既可能是"没文字"也可能是
    // "模型没就绪"，靠 lastError() 区分（模型问题在进来之前已经拦过一次，这里是兜底）。
    auto *watcher = new QFutureWatcher<QVector<studious::platform::OcrWord>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token]() {
        const QVector<studious::platform::OcrWord> words = watcher->result();
        watcher->deleteLater();
        if (token != m_shotToken || m_phase == QLatin1String("Idle"))
            return;   // 这次截屏已经被取消/替换，结果作废
        if (words.isEmpty()) {
            const QString err = m_ocr ? m_ocr->lastError() : QString();
            fail(err.isEmpty() ? tr("识别失败：该区域没有检测到文字。") : err);
            return;
        }

        m_lastWords.clear();
        m_blocks.clear();
        for (const studious::platform::OcrWord &w : words) {
            QVariantMap block = wordToMap(w);
            block.insert(QStringLiteral("original"), w.text);
            block.insert(QStringLiteral("text"), QString());
            block.insert(QStringLiteral("untranslated"), true);
            m_blocks.append(block);
            m_lastWords.append(w.text);
        }
        emit blocksChanged();
        startTranslation();
    });

    // OCR 是同步阻塞的重活，必须在工作线程：engine 自己不带锁，按实例独占即可。
    auto *engine = m_ocr.get();
    const studious::ocr::OcrOptions options = [&] {
        studious::ocr::OcrOptions o;
        const AppSettings settings = AppSettings::load();
        // 源语言给了具体值就只认它，否则中英混合（界面文字的绝大多数情况）。
        if (!settings.srcLang.isEmpty() && settings.srcLang != QLatin1String("auto"))
            o.languages = { settings.srcLang };
        return o;
    }();
    watcher->setFuture(QtConcurrent::run([engine, crop, options] { return engine->recognize(crop, options); }));
}

void ScreenFlow::debugSampleBlocks()
{
    // 走查时未必有真选区：给一个 720×200 逻辑像素的假框兜底。
    const QRect selected = physicalSelection();
    const QRect physical = selected.width() > 0
                               ? selected
                               : QRect(0, 0, int(720 * (m_pixelScale > 0 ? m_pixelScale : 1.0)),
                                       int(200 * (m_pixelScale > 0 ? m_pixelScale : 1.0)));
    const int lineHeight = 30;
    const int gap = 14;
    m_blocks.clear();
    const QStringList samples{
        QStringLiteral("你好，世界"),
        QStringLiteral("这是一段用来检查排版的译文"),
        QStringLiteral("第三行：换行与省略号是否能正常裁切"),
    };
    const QStringList originals{
        QStringLiteral("Hello, world"),
        QStringLiteral("This is a sample translation used to check layout"),
        QStringLiteral("Third line: does the ellipsis clip correctly"),
    };
    for (int i = 0; i < samples.size(); ++i) {
        QVariantMap block;
        block.insert(QStringLiteral("text"), samples.at(i));
        block.insert(QStringLiteral("original"), originals.at(i));
        block.insert(QStringLiteral("x"), 4);
        block.insert(QStringLiteral("y"), 4 + i * (lineHeight + gap));
        block.insert(QStringLiteral("w"), qMax(40, physical.width() - 8));
        block.insert(QStringLiteral("h"), lineHeight);
        block.insert(QStringLiteral("untranslated"), false);
        m_blocks.append(block);
    }
    setStatusText(QStringLiteral("调试样张 · %1").arg(m_engineLabel));
    setPhase(QStringLiteral("Showing"));
    emit blocksChanged();
}
void ScreenFlow::startTranslation()
{
    setPhase(QStringLiteral("Translating"));
    setStatusText(tr("翻译中…"));

    const AppSettings settings = AppSettings::load();
    QStringList dst = settings.dstLang.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (dst.isEmpty())
        dst = { QStringLiteral("zh") };

    QStringList paragraphs;
    paragraphs.reserve(m_lastWords.size());
    for (const QVariant &v : std::as_const(m_lastWords))
        paragraphs.append(v.toString());
    const QString text = paragraphs.join(QStringLiteral("\n\n"));

    m_busy = true;
    if (!m_qt->translate(text, settings.srcLang.isEmpty() ? QStringLiteral("auto") : settings.srcLang, dst)) {
        m_busy = false;
        fail(m_qt->lastError().isEmpty() ? tr("翻译没能启动。") : m_qt->lastError());
    }
}

void ScreenFlow::cancel()
{
    if (m_busy)
        m_qt->cancel();
    m_busy = false;
    ++m_shotToken;   // 作废在途的 OCR 结果
    m_selectionLogical = QRect();
    m_blocks.clear();
    m_lastWords.clear();
    m_errorText.clear();
    setPhase(QStringLiteral("Idle"));
    setStatusText(QString());
    emit selectionChanged();
    emit blocksChanged();
    emit errorTextChanged();
}

void ScreenFlow::toggleOriginal()
{
    m_showOriginal = !m_showOriginal;
    setPhase(m_showOriginal ? QStringLiteral("ShowingOriginal") : QStringLiteral("Showing"));
    emit showOriginalChanged();
}

void ScreenFlow::retranslate()
{
    if (m_lastWords.isEmpty()) {
        // 没有 OCR 结果就只能从头来（选区还在，重新识别一次）。
        if (hasSelection())
            confirmSelection(m_selectionLogical);
        return;
    }
    m_errorText.clear();
    emit errorTextChanged();
    startTranslation();
}

bool ScreenFlow::copyResult()
{
    QStringList lines;
    for (const QVariant &v : std::as_const(m_blocks)) {
        const QVariantMap block = v.toMap();
        const QString text = block.value(QStringLiteral("text")).toString();
        lines.append(text.isEmpty() ? block.value(QStringLiteral("original")).toString() : text);
    }
    if (lines.isEmpty())
        return false;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(lines.join(QLatin1Char('\n')));
    return true;
}

bool ScreenFlow::saveToRecords(const QString &source)
{
    // 记录落在 B7 那个面板里；存储层由 AppBackend 持有（一份 db），
    // 这里只是把这一屏的数据整理好交出去，避免两个对象各开一个连接。
    emit saveRecordRequested(m_sourceLabel.isEmpty() ? source : m_sourceLabel,
                             m_shot.isNull() || !hasSelection() ? QString() : imageSourceUrl(),
                             physicalSelection(),
                             m_lastWords,
                             m_blocks);
    return true;
}

QString ScreenFlow::imageSourceUrl() const
{
    if (m_shot.isNull())
        return QString();
    const QString path = shotFilePath();
    if (!m_shot.save(path, "PNG"))
        return QString();
    return QUrl::fromLocalFile(path).toString();
}

QVariantMap ScreenFlow::pixelInfoAt(int x, int y) const
{
    QVariantMap info;
    info.insert(QStringLiteral("ok"), false);
    info.insert(QStringLiteral("x"), x);
    info.insert(QStringLiteral("y"), y);
    info.insert(QStringLiteral("hex"), QString());
    info.insert(QStringLiteral("r"), 0);
    info.insert(QStringLiteral("g"), 0);
    info.insert(QStringLiteral("b"), 0);
    if (m_shot.isNull() || x < 0 || y < 0 || x >= m_shot.width() || y >= m_shot.height())
        return info;

    const QColor c = m_shot.pixelColor(x, y);
    info.insert(QStringLiteral("ok"), true);
    // 交给 QML 直接当颜色字符串用，省得在 QML 里拼十六进制。
    info.insert(QStringLiteral("hex"), c.name(QColor::HexRgb).toUpper());
    info.insert(QStringLiteral("r"), c.red());
    info.insert(QStringLiteral("g"), c.green());
    info.insert(QStringLiteral("b"), c.blue());
    return info;
}

QRect ScreenFlow::physicalSelection() const{
    if (m_shot.isNull() || !hasSelection())
        return QRect();
    const qreal scaleX = m_pixelScale;
    const qreal scaleY = m_pixelScale;
    const QRect r = m_selectionLogical;
    return QRect(qRound(r.x() * scaleX),
                 qRound(r.y() * scaleY),
                 qRound(r.width() * scaleX),
                 qRound(r.height() * scaleY));
}

QRect ScreenFlow::selectionScreenRect() const
{
    // 物理像素 → 虚拟桌面坐标。浮层定位要的是这个（WinPlatform::placeOverlay 只吃物理像素）。
    const QRect desktop = platform()->virtualDesktopRect();
    const QRect sel = physicalSelection();
    return QRect(desktop.topLeft() + sel.topLeft(), sel.size());
}

} // namespace studious::ui
