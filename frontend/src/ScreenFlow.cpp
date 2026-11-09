#include "ScreenFlow.h"

#include "AppSettings.h"
#include "Languages.h"
#include "OcrModels.h"
#include "LocalEngine.h"
// 「词 → 行」的规则搬到了 OcrLines，实时翻译（LiveSession）用同一份。
#include "OcrLines.h"
#include "QuickTranslate.h"

#include "platform/PlatformFactory.h"
#include "platform/Types.h"

#include <studious/ocr/OcrEngine.h>

#include <algorithm>

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace studious::ui {
namespace {

constexpr const char *kTempDirName = "studious-capture";

// 整屏位图与 QML 的 Image 之间只能走文件/URL，所以每次截屏落一个临时 PNG。
// 用固定目录 + 递增序号而不是 QTemporaryDir：QML 的 Image 是异步加载的，
// 目录一旦在加载前被删就会白屏。文件的生命周期由 ScreenFlow 自己管 ——
// m_shotPath 记着当前这一张，releaseShotFile() 负责删（不再指望系统清理临时目录）。
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
    // 三件套的收拢逻辑只有一份（见 OcrModels.cpp）：这里以前抄了一份给 LiveSession 用。
    return studious::ui::ocr::stageModels();
}

// OCR 的 box 是「裁剪图」坐标；浮层要按「相对选区左上角」画，两者是同一套物理像素。
// 一行文字对应一张贴纸：OCR 给的是「词」，翻译与贴纸都按「行」来，所以这里把
// 同一水平带上的词并成一块（见 groupWordsIntoLines()）。
QVariantMap lineToMap(const QRect &box, const QString &text, qreal confidence)
{
    QVariantMap m;
    m.insert(QStringLiteral("x"), box.x());
    m.insert(QStringLiteral("y"), box.y());
    m.insert(QStringLiteral("w"), box.width());
    m.insert(QStringLiteral("h"), box.height());
    m.insert(QStringLiteral("original"), text);
    m.insert(QStringLiteral("text"), QString());
    m.insert(QStringLiteral("untranslated"), true);
    m.insert(QStringLiteral("confidence"), confidence);
    return m;
}

// 归行 / 行内拼接在 OcrLines.h（groupWordsIntoLines），实时翻译共用同一份规则。

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
        // 取消造成的那一次 finished 是预期的，不能当失败弹错误框。
        if (m_cancelling) {
            m_cancelling = false;
            return;
        }
        m_engineLabel = r.engineLabel;
        if (!r.ok) {
            fail(r.error.isEmpty() ? tr("翻译失败。") : r.error);
            return;
        }
        const AppSettings settings = AppSettings::load();
        const QString dst = langs::normalizeTargets(langs::split(settings.dstLang), settings.engineId)
                                .value(0, QStringLiteral("zh"));
        const QStringList lines = r.byLang.value(dst);
        // 段落与 OCR 行是一一对应的（QuickTranslate 按空行切段，这里传的就是每行一块），
        // 数量对不上时以块为准：少补空串、多截断，绝不把错位的译文贴到别的行上。
        for (int i = 0; i < m_blocks.size(); ++i) {
            QVariantMap block = m_blocks.at(i).toMap();
            QString text = lines.value(i);
            // 模型把原文原样退回来（菜单名、型号、已经是目标语言的短句）：
            // 按「未翻译」处理，别把原文当成译文贴上去。
            const QString original = block.value(QStringLiteral("original")).toString().trimmed();
            if (!text.isEmpty() && text.trimmed() == original)
                text.clear();
            block.insert(QStringLiteral("text"), text);
            block.insert(QStringLiteral("untranslated"), text.isEmpty());
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
            if (!copied.isEmpty()) {
                QGuiApplication::clipboard()->setText(copied.join(QLatin1Char('\n')));
                // 「复制后自动关闭」：既然要的文字已经在剪贴板里了，就别让人再盯一眼贴纸。
                // 用 singleShot 收，是因为这会儿还在把 phase 推到 Showing 的路上；
                // 延迟秒数来自设置（0 = 下一轮事件循环就收）。
                const AppSettings autoHide = AppSettings::load();
                if (autoHide.captureAutoHide) {
                    QTimer::singleShot(qBound(0, autoHide.overlayAutoHideDelay, 10) * 1000, this,
                                       [this]() { cancel(); });
                }
            }
        }
        emit blocksChanged();
        // 设置里「保存翻译历史」开着就自动存档，不再要用户点按钮。
        // 放在 blocksChanged 之后：saveToRecords 读的是 m_blocks，得先就位。
        if (AppSettings::load().historyEnabled)
            saveToRecords(QStringLiteral("capture"));
        emit finished();
    });
    connect(m_qt, &QuickTranslate::errorOccurred, this, [this](const QString &message) {
        m_busy = false;
        // 本地 LLM 刚被拉起来：启动要一两秒，这里自己等，别把「正在启动」当失败摆给用户。
        // 20 次 × 1 秒：模型再大也够冷启动了，超了才真的报错。
        if (m_qt->lastFailureRetryable() && m_retryLeft < 20) {
            ++m_retryLeft;
            setPhase(QStringLiteral("Translating"));
            setStatusText(tr("等待本地 LLM 启动（第 %1 次重试）…").arg(m_retryLeft));
            QTimer::singleShot(1000, this, [this] {
                // 用户可能已经关掉浮层/重新框选，那就别再翻旧图。
                if (m_shot.isNull() || m_lastWords.isEmpty()) {
                    return;
                }
                startTranslation();
            });
            return;
        }
        fail(message);
    });
}

ScreenFlow::~ScreenFlow()
{
    // 先断引擎回调再 cancel：cancel() 会同步 emit 一个 finished，
    // 那时候本对象已经在拆了，不该再跑一遍「应用译文」（照 LiveSession 的做法）。
    if (m_qt) {
        disconnect(m_qt, nullptr, this, nullptr);
        if (m_qt->isBusy())
            m_qt->cancel();
    }
    // 再把在飞的识别等回来：OCR 是工作线程里的 ONNX 推理，
    // 放着不管直接析构 QObject，回调会写已释放的 this。
    if (m_ocrWatcher) {
        m_ocrWatcher->waitForFinished();
        // 之后这个 watcher 随 QObject 析构一起没了，排队中的 finished 事件也随之作废。
        m_ocrWatcher = nullptr;
    }
    m_recognizing = false;   // 已等回来了，不再有线程在跑
    // 引擎由 shared_ptr 持有：这里只是放掉自己那一份，线程里那份撑到 recognize() 返回。
    m_ocr.reset();
    // 最后一张整屏 PNG 也删掉：进程退出后没人会再来清临时目录。
    releaseShotFile();
}

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
        self->m_ocr = std::make_shared<studious::ocr::OcrEngine>(ocrModelsDir());
    return self->m_ocr->isAvailable();
}

QString ScreenFlow::ocrMissingText() const
{
    auto *self = const_cast<ScreenFlow *>(this);
    if (!self->m_ocr)
        self->m_ocr = std::make_shared<studious::ocr::OcrEngine>(ocrModelsDir());
    const QStringList missing = self->m_ocr->missingFiles();
    if (missing.isEmpty())
        return self->m_ocr->lastError();
    return tr("缺少 OCR 模型：%1，去资产页下载。").arg(missing.join(QStringLiteral("、")));
}

QString ScreenFlow::ocrTest(const QString &imagePath)
{
    // 开发走查用：拿一张图跑一遍识别，把每个框的文本与置信度打到 stderr。
    // 正常流程不走这里（第 265 行那段是真正的工作线程路径），它只是让"模型到底认不认得字"
    // 这件事能单独验证，不必每次拖一次框、翻一次译再猜。
    const QImage image(imagePath);
    if (image.isNull()) {
        qWarning("[ocr-test] cannot read image: %s", qUtf8Printable(imagePath));
        return QStringLiteral("image");
    }

    studious::ocr::OcrEngine engine(ocrModelsDir());
    if (!engine.isAvailable()) {
        const QString missing = engine.missingFiles().join(QStringLiteral(", "));
        qWarning("[ocr-test] unavailable (%s) %s",
                 qUtf8Printable(missing),
                 qUtf8Printable(engine.lastError()));
        return QStringLiteral("unavailable: ") + missing;
    }

    const QVector<studious::platform::OcrWord> words =
        engine.recognize(image, studious::ocr::OcrOptions{});
    qInfo("[ocr-test] %d word(s) from %dx%d", int(words.size()), image.width(), image.height());
    for (const studious::platform::OcrWord &w : words) {
        qInfo("[ocr-test] %.3f %4d,%4d %4dx%-4d %s",
              w.confidence,
              w.box.x(),
              w.box.y(),
              w.box.width(),
              w.box.height(),
              qUtf8Printable(w.text));
    }
    if (words.isEmpty() && !engine.lastError().isEmpty())
        qWarning("[ocr-test] %s", qUtf8Printable(engine.lastError()));

    // 控制台在中文 Windows 上会把非 GBK 字符换成问号，日文假名根本看不出来。
    // 顺手把结果写成 UTF-8 的旁注文件（<图>.ocr.txt），走查时直接读文件。
    {
        QFile sidecar(imagePath + QStringLiteral(".ocr.txt"));
        if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QString dump;
            for (const studious::platform::OcrWord &w : words) {
                dump += QStringLiteral("%1\t%2,%3\t%4x%5\t%6\n")
                            .arg(w.confidence, 0, 'f', 3)
                            .arg(w.box.x())
                            .arg(w.box.y())
                            .arg(w.box.width())
                            .arg(w.box.height())
                            .arg(w.text);
            }
            sidecar.write(dump.toUtf8());
        }
    }
    return QStringLiteral("%1 word(s)").arg(words.size());
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
    // m_recognizing 单独挡：识别阶段 m_busy 还是 false，
    // 这时再按热键就会和同一份无锁 OcrEngine 撞车。
    if (m_busy || m_recognizing)
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
        // 抓屏失败时把原始原因写进日志：界面上只显示一句人话，排查时总要有个出处。
        qWarning("[capture] interactive capture failed: status=%d size=%dx%d error=%s",
                 int(shot.status), shot.image.width(), shot.image.height(),
                 qUtf8Printable(shot.error));
        fail(shot.error.isEmpty() ? tr("抓屏失败。") : shot.error);
        return false;
    }

    // 上一张整屏图与它的临时 PNG 已经没有用了：先还掉再换新的。
    releaseShotFile();
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
    // 上一次识别还在飞（Esc 取消过也一样：线程收不回来）：这一次直接不收，
    // 免得同一份无锁 OcrEngine 被两个工作线程同时喂。
    if (m_recognizing)
        return;

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
    m_ocrWatcher = watcher;
    m_recognizing = true;
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, token]() {
        // 线程已经收工，之后才允许下一次识别（无论这一单是否已被取消）。
        m_recognizing = false;
        if (m_ocrWatcher == watcher)
            m_ocrWatcher = nullptr;

        const QVector<studious::platform::OcrWord> raw = watcher->result();
        watcher->deleteLater();
        if (token != m_shotToken || m_phase == QLatin1String("Idle"))
            return;   // 这次截屏已经被取消/替换，结果作废

        // 置信度过滤：图标、边框、半透明文字经常被检测成框，识出来是 "XA""6" 这种噪声。
        // 0.5 是实测分界（真文字普遍 0.9+，噪声多在 0.3 以下），也可以自己调。
        constexpr qreal kMinConfidence = 0.5;
        QVector<studious::platform::OcrWord> words;
        words.reserve(raw.size());
        for (const studious::platform::OcrWord &w : raw) {
            if (w.confidence < kMinConfidence)
                continue;
            if (w.text.trimmed().isEmpty())
                continue;
            words.append(w);
        }

        if (words.isEmpty()) {
            const QString err = m_ocr ? m_ocr->lastError() : QString();
            fail(err.isEmpty() ? tr("识别失败：该区域没有检测到文字。") : err);
            return;
        }

        m_lastWords.clear();
        m_blocks.clear();
        // 一个词一块会把"一句话"拆成十几个翻译单元：云端 LLM 要发十几次请求，
        // 本地 MT 更会被碎片弄崩。所以先并成行，再按行贴。
        // text / confidence 现在都由 OcrLines 一起给出（confidence 取行内最小值，
        // 口径没变，只是从这里的循环挪进了 OcrLines）。
        const QVector<OcrLine> lines = groupWordsIntoLines(words);
        for (const OcrLine &line : lines) {
            if (line.text.isEmpty())
                continue;
            m_blocks.append(lineToMap(line.box, line.text, line.confidence));
            m_lastWords.append(line.text);
        }
        emit blocksChanged();
        startTranslation();
    });

    // OCR 是同步阻塞的重活，必须在工作线程：engine 自己不带锁，按实例独占即可。
    // 捕获 shared_ptr 而不是裸指针：本对象即使在识别途中被析构，引擎也不会提前释放。
    const std::shared_ptr<studious::ocr::OcrEngine> engine = m_ocr;
    const studious::ocr::OcrOptions options = [&] {
        studious::ocr::OcrOptions o;
        const AppSettings settings = AppSettings::load();
        // 源语言给了具体值就只认它，否则中英混合（界面文字的绝大多数情况）。
        if (!langs::isAuto(settings.srcLang))
            o.languages = { langs::normalizeSource(settings.srcLang) };
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
    const QStringList dst = langs::normalizeTargets(langs::split(settings.dstLang), settings.engineId);

    QStringList paragraphs;
    paragraphs.reserve(m_lastWords.size());
    for (const QVariant &v : std::as_const(m_lastWords))
        paragraphs.append(v.toString());
    const QString text = paragraphs.join(QStringLiteral("\n\n"));

    // 开发钩子：把每行贴纸的框与原文打出来，用来核对"贴纸没盖住原行"这类几何问题。
    if (qEnvironmentVariableIsSet("STUDIOUS_DEBUG_BLOCKS")) {
        for (int i = 0; i < m_blocks.size() && i < paragraphs.size(); ++i) {
            const QVariantMap block = m_blocks.at(i).toMap();
            qInfo("[blocks] %2d x=%4d y=%4d w=%4d h=%3d %.2f %s", i,
                  block.value(QStringLiteral("x")).toInt(),
                  block.value(QStringLiteral("y")).toInt(),
                  block.value(QStringLiteral("w")).toInt(),
                  block.value(QStringLiteral("h")).toInt(),
                  block.value(QStringLiteral("confidence")).toReal(),
                  qUtf8Printable(paragraphs.at(i)));
        }
    }

    m_busy = true;
    if (!m_qt->translate(text, langs::normalizeSource(settings.srcLang), dst)) {
        // 同步拒绝只有一条路径：QuickTranslate 在返回 false 之前已经 emit 过
        // errorOccurred，那个连接里做了 fail()/重试。这里不再重复处理。
        // 补一条日志：引擎未就绪（缺资产 / 服务没起）时界面上只有一句闪过的提示。
        qWarning("[screenflow] startTranslation: request rejected by engine (lines=%d, src=%s, dst=%s, err=%s)",
                 int(paragraphs.size()), qUtf8Printable(settings.srcLang),
                 qUtf8Printable(dst.join(QLatin1Char(','))), qUtf8Printable(m_qt->lastError()));
        m_busy = false;
        return;
    }
    // 真的发出去了：之前的启动重试计数清零。
    m_retryLeft = 0;
}

void ScreenFlow::cancel()
{
    // 和 cancelTranslation() 同一套：先在 cancel() 之前置标志，
    // 这样 QuickTranslate 同步回来的那次「已取消」finished 会被守卫吃掉，
    // 不会先 fail 到 Error 再回 Idle。取消永远不弹错误。
    if (m_qt->isBusy()) {
        m_cancelling = true;
        m_qt->cancel();
    }
    m_busy = false;
    ++m_shotToken;   // 作废在途的 OCR 结果（识别线程本身收不回来，靠代号丢弃）
    // 整个流程结束：整屏位图（双 4K ≈ 30-60MB）和落盘的 PNG 都该还回去了。
    // m_shot 以前只在下次截屏时被覆盖 —— 浮层关掉之后这份图没人再用，却常驻到进程结束。
    releaseShotFile();
    m_selectionLogical = QRect();
    m_blocks.clear();
    m_lastWords.clear();
    m_errorText.clear();
    setPhase(QStringLiteral("Idle"));
    setStatusText(QString());
    emit screenChanged();   // 整屏图已经放掉了：尺寸归零，QML 的遮罩底图跟着关
    emit selectionChanged();
    emit blocksChanged();
    emit errorTextChanged();
}

void ScreenFlow::cancelTranslation()
{
    // m_recognizing 也算「在干活」：识别阶段取消以前会被当成没事发生，
    // 代号不推，OCR 回来照样往下翻译。
    const bool wasWorking = m_busy || m_recognizing || m_qt->isBusy();
    if (m_qt->isBusy()) {
        m_cancelling = true;
        m_qt->cancel();     // 会同步发一个「已取消」的 finished，被上面的守卫吃掉
    }
    m_busy = false;
    if (!wasWorking) {
        return;             // 没在跑：别动当前阶段（例如已经 Showing 了）
    }
    // 作废在途的 OCR 结果：识别阶段取消时，回调回来也不能再往下翻译。
    ++m_shotToken;
    m_errorText.clear();
    emit errorTextChanged();
    setStatusText(tr("已取消。"));
    // 选区还在 → 回到「框选好但还没翻译」的状态，工具栏可以直接再按翻译；
    // 没选区（理论上不会）就老实回 Idle。
    setPhase(hasSelection() ? QStringLiteral("Selecting") : QStringLiteral("Idle"));
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
        const QString original = block.value(QStringLiteral("original")).toString();
        const QString translated = block.value(QStringLiteral("text")).toString();
        // 复制要跟屏幕上看到的一致：正在显示原文就复制原文，显示译文就复制译文。
        // 缺哪一边就退回另一边，避免某些块只识别出一半时复制出空行。
        lines.append(m_showOriginal ? (original.isEmpty() ? translated : original)
                                    : (translated.isEmpty() ? original : translated));
    }
    if (lines.isEmpty())
        return false;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(lines.join(QLatin1Char('\n')));

    // 复制完就把浮层收掉（设置里「复制后自动关闭」，默认开）：用户要的是这段文字，
    // 不是继续盯着屏幕上的贴纸。延迟秒数允许用户调（0 = 立刻）。
    const AppSettings copySettings = AppSettings::load();
    if (copySettings.captureAutoHide && (m_phase == QLatin1String("Showing")
                                         || m_phase == QLatin1String("ShowingOriginal"))) {
        QTimer::singleShot(qBound(0, copySettings.overlayAutoHideDelay, 10) * 1000, this,
                           [this]() { cancel(); });
    }
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
    // 同一张图只落一次盘：QML 里每求值一次绑定都会调到这里，以前是每调一次写一张新 PNG
    // （文件名带自增计数，永远不重复、也永远不删）。
    if (!m_shotUrl.isEmpty())
        return m_shotUrl;
    const QString path = shotFilePath();
    if (!m_shot.save(path, "PNG")) {
        QFile::remove(path);
        return QString();
    }
    m_shotPath = path;
    m_shotUrl = QUrl::fromLocalFile(path).toString();
    return m_shotUrl;
}

void ScreenFlow::releaseShotFile()
{
    if (!m_shotPath.isEmpty()) {
        QFile::remove(m_shotPath);
    }
    m_shotPath.clear();
    m_shotUrl.clear();
    m_shot = QImage();
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
