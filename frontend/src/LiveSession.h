// 实时翻译（设计稿 C 页）的引擎：定时抓选区 → OCR → 翻 → 把每行贴回原位。
//
// 和 ScreenFlow 的分工：ScreenFlow 是「用户划一次、翻一次」，那条链上每一步都
// 可以慢慢来（框选、等 OCR、等引擎）；这里的每一步都在 600 ms 的节拍里跑，
// 慢一帧就少一帧，所以宁可丢帧也**绝不排队** —— 排队会让屏幕上的译文一直
// 追着画面跑，越追越旧，看起来像卡死。
//
// 本类不碰界面：只算坐标、只出数据，浮层/控制条由 QML 画。窗口句柄由 QML 通过
// attachPlateWindow() / attachControlWindow() 交进来，之后再按选区摆位或打标记。
#pragma once

#include "OcrLines.h"

#include "platform/IPlatform.h"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWindow>

#include <memory>

class QTimer;

namespace studious::ocr {
class OcrEngine; // 后端推理引擎，只在工作线程里用（非线程安全）
}

namespace studious::ui {

class QuickTranslate;
struct QuickTranslateResult;

class LiveSession final : public QObject
{
    Q_OBJECT
    // Idle | Running | Paused | Error。QML 直接比字符串（LiveControlBar 就是这么用的）。
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    // 每块 {text, original, x, y, w, h, confidence, untranslated}，
    // 坐标是**相对选区左上角**的物理像素 —— 浮层窗口就摆在选区上，不用再加桌面偏移。
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY blocksChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QString engineLabel READ engineLabel NOTIFY engineLabelChanged)
    // 空串 = 不显示。慢的时候给一句人话，而不是让用户自己怀疑「是不是没在工作」。
    Q_PROPERTY(QString slowHint READ slowHint NOTIFY slowHintChanged)
    Q_PROPERTY(int updateCount READ updateCount NOTIFY updateCountChanged)
    // 选区两种口径都留着：regionLogical 给控制条显示「778 × 243」——
    // 用户量的是逻辑像素，报物理像素他会以为框错了；regionPhysical 给浮层定位。
    Q_PROPERTY(QRect regionLogical READ regionLogical NOTIFY regionChanged)
    Q_PROPERTY(QRect regionPhysical READ regionPhysical NOTIFY regionChanged)
    Q_PROPERTY(qreal pixelScale READ pixelScale NOTIFY regionChanged)
    Q_PROPERTY(bool showOriginal READ showOriginal NOTIFY showOriginalChanged)
    // 空串 = 跟随全局设置（设置页里选的引擎）；否则是界面 id（web/llm-cloud/llm-local/local-mt）。
    Q_PROPERTY(QString engineId READ engineId WRITE setEngineId NOTIFY engineIdChanged)
    // 选区锁定：锁定 = 只读（点击穿透），解锁后浮层能接鼠标，显示八个手柄可拖可缩放。
    Q_PROPERTY(bool regionLocked READ regionLocked WRITE setRegionLocked NOTIFY regionLockedChanged)
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable NOTIFY ocrStatusChanged)
    Q_PROPERTY(QString ocrMissingText READ ocrMissingText NOTIFY ocrStatusChanged)

public:
    explicit LiveSession(studious::platform::IPlatform *platform = nullptr, QObject *parent = nullptr);
    ~LiveSession() override;

    QString phase() const { return m_phase; }
    QVariantList blocks() const { return m_blocks; }
    QString statusText() const { return m_statusText; }
    QString errorText() const { return m_errorText; }
    QString engineLabel() const { return m_engineLabel; }
    QString slowHint() const { return m_slowHint; }
    int updateCount() const { return m_updateCount; }
    QRect regionLogical() const { return m_regionLogical; }
    QRect regionPhysical() const { return m_regionPhysical; }
    qreal pixelScale() const { return m_pixelScale; }
    bool showOriginal() const { return m_showOriginal; }
    QString engineId() const { return m_engineId; }
    void setEngineId(const QString &uiEngineId);
    bool regionLocked() const { return m_regionLocked; }
    void setRegionLocked(bool locked);
    // 拖动手柄/整体移动之后由 QML 交回新选区（逻辑像素）。只挪窗口、不重开会话。
    Q_INVOKABLE void setRegion(const QRect &regionLogical);
    // 用户正在拖手柄/拖整块选区时把抓帧停掉：手柄和边框本身就压在选区边缘，
    // 不暂停就会把「正在移动的紫点」当成原文识别一轮，松手后又得等一帧才恢复干净。
    Q_INVOKABLE void setRegionDragging(bool dragging);
    bool ocrAvailable() const;
    QString ocrMissingText() const;

    // 选区是逻辑像素、相对虚拟桌面左上角（和框选页交给 ScreenFlow 的口径一致）。
    Q_INVOKABLE bool start(const QRect &regionLogical);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void toggleOriginal();
    Q_INVOKABLE bool copyResult();
    // 丢掉 lastText：下一帧即使画面没变也重翻。给「换引擎/换语言后重来一单」用。
    Q_INVOKABLE void retranslate();
    // 立刻采一帧（控制条上的「刷新」）：不等下一个定时拍，跳过"本地引擎还在启动"的退避。
    // 正在抓帧时点了就忽略——这一拍本来就会出结果，排队反而会连抓两次。
    Q_INVOKABLE void refresh();

    // 译文浮层：排除截图 + 点击穿透 + 摆到选区上。
    Q_INVOKABLE bool attachPlateWindow(QObject *window);
    // 控制条：**只**排除截图。它要能点、能拖、能收 Esc，穿透了就等于没有。
    Q_INVOKABLE bool attachControlWindow(QObject *window);
    // 控制条是否压在实时选区上：压在选区里就必须排除出抓屏，否则它自己的
    // 「暂停 / 刷新 / 设置」会被下一帧 OCR 当成原文再翻一遍。
    // 平时（在选区外面）保持可被拍到 —— 用户要拿截图工具把它拍下来反馈问题。
    Q_INVOKABLE void setControlBarOverlapping(bool overlaps);
    // 重新给译文浮层打一遍「穿透 + 排除截图」。
    // QML 侧改 visible / 主题会让 Qt 自己回写一次窗口样式，把 Win32 的 WS_EX_TRANSPARENT
    // 抹掉（QML 的 flags 和 SetWindowLong 是两拨人在写同一份数据），所以由 QML 在
    // 显隐、锁定状态变化后 QT_DEFERRED 地叫一次，把我们要的那几位重新盖回去。
    Q_INVOKABLE void reapplyPlateFlags();

signals:
    void phaseChanged();
    void blocksChanged();
    void statusTextChanged();
    void errorTextChanged();
    void engineLabelChanged();
    void slowHintChanged();
    void updateCountChanged();
    void regionChanged();
    void showOriginalChanged();
    void engineIdChanged();
    void regionLockedChanged();
    void ocrStatusChanged();
    void finished();
    // 收单时给记录层一条：原文行 + 译文块。只有正常停止和出错才发，
    // 用户中途 pause() 不算结束。
    void sessionFinished(const QVariantList &originals, const QVariantList &blocks);

private:
    // 一帧的全部结果。必须是可拷贝类型：QFutureWatcher<T> 的 result() 要按值取回来。
    struct FrameResult {
        bool ok = false;
        QString error;            // 抓屏/OCR 的失败原因（ok=false 时才有意义）
        QVector<OcrLine> lines;   // box 已还原成「相对选区左上角」的物理像素
    };

    void runFrame();
    void onFrameFinished();
    void onTranslationFinished(const QuickTranslateResult &result);
    void applyTranslation(const QuickTranslateResult &result);
    void scheduleTimer();
    void stopTimer();
    void resetLive();
    void acceptOcrLines(const QVector<OcrLine> &lines);

    void setPhase(const QString &phase);
    void setStatusText(const QString &text);
    void setErrorText(const QString &text);
    void setBlocks(const QVariantList &blocks);
    void setSlowHint(const QString &hint);
    void setShowOriginal(bool on);

    bool ensureOcr();
    // 把译文浮层摆到选区上。返回 false = 摆位失败（错误已经写进 errorText）。
    bool placeWindows();
    // 把控制条抬到译文浮层之上：控制条落进选区时会被那层原文底色盖住。
    void raiseControlWindow();
    // 逻辑选区 → 物理选区。直接乘 AppSettings 里没有的缩放因子，所以从屏幕取。
    qreal resolvePixelScale() const;

    studious::platform::IPlatform *m_platform = nullptr;
    std::unique_ptr<studious::platform::IPlatform> m_ownedPlatform;
    std::unique_ptr<studious::ocr::OcrEngine> m_ocr;
    QuickTranslate *m_qt = nullptr;

    QFutureWatcher<FrameResult> *m_watcher = nullptr;
    QTimer *m_timer = nullptr;

    QString m_phase = QStringLiteral("Idle");
    QString m_statusText;
    QString m_errorText;
    QString m_engineLabel;
    QString m_slowHint;
    int m_updateCount = 0;

    QRect m_regionLogical;
    QRect m_regionPhysical;
    qreal m_pixelScale = 1.0;

    QVariantList m_blocks;
    // 这一局的记录已经交出去了（正常 stop 或出错时各一次）。控制条在 Error 之后
    // 还会被点一次停止，那里不许再发一遍 sessionFinished，否则记录页出现两条一样的。
    bool m_sessionReported = false;
    // 当前帧的行：等译文回来要 1:1 贴块，所以整份留着（不留在 Future 里，
    // 因为翻译是另一趟异步，回来时 Future 早没了）。
    QVector<OcrLine> m_lines;
    QVariantList m_originals;
    bool m_showOriginal = false;
    // 空 = 跟设置走。真实下发给 QuickTranslate 前会换成界面 id 或清掉覆盖。
    QString m_engineId;
    // 开局解锁：让用户先看见八个手柄、能把选区拖准；点控制条上的「锁定」才交回穿透。
    bool m_regionLocked = false;
    bool m_regionDragging = false;
    void applyEngineOverride();
    bool m_frameBusy = false;
    // 切引擎时会主动 cancel 在飞的一单：那次失败是我们自己造成的，
    // 不能当成真的失败去刷"翻译失败"，否则切完引擎画面会闪一下红字。
    bool m_engineSwitching = false;
    // 在途那一帧属于哪一局：stop() 之后旧的 OCR 才回来时，靠它拦住 ——
    // 光看 phase 不够，因为紧接着 start() 又会让 phase 回到 Running。
    quint64 m_frameSession = 0;
    int m_emptyFrames = 0;
    // 本地 LLM 正在启动时的退避：接下来这几帧直接跳过（不抓屏、不发翻译请求）。
    // 600 ms 一帧的节奏下，一直硬试只会不断把「它还没起来」再问一遍。
    int m_retrySkipFrames = 0;
    // 用户按了「刷新」：这一帧即使和上一帧文字一模一样也要重翻一次。
    // 只作用于紧接着的那一帧（acceptOcrLines 里用掉就清），不会把"画面未变就不重翻"
    // 这条规则永久废掉。
    bool m_forceNextFrame = false;
    // 每 start/stop 自增：OCR 与翻译都可能跨局回来，靠它丢掉上一局的结果。
    quint64 m_session = 0;
    quint64 m_pendingSession = 0;
    // 上一次**翻译成功**的 OCR 全文（liveOnlyOnChange 拿它比），
    // 以及在途那一趟的全文 —— 只有翻译成功才把 pending 提为 last，
    // 否则引擎暂时失败一次会让同一屏内容永远不再翻（last 先被写脏了）。
    QString m_lastText;
    QString m_pendingText;
    QString m_pendingLang;
    // 上一帧读到的 settings/engine/id：跟随主界面时靠它发现「主界面换引擎了」。
    QString m_lastSettingsEngineId;

    QPointer<QWindow> m_plateWindow;
    QPointer<QWindow> m_controlWindow;
    // 控制条当前有没有被排除出抓屏（见 setControlBarOverlapping）。
    bool m_controlBarExcluded = false;
};

} // namespace studious::ui
