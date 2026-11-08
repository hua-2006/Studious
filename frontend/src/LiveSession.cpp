#include "LiveSession.h"

#include "AppSettings.h"
#include "Languages.h"
#include "OcrModels.h"
#include "QuickTranslate.h"

#include "platform/PlatformFactory.h"
#include "platform/Types.h"

#include <studious/ocr/OcrEngine.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace studious::ui {
namespace {

// 一帧的翻译超过 1 秒，用户就能肉眼看出「刷新跟不上画面」。
// 600 ms 的节拍配 1.1 s 的引擎 = 每两帧才出一份译文；与其让人以为卡死，
// 不如直接说明，让他去换引擎或接受这个效果。
constexpr qint64 kSlowThresholdMs = 1000;

// 连续认不出字时只更新状态文字，不再收单：实时翻译要一直盯着画面，
// 「这几帧没字」是常态（过场、黑屏、对话框关掉），不该判死。

// 与 ScreenFlow 同一个阈值：图标、边框、半透明文字常被检测成框，
// 识出来是 "XA""6" 这种噪声。真文字普遍 0.9+，噪声多在 0.3 以下。
constexpr qreal kMinConfidence = 0.5;


QString ocrModelsDir()
{
    // 三件套的收拢逻辑只有一份（见 OcrModels.cpp）：这里以前抄了一份给 LiveSession 用。
    return studious::ui::ocr::stageModels();
}

studious::platform::CaptureRect toCaptureRect(const QRect &r)
{
    return studious::platform::CaptureRect { r.x(), r.y(), r.width(), r.height() };
}

// 块的形状跟 ScreenFlow 的 lineToMap() 一致（QML 两个页面用的是同一套字段），
// 只是 original 与 text 在实时这边是**同时**到齐的，所以一次填完。
QVariantMap blockMap(const QRect &box, const QString &original, const QString &text, qreal confidence)
{
    QVariantMap m;
    m.insert(QStringLiteral("x"), box.x());
    m.insert(QStringLiteral("y"), box.y());
    m.insert(QStringLiteral("w"), box.width());
    m.insert(QStringLiteral("h"), box.height());
    m.insert(QStringLiteral("original"), original);
    m.insert(QStringLiteral("text"), text);
    // 译文缺失（引擎漏段、或这一行本身就没什么好翻）→ 让 QML 别画贴纸，
    // 直接露出屏幕上的原文（设计稿 C7），比盖一块空白好。
    m.insert(QStringLiteral("untranslated"), text.isEmpty());
    m.insert(QStringLiteral("confidence"), confidence);
    return m;
}

// 目标语言在 AppSettings 里是逗号分隔的一串（dstLang），实时只用第一个：
// 一屏上贴两种译法没法读。
QString firstDstLang(const AppSettings &settings)
{
    return langs::normalizeTargets(langs::split(settings.dstLang), settings.engineId)
        .value(0, QStringLiteral("zh"));
}

} // namespace

LiveSession::LiveSession(studious::platform::IPlatform *platform, QObject *parent)
    : QObject(parent)
    , m_platform(platform)
{
    // 平台对象由 main.cpp 注入（和 ScreenFlow 共用同一个）；没给就自己造一个，
    // 这样单测 / --ui-shot 里也能独立跑起来。
    if (!m_platform) {
        m_ownedPlatform = studious::platform::createPlatform();
        m_platform = m_ownedPlatform.get();
    }

    m_qt = new QuickTranslate(this);
    connect(m_qt, &QuickTranslate::finished, this, &LiveSession::onTranslationFinished);
    // errorOccurred 是 translate() 同步拒绝时的补充说明，不进 errorText：
    // 忙是正常节拍，把它当错误弹出来会让浮层每两帧闪一次红。

    m_timer = new QTimer(this);
    // 实时抓屏没必要毫秒级准时，Coarse 省一次内核定时器精度的拉扯。
    m_timer->setTimerType(Qt::CoarseTimer);
    connect(m_timer, &QTimer::timeout, this, &LiveSession::runFrame);

    // 一个 watcher 复用一整局：一帧没回来绝不下第二帧，所以不存在并发。
    // 复用而不是每帧 new，是为了析构时能 waitForFinished() 到正在跑的那一个。
    m_watcher = new QFutureWatcher<FrameResult>(this);
    connect(m_watcher, &QFutureWatcherBase::finished, this, &LiveSession::onFrameFinished);
    // engineLabel 开局留空（QML 那边空串就不占位）：第一次译文回来才知道
    // 到底是本地 MT 还是哪个在线引擎，提前猜一个名字反而误导。
}

LiveSession::~LiveSession()
{
    // 先停表，再把在途的工作线程等回来：OCR 里是 ONNX 推理，
    // 放着不管直接析构 QObject 会在退出时崩在别人的线程上。
    m_timer->stop();
    if (m_watcher)
        m_watcher->waitForFinished();
    if (m_qt) {
        // 先断信号再 cancel：cancel 会同步 emit 一个 finished，
        // 那时候本对象已经在拆了，不该再跑一遍「应用译文」。
        disconnect(m_qt, nullptr, this, nullptr);
        m_qt->cancel();
    }
}

// ---------------------------------------------------------------------------
// 属性
// ---------------------------------------------------------------------------

bool LiveSession::ocrAvailable() const
{
    // 和 ScreenFlow 一样懒建：模型没下载时也不该在构造期就去扫盘。
    return const_cast<LiveSession *>(this)->ensureOcr();
}

QString LiveSession::ocrMissingText() const
{
    LiveSession *self = const_cast<LiveSession *>(this);
    if (self->ensureOcr())
        return QString();
    const QStringList missing = m_ocr ? m_ocr->missingFiles() : QStringList();
    if (!missing.isEmpty())
        return tr("缺少 OCR 模型：%1，去资产页下载。").arg(missing.join(QStringLiteral("、")));
    return m_ocr ? m_ocr->lastError() : QString();
}

bool LiveSession::ensureOcr()
{
    if (!m_ocr)
        m_ocr = std::make_unique<studious::ocr::OcrEngine>(ocrModelsDir());
    return m_ocr->isAvailable();
}

// ---------------------------------------------------------------------------
// 会话生命周期
// ---------------------------------------------------------------------------

bool LiveSession::start(const QRect &regionLogical)
{
    if (!m_platform) {
        setErrorText(tr("当前环境没有可用的抓屏后端。"));
        setPhase(QStringLiteral("Error"));
        return false;
    }

    if (regionLogical.width() <= 0 || regionLogical.height() <= 0) {
        // 不切 phase：空选区是调用方还没框完，不是这次会话出错。
        setErrorText(tr("选区为空，先框一块区域。"));
        return false;
    }

    const studious::platform::CapabilityReport caps = m_platform->capabilities();
    if (!caps.capture.available) {
        setErrorText(caps.capture.reason.isEmpty() ? tr("当前环境不能抓屏。") : caps.capture.reason);
        setPhase(QStringLiteral("Error"));
        return false;
    }

    if (!ensureOcr()) {
        const QString reason = ocrMissingText();
        setErrorText(reason.isEmpty() ? tr("OCR 引擎不可用。") : reason);
        setPhase(QStringLiteral("Error"));
        return false;
    }

    // 上一局还在跑（用户直接点了「重新开始」）：先按正常收单处理，
    // 否则上一局的译文会残留在新选区上。
    if (m_phase != QLatin1String("Idle"))
        stop();

    ++m_session;
    m_regionLogical = regionLogical;
    m_pixelScale = resolvePixelScale();
    const QRect physical(int(qRound(regionLogical.x() * m_pixelScale)),
                         int(qRound(regionLogical.y() * m_pixelScale)),
                         int(qRound(regionLogical.width() * m_pixelScale)),
                         int(qRound(regionLogical.height() * m_pixelScale)));
    m_regionPhysical = physical;
    emit regionChanged();
    // 换选区后浮层要跟着挪，否则译文贴在新选区上、窗口还在老地方。
    placeWindows();

    setErrorText(QString());
    setPhase(QStringLiteral("Running"));
    setStatusText(tr("正在识别…"));
    scheduleTimer();
    // 立刻出一帧：等满一个间隔才第一次更新，用户会以为没生效。
    runFrame();
    return true;
}

void LiveSession::stop()
{
    const bool wasActive = m_phase != QLatin1String("Idle");
    stopTimer();
    // 自增会话号：在途的 OCR / 翻译回来时（可能几十毫秒后）会被认成上一局，直接丢。
    ++m_session;
    // 顺手取消在途翻译：不取消的话下一单 start() 的头几百毫秒里
    // QuickTranslate 还在忙，新的一帧发不出去。
    if (m_qt)
        m_qt->cancel();

    const QVariantList originals = m_originals;
    const QVariantList blocks = m_blocks;
    const bool hasResult = (!originals.isEmpty() || !blocks.isEmpty()) && !m_sessionReported;

    resetLive();
    setShowOriginal(false);
    setPhase(QStringLiteral("Idle"));

    if (hasResult)
        emit sessionFinished(originals, blocks);
    if (wasActive)
        emit finished();
}

void LiveSession::pause()
{
    if (m_phase != QLatin1String("Running"))
        return;
    stopTimer();
    // 保留 blocks：暂停的语义是「就停在这一帧」，画面上的译文不许消失。
    setPhase(QStringLiteral("Paused"));
    setStatusText(tr("已暂停 · 已更新 %1 次").arg(m_updateCount));
}

void LiveSession::resume()
{
    if (m_phase != QLatin1String("Paused"))
        return;
    setPhase(QStringLiteral("Running"));
    setStatusText(tr("正在识别…"));
    scheduleTimer();
    runFrame();
}

void LiveSession::scheduleTimer()
{
    if (!m_timer)
        return;
    // 拖选区期间不抓帧（setRegionDragging 会停表，这里别偷偷又开起来）。
    if (m_regionDragging)
        return;
    // 每帧重新读设置（runFrame 里也会读），这里只负责把间隔对齐到最新的值，
    // 用户在设置页把 600 拖到 1200 立刻生效，不用重开一单。
    const AppSettings settings = AppSettings::load();
    const int interval = qBound(400, settings.liveIntervalMs, 1500);
    if (m_timer->interval() != interval)
        m_timer->setInterval(interval);
    if (!m_timer->isActive())
        m_timer->start(interval);
}

void LiveSession::stopTimer()
{
    if (m_timer)
        m_timer->stop();
}

// ---------------------------------------------------------------------------
// 一帧：抓屏 + OCR 在工作线程，结果经 QFutureWatcher 回主线程
// ---------------------------------------------------------------------------

void LiveSession::runFrame()
{
    if (!m_platform || !m_ocr)
        return;
    // 上一帧还没回来就跳过这一拍 —— 这里是「绝不排队」的唯一入口。
    if (m_phase != QLatin1String("Running") || m_frameBusy || m_regionPhysical.isEmpty())
        return;

    // 上一次翻译因为「本地 LLM 还在启动」失败：安静地等几帧再试，
    // 别每 600 ms 去敲一次启动闸门（日志里那种一秒连开好几个 llama-server 就是这么来的）。
    if (m_retrySkipFrames > 0) {
        --m_retrySkipFrames;
        return;
    }

    const AppSettings settings = AppSettings::load();

    studious::ocr::OcrOptions options;
    // 源语言给了具体值就只认它，否则中英混合（界面文字的绝大多数情况）。
    if (!langs::isAuto(settings.srcLang))
        options.languages = { langs::normalizeSource(settings.srcLang) };

    studious::platform::IPlatform *platform = m_platform;
    studious::ocr::OcrEngine *engine = m_ocr.get();
    const studious::platform::CaptureRect region = toCaptureRect(m_regionPhysical);
    const QPoint expectedOrigin = m_regionPhysical.topLeft();

    m_frameBusy = true;
    m_frameSession = m_session;

    // captureRect 内部自己套了 SetThreadDpiAwarenessContext，从工作线程调是安全的；
    // 抓屏 + 推理放一起，省一次跨线程搬图。
    m_watcher->setFuture(QtConcurrent::run([platform, engine, region, options, expectedOrigin] {
        FrameResult out;

        const studious::platform::CaptureResult cap = platform->captureRect(region);
        if (cap.status != studious::platform::PlatformStatus::Ok || cap.image.isNull()) {
            out.error = cap.error.isEmpty() ? QObject::tr("抓屏失败。") : cap.error;
            return out;
        }

        const QVector<studious::platform::OcrWord> raw = engine->recognize(cap.image, options);
        if (raw.isEmpty() && !engine->lastError().isEmpty()) {
            out.error = engine->lastError();
            return out;
        }
        if (raw.isEmpty()) {
            // 认不出字时把「抓到的到底是不是一张空图」记下来：区域选错、抓到自家浮层、
            // 画面全黑，这几种在界面上都只表现为"没有文字"，日志里得能分开。
            const QImage &img = cap.image;
            qint64 sum = 0;
            int samples = 0;
            for (int y = 0; y < img.height(); y += qMax(1, img.height() / 24)) {
                const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); x += qMax(1, img.width() / 24)) {
                    sum += qGray(row[x]);
                    ++samples;
                }
            }
            qInfo("[live] empty frame: %dx%d mean=%.1f",
                  img.width(),
                  img.height(),
                  samples > 0 ? double(sum) / samples : 0.0);
        }

        // 抓到的矩形可能被平台按桌面边界夹过（跨屏、出界），所以坐标要用**实际**
        // 抓到的那个矩形来还原，不能直接假设等于请求的矩形。
        const QPoint offset = QRect(cap.rect.x, cap.rect.y, cap.rect.width, cap.rect.height).topLeft()
                              - expectedOrigin;

        QVector<studious::platform::OcrWord> words;
        words.reserve(raw.size());
        for (const studious::platform::OcrWord &w : raw) {
            if (w.confidence < kMinConfidence)
                continue;
            if (w.text.trimmed().isEmpty())
                continue;
            studious::platform::OcrWord moved = w;
            moved.box.translate(-offset);
            words.append(moved);
        }

        out.lines = groupWordsIntoLines(words);
        out.ok = true;
        return out;
    }));
}

void LiveSession::onFrameFinished()
{
    const FrameResult result = m_watcher->result();
    m_frameBusy = false;
    // 上一局的帧（stop 之后才回来，此时新的一局可能已经 Running 了）→ 作废。
    if (m_frameSession != m_session)
        return;
    if (m_phase != QLatin1String("Running"))
        return;   // 已停止 / 已暂停：这一帧的结果作废

    if (!result.ok) {
        // 抓屏失败基本是「窗口关了 / 屏幕锁了」，直接收单，别在这儿每秒重试。
        setPhase(QStringLiteral("Error"));
        setErrorText(result.error.isEmpty() ? tr("抓屏失败。") : result.error);
        stopTimer();
        m_sessionReported = true;
        emit sessionFinished(m_originals, m_blocks);
        return;
    }

    acceptOcrLines(result.lines);
}

void LiveSession::acceptOcrLines(const QVector<OcrLine> &lines)
{
    QStringList paragraphList;
    for (const OcrLine &line : lines) {
        if (!line.text.isEmpty())
            paragraphList.append(line.text);
    }

    if (paragraphList.isEmpty()) {
        ++m_emptyFrames;
        // 以前连续 3 帧就收单，结果「画面正好没有字」（过场、黑屏、对话框关掉）也会被判死。
        // 现在只记状态、不停止：用户开着就一直接着看，直到自己关。
        setStatusText(tr("这一帧没有文字（已连续 %1 帧）").arg(m_emptyFrames));
        return;
    }
    m_emptyFrames = 0;

    const AppSettings settings = AppSettings::load();
    // 主界面换了引擎：跟随模式（没在控制条里单独选过）要立刻用新引擎重翻一遍。
    // 不置这一下的话，「画面没变」那道闸会把这一帧整个跳过，用户看到的就是「不跟随」。
    if (m_engineId.isEmpty() && settings.engineId != m_lastSettingsEngineId) {
        m_lastSettingsEngineId = settings.engineId;
        m_lastText.clear();
    }
    // 指纹就是原文本身：一帧几百字，字符串比较比算哈希还快，也不会撞哈希漏翻。
    const QString fingerprint = paragraphList.join(QLatin1Char('\n'));

    // 画面没变就不重翻。pending != last 说明上一趟还在飞（或上次失败了）：
    // 那种情况要重试，不能因为「和上一次相同」把失败的那一帧永久钉住。
    if (settings.liveOnlyOnChange && fingerprint == m_lastText && fingerprint != m_pendingText) {
        setStatusText(tr("画面未变 · 已更新 %1 次").arg(m_updateCount));
        return;
    }

    // 上一趟译文还在飞：这一帧**整帧丢掉**，连框都不更新。
    // 只换框不换字会让上一趟的译文贴到这一趟的坐标上（画面在动时特别明显），
    // 比晚一拍显示难看得多；反正下一拍还有机会。
    if (m_qt->isBusy()) {
        setStatusText(tr("引擎忙，等下一帧 · 已更新 %1 次").arg(m_updateCount));
        return;
    }

    m_lines = lines;

    // 译文块先按原文行铺出来（text 空、untranslated=true），QML 只显示能翻的部分；
    // 翻译回来再整块替换。这样「OCR 到了但译文还没到」的那一两个节拍里，
    // 屏幕上是干净的原文，而不是上一帧错位的旧贴纸。
    QVariantList pendingBlocks;
    m_originals.clear();
    for (const OcrLine &line : lines) {
        if (line.text.isEmpty())
            continue;
        pendingBlocks.append(blockMap(line.box, line.text, QString(), line.confidence));
        m_originals.append(line.text);
    }
    setBlocks(pendingBlocks);

    const QString dst = firstDstLang(settings);
    const QString src = langs::normalizeSource(settings.srcLang);
    // QuickTranslate 按**空行**切段，所以行与行之间要用两个换行才能保住 1:1；
    // 单换行会被它并成一段，回来的译文就没有块可贴了。
    const QString text = paragraphList.join(QStringLiteral("\n\n"));

    if (!m_qt->translate(text, src, { dst })) {
        // 同步拒绝（引擎起不来 / 没有目标语言）：保留这一帧的原文框，
        // m_lastText 不写，下一拍会重试。
        qWarning("[live] LiveSession: translate rejected (src=%s, dst=%s, err=%s)",
                 qUtf8Printable(src), qUtf8Printable(dst), qUtf8Printable(m_qt->lastError()));
        setStatusText(tr("翻译没发出去：%1").arg(m_qt->lastError()));
        return;
    }
    m_pendingText = fingerprint;
    m_pendingLang = dst;
    m_pendingSession = m_session;
    setStatusText(tr("正在翻译 %1 行…").arg(paragraphList.size()));
}

void LiveSession::onTranslationFinished(const QuickTranslateResult &result)
{
    if (qEnvironmentVariableIsSet("STUDIOUS_LLM_TRACE")) {
        qInfo("[live] translation finished ok=%d err=%s sameSession=%d phase=%s",
              int(result.ok), qUtf8Printable(result.error),
              int(m_pendingSession == m_session), qUtf8Printable(m_phase));
    }
    // 上一局（stop 后又 start 了一单）的结果：直接丢，别贴到新选区上。
    if (m_pendingSession != m_session)
        return;
    if (m_phase != QLatin1String("Running") && m_phase != QLatin1String("Paused"))
        return;
    if (!result.ok) {
        if (m_engineSwitching) {
            // 我们自己取消的：不报错、不弹红字，只把 pending 清掉让下一帧用新引擎重翻。
            m_engineSwitching = false;
            m_pendingText.clear();
            return;
        }
        // 引擎暂时失败（网络抖、本地进程崩）不是「这一单完了」：保留最后一帧译文，
        // 下一拍会重试（m_lastText 没被写脏）。
        m_pendingText.clear();
        // 「本地 LLM 正在启动」属于可重试：退避几帧，别一帧一次地敲启动。
        if (m_qt != nullptr && m_qt->lastFailureRetryable()) {
            m_retrySkipFrames = 3;
            setStatusText(tr("本地 LLM 正在启动，稍后自动重试…"));
            return;
        }
        setStatusText(tr("翻译失败：%1").arg(result.error));
        return;
    }
    m_engineSwitching = false;
    applyTranslation(result);
}

void LiveSession::applyTranslation(const QuickTranslateResult &result)
{
    QStringList texts = result.byLang.value(m_pendingLang);
    if (texts.isEmpty() && !result.byLang.isEmpty())
        texts = result.byLang.constBegin().value();   // 期间改了目标语言：拿任何一份也好过空屏

    QVariantList blocks;
    blocks.reserve(m_lines.size());
    int index = 0;
    for (const OcrLine &line : std::as_const(m_lines)) {
        if (line.text.isEmpty())
            continue;
        // 段落与块 1:1：QuickTranslate 保证 byLang 的列表跟输入段落等长（缺的空串），
        // 所以按下标取就是这一行的译文。缺的 → text 空 → untranslated，露出原文。
        QString text = index < texts.size() ? texts.at(index).trimmed() : QString();
        // 模型把原文原样退回来（菜单名、型号、已经是目标语言的短句）时，
        // 别把原文当译文贴上去 —— 那样用户看不出这句到底翻过没有。
        if (!text.isEmpty() && text == line.text.trimmed())
            text.clear();
        ++index;
        blocks.append(blockMap(line.box, line.text, text, line.confidence));
    }
    setBlocks(blocks);

    m_lastText = m_pendingText;
    m_pendingText.clear();
    ++m_updateCount;
    emit updateCountChanged();

    if (!result.engineLabel.isEmpty() && result.engineLabel != m_engineLabel) {
        m_engineLabel = result.engineLabel;
        emit engineLabelChanged();
    }

    const bool slow = result.elapsedMs > kSlowThresholdMs;
    setSlowHint(slow ? tr("当前引擎每次约 %1 秒，实时效果受限")
                           .arg(result.elapsedMs / 1000.0, 0, 'f', 1)
                     : QString());

    setErrorText(QString());
    setStatusText(tr("已更新 %1 次 · %2 行 · %3 ms")
                      .arg(m_updateCount)
                      .arg(index)
                      .arg(result.elapsedMs));
}

// ---------------------------------------------------------------------------
// 用户动作
// ---------------------------------------------------------------------------

void LiveSession::toggleOriginal()
{
    setShowOriginal(!m_showOriginal);
}

bool LiveSession::copyResult()
{
    QStringList rows;
    for (const QVariant &block : std::as_const(m_blocks)) {
        const QVariantMap m = block.toMap();
        // 没译文就用原文兜底：复制出来的东西总得是屏幕上的那句话。
        QString text = m.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            text = m.value(QStringLiteral("original")).toString();
        if (!text.isEmpty())
            rows.append(text);
    }
    if (rows.isEmpty())
        return false;
    if (QClipboard *clip = QGuiApplication::clipboard())
        clip->setText(rows.join(QLatin1Char('\n')));
    setStatusText(tr("已复制 %1 行").arg(rows.size()));
    return true;
}

void LiveSession::retranslate()
{
    // 只丢掉指纹：下一帧即使画面没变也会重新走一遍翻译。
    // 不动 m_pendingText —— 在途那一趟回来时还要靠它写回 m_lastText。
    m_lastText.clear();
    if (m_phase == QLatin1String("Running"))
        runFrame();
}

// ---------------------------------------------------------------------------
// 窗口挂载
// ---------------------------------------------------------------------------

bool LiveSession::attachPlateWindow(QObject *window)
{
    auto *w = qobject_cast<QWindow *>(window);
    if (!w) {
        setErrorText(tr("译文浮层窗口无效。"));
        return false;
    }
    if (!m_platform) {
        setErrorText(tr("当前环境没有可用的抓屏后端。"));
        return false;
    }
    m_plateWindow = w;

    QString error;
    // 穿透是默认硬要求：浮层压在用户正在读的窗口上，误点一下就是打断阅读。
    // 但「解锁选区」时要反过来 —— 浮层得能接鼠标，八向手柄才拖得动。
    // 排除截图始终是硬要求：否则下一轮 OCR 会把上一轮的译文当原文再翻一遍，
    // 译文越翻越离谱且永远停不下来。
    if (!m_platform->setClickThrough(w, m_regionLocked, &error)) {
        setErrorText(error.isEmpty() ? tr("译文浮层不能点击穿透。") : error);
        return false;
    }
    if (!m_platform->setExcludeFromCapture(w, true, &error)) {
        setErrorText(error.isEmpty() ? tr("译文浮层不能排除在截图之外。") : error);
        return false;
    }
    return placeWindows();
}

bool LiveSession::attachControlWindow(QObject *window)
{
    auto *w = qobject_cast<QWindow *>(window);
    if (!w) {
        setErrorText(tr("控制条窗口无效。"));
        return false;
    }
    if (!m_platform) {
        setErrorText(tr("当前环境没有可用的抓屏后端。"));
        return false;
    }
    m_controlWindow = w;

    QString error;
    // 控制条**要能被截图拍到**：用户要用截图工具把它拍下来（反馈问题、做说明文档），
    // 排除掉就成了"看得见、拍不到"的幽灵窗口。它摆在选区外面（placeWindows 负责），
    // 所以也不会被自己的 OCR 读进去。
    // 唯一必须关掉的是点击穿透 —— WinPlatform 的排除截图接口会顺手打开它，这里显式关回。
    if (!m_platform->setClickThrough(w, false, &error)) {
        setErrorText(error.isEmpty() ? tr("控制条不能接收点击。") : error);
        return false;
    }
    return true;
}

void LiveSession::setEngineId(const QString &uiEngineId)
{
    // 认不出来的写法当「自动」，别把错字写进状态里。
    const QString id = uiEngineId.trimmed().toLower();
    QString next;
    if (id == QLatin1String("web") || id == QLatin1String("llm-cloud")
        || id == QLatin1String("llm-local") || id == QLatin1String("local-mt")) {
        next = id;
    } else if (id == QLatin1String("llm")) {
        next = QStringLiteral("llm-cloud");
    }
    if (next == m_engineId)
        return;
    m_engineId = next;
    // 切引擎先把在飞的那一单掐掉：不然旧引擎的译文会晚一步回来，
    // 覆盖掉新引擎刚写上去的结果，界面上看就是"切了没反应"。
    if (m_qt) {
        m_engineSwitching = true;
        m_qt->cancel();
    }
    applyEngineOverride();
    emit engineIdChanged();
    // 换了引擎就别等画面变化：下一帧用新引擎重翻一遍。
    retranslate();
}

void LiveSession::applyEngineOverride()
{
    if (!m_qt)
        return;
    if (m_engineId.isEmpty()) {
        m_qt->clearEngineOverride();
    } else {
        m_qt->setEngineOverride(m_engineId);
    }
}

void LiveSession::setRegionLocked(bool locked)
{
    if (locked == m_regionLocked)
        return;
    m_regionLocked = locked;
    // 解锁 → 浮层要接鼠标（穿透 = false）；锁定 → 恢复穿透（穿透 = true），
    // 别打断用户点下面的窗口。注意语义：穿透 == 「锁定」，别写反。
    // 这里改的是 Win32 扩展样式，QML 随后改 visible/绘制时 Qt 会再写一次窗口样式，
    // 有可能把 WS_EX_TRANSPARENT 抹掉 —— 所以 QML 那边还会在状态变化后叫 reapplyPlateFlags()。
    if (m_plateWindow && m_platform) {
        QString error;
        if (!m_platform->setClickThrough(m_plateWindow.data(), locked, &error) && !error.isEmpty())
            qWarning("[live] setClickThrough(%d) failed: %s", int(locked), qUtf8Printable(error));
    }
    emit regionLockedChanged();
}

void LiveSession::reapplyPlateFlags()
{
    if (!m_plateWindow || !m_platform)
        return;
    QString error;
    if (!m_platform->setClickThrough(m_plateWindow.data(), m_regionLocked, &error)
        && !error.isEmpty()) {
        qWarning("[live] reapply setClickThrough(%d) failed: %s", int(m_regionLocked),
                 qUtf8Printable(error));
    }
    if (!m_platform->setExcludeFromCapture(m_plateWindow.data(), true, &error) && !error.isEmpty())
        qWarning("[live] reapply setExcludeFromCapture failed: %s", qUtf8Printable(error));
}

void LiveSession::setRegionDragging(bool dragging)
{
    if (dragging == m_regionDragging)
        return;
    m_regionDragging = dragging;
    if (dragging) {
        // 拖的时候不抓帧：手柄就在选区边上，抓进去会把紫点当文字识别。
        stopTimer();
        return;
    }
    // 松手：重新对齐到最新间隔，并立刻补一帧（不用等满一个周期）。
    if (m_phase == QLatin1String("Running")) {
        scheduleTimer();
        runFrame();
    }
}

void LiveSession::setRegion(const QRect &regionLogical)
{
    if (regionLogical.width() < 8 || regionLogical.height() < 8)
        return;
    if (regionLogical == m_regionLogical)
        return;
    m_regionLogical = regionLogical;
    m_pixelScale = resolvePixelScale();
    m_regionPhysical = QRect(int(qRound(regionLogical.x() * m_pixelScale)),
                             int(qRound(regionLogical.y() * m_pixelScale)),
                             int(qRound(regionLogical.width() * m_pixelScale)),
                             int(qRound(regionLogical.height() * m_pixelScale)));
    emit regionChanged();
    placeWindows();
    // 选区变了，上一帧的指纹作废：下一拍一定要重新抓一次。
    m_lastText.clear();
    m_emptyFrames = 0;
    if (m_phase == QLatin1String("Error")) {
        setErrorText(QString());
        setPhase(QLatin1String("Running"));
        scheduleTimer();
    }
}

bool LiveSession::placeWindows()
{
    if (!m_platform)
        return false;
    if (!m_plateWindow)
        return true;   // 浮层还没交句柄：等它 Component.onCompleted 时再叫一次
    if (m_regionPhysical.isEmpty())
        return true;   // 还没开会话：等 start() 之后再摆

    QString error;
    if (!m_platform->placeOverlay(m_plateWindow.data(), m_regionPhysical, &error)) {
        setErrorText(error.isEmpty() ? tr("译文浮层摆不到选区上。") : error);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 杂项
// ---------------------------------------------------------------------------

qreal LiveSession::resolvePixelScale() const
{
    // 选区的「逻辑 → 物理」只能用屏幕的缩放比换算，AppSettings 里没有这个东西。
    // 首选虚拟桌面的比值（多屏混合 DPI 下它是个折中值，跟 ScreenFlow 同一个口径），
    // 拿不到再退到主屏。
    qreal scale = 0.0;
    if (m_platform) {
        const QRect physical = m_platform->virtualDesktopRect();
        const QSize logical = QGuiApplication::primaryScreen()
                                  ? QGuiApplication::primaryScreen()->virtualGeometry().size()
                                  : QSize();
        if (!physical.isEmpty() && logical.width() > 0)
            scale = qreal(physical.width()) / logical.width();
    }
    if (scale <= 0.0 && QGuiApplication::primaryScreen())
        scale = QGuiApplication::primaryScreen()->devicePixelRatio();
    return scale > 0.0 ? scale : 1.0;
}

void LiveSession::resetLive()
{
    m_lines.clear();
    m_originals.clear();
    m_lastText.clear();
    m_pendingText.clear();
    m_pendingSession = 0;
    m_emptyFrames = 0;
    m_sessionReported = false;
    setBlocks(QVariantList());
    setStatusText(QString());
    setErrorText(QString());
    setSlowHint(QString());
}

void LiveSession::setPhase(const QString &phase)
{
    if (phase == m_phase)
        return;
    m_phase = phase;
    emit phaseChanged();
}

void LiveSession::setStatusText(const QString &text)
{
    if (text == m_statusText)
        return;
    m_statusText = text;
    emit statusTextChanged();
}

void LiveSession::setErrorText(const QString &text)
{
    if (text == m_errorText)
        return;
    m_errorText = text;
    emit errorTextChanged();
}

void LiveSession::setBlocks(const QVariantList &blocks)
{
    m_blocks = blocks;
    emit blocksChanged();
}

void LiveSession::setSlowHint(const QString &hint)
{
    if (hint == m_slowHint)
        return;
    m_slowHint = hint;
    emit slowHintChanged();
}

void LiveSession::setShowOriginal(bool on)
{
    if (on == m_showOriginal)
        return;
    m_showOriginal = on;
    emit showOriginalChanged();
}

} // namespace studious::ui
