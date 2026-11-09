// 截图翻译的流程控制（M1+M2+M4 的 C++ 那一半）。
//
// 为什么单独一个 QObject、而不是塞进 AppBackend：
//   1) 截屏/OCR/浮层是三条各自会失败的异步链，混进 2000 行的 AppBackend 会让它更难读；
//   2) 小窗翻译（QuickTranslate）与实时翻译要复用同一套「抓图 → OCR → 翻译 → 出块」逻辑，
//      抽出来才不用复制三份；
//   3) 它只依赖平台层（截屏/热键/浮层）与后端（OCR/翻译），不碰界面控件。
//
// 状态机（与 docs/TempUI/flows.html 的 A1–A10 对齐）：
//   Idle → Capturing（抓整屏） → Selecting（QML 遮罩在等框选）
//        → Recognizing（OCR，工作线程） → Translating（引擎在跑）
//        → Showing（浮层出块） ⇄ ShowingOriginal
//   任一步失败 → Error，`errorText` 说人话，界面可「重新翻译」。
#pragma once

#include "platform/IPlatform.h"
#include "platform/Types.h"

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>

class QScreen;
// OCR 工作线程的句柄：这里只声明类型，析构时用它把在飞的识别等回来（见 ~ScreenFlow）。
// 用非模板基类而不是 QFutureWatcher<T>：头文件里不必带 QtConcurrent 的模板定义，
// waitForFinished() 本来就在基类上。
class QFutureWatcherBase;

namespace studious::ocr {
class OcrEngine;
}

namespace studious::ui {

class QuickTranslate;

class ScreenFlow final : public QObject {
    Q_OBJECT
    // Idle / Capturing / Selecting / Recognizing / Translating / Showing / ShowingOriginal / Error
    Q_PROPERTY(QString phase READ phase NOTIFY phaseChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    // 日志行（OCR 出了几块、引擎是什么、耗时多少），给工具栏第二行用。
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // 选区（逻辑像素，给 QML 画遮罩用）与抓到的整屏（物理像素）
    Q_PROPERTY(QRect selectionRect READ selectionRect NOTIFY selectionChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QSize screenSize READ screenSize NOTIFY screenChanged)
    // 整屏物理像素 / 虚拟桌面逻辑像素。QML 用它把 OCR 的物理坐标折回逻辑坐标。
    Q_PROPERTY(qreal pixelScale READ pixelScale NOTIFY screenChanged)
    // 虚拟桌面的逻辑矩形：遮罩窗口要铺满它（QML 的 Screen.virtualWidth 在这版 Qt 里取不到）。
    Q_PROPERTY(QRect desktopRect READ desktopRect NOTIFY screenChanged)
    // 译文块：{ text, original, x, y, w, h, confidence, untranslated }
    // 坐标是**相对选区左上角**的物理像素，QML 自己乘 DPR。
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY blocksChanged)
    Q_PROPERTY(bool showOriginal READ showOriginal NOTIFY showOriginalChanged)
    Q_PROPERTY(QString engineLabel READ engineLabel NOTIFY statusTextChanged)
    // OCR 模型是否就绪（不在时界面直接说「去资产页下载」，而不是让用户等一条报错）
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable NOTIFY ocrStatusChanged)
    Q_PROPERTY(QString ocrMissingText READ ocrMissingText NOTIFY ocrStatusChanged)

public:
    // platform 由调用方注入（进程里只需要一份热键消息窗口与一份抓屏实现）；
    // 传 nullptr 时自己建一份并持有，方便测试与 --ui-shot 这类短命进程。
    explicit ScreenFlow(studious::platform::IPlatform *platform = nullptr, QObject *parent = nullptr);
    ~ScreenFlow() override;

    QString phase() const { return m_phase; }
    QString errorText() const { return m_errorText; }
    QString statusText() const { return m_statusText; }
    QRect selectionRect() const { return m_selectionLogical; }
    bool hasSelection() const { return m_selectionLogical.isValid(); }
    QSize screenSize() const { return m_shot.size(); }
    qreal pixelScale() const { return m_pixelScale; }
    QRect desktopRect() const;
    QVariantList blocks() const { return m_blocks; }
    bool showOriginal() const { return m_showOriginal; }
    QString engineLabel() const { return m_engineLabel; }
    bool ocrAvailable() const;
    QString ocrMissingText() const;

    // 1) 热键 / 入口卡片调它：抓整屏 → phase=Selecting，QML 收到 screenCaptured 弹出遮罩。
    Q_INVOKABLE bool begin();
    // 2) 遮罩松手后调它（region 是**逻辑像素**，相对整屏左上角）。
    Q_INVOKABLE void confirmSelection(const QRect &region);
    // 3) 取消（Esc / 工具栏「取消」）：回 Idle，丢掉这一次的所有中间状态。
    Q_INVOKABLE void cancel();

    // 打断当前这一单（识别中 / 翻译中），但**保住选区与已有贴纸**：
    // 用户可能只是想换个引擎再翻一次，不愿重新框选。
    // 与 cancel() 的区别：cancel() 是「结束整个截屏流程」，这个是「这一单不做了」。
    Q_INVOKABLE void cancelTranslation();
    // 4) 浮层工具栏的三个动作。
    Q_INVOKABLE void toggleOriginal();
    Q_INVOKABLE void retranslate();
    Q_INVOKABLE bool copyResult();
    Q_INVOKABLE bool saveToRecords(const QString &source = QStringLiteral("capture"));

    // 给 QML 画布用的原图（整屏，物理像素）。QML 用 Image { source: "" } 画不了 QImage，
    // 所以由 C++ 提供 QQuickImageProvider 之外的简单办法：见 imageSourceUrl()。
    Q_INVOKABLE QString imageSourceUrl() const;

    // 放大镜用：取鼠标处那个像素的颜色（物理像素坐标）。
    // QML 的 Image 读不到像素，色值只能由这边给；越界返回 ok=false。
    // 返回 {ok, x, y, hex, r, g, b}，hex 可直接当颜色字符串用。
    Q_INVOKABLE QVariantMap pixelInfoAt(int x, int y) const;

    Q_INVOKABLE QRect physicalSelection() const;   // 物理像素，浮层定位用
    Q_INVOKABLE QRect selectionScreenRect() const; // 选区在虚拟桌面里的物理坐标

    // 开发走查用：对一张本地图片跑一次 OCR，把每个框的文本与置信度打到 stderr。
    // 返回 "N word(s)" / "unavailable: …" / "image"，只用于命令行验证模型装没装对。
    Q_INVOKABLE QString ocrTest(const QString &imagePath);

    // 仅截图走查用：塞三段假译文，看 A4/A5 的译文块排版对不对。
    // 不会出现在任何正式路径里（只有 --ui-shot 的调试钩子会调它）。
    Q_INVOKABLE void debugSampleBlocks();

signals:
    void phaseChanged();
    void errorTextChanged();
    void statusTextChanged();
    void selectionChanged();
    void screenChanged();
    void blocksChanged();
    void showOriginalChanged();
    void ocrStatusChanged();
    // 整屏到手（QML 用它决定遮罩尺寸）；imageSize 是物理像素。
    void screenCaptured(const QSize &imageSize);
    // 要落一条记录：存储层在 AppBackend（一份 db、一个连接），这里只交出这一屏的数据。
    void saveRecordRequested(const QString &source,                             const QString &imagePath,
                             const QRect &regionPhysical,
                             const QVariantList &originals,
                             const QVariantList &blocks);
    void finished();
    // 热键按下的其它入口：mini（小窗）/ live（实时）。QML 收到后自己开窗。
    void launchRequested(const QString &kind);

private:
    void setPhase(const QString &phase);
    void setStatusText(const QString &text);
    void fail(const QString &message);
    void startTranslation();
    void rebuildBlocks();
    // 释放本次截屏的整屏位图，并把落盘的临时 PNG 删掉。
    // 调用点：换新图之前（begin）、整个流程结束（cancel）、对象析构。
    void releaseShotFile();

    studious::platform::IPlatform *platform() const;

    studious::platform::IPlatform *m_platform = nullptr;
    std::unique_ptr<studious::platform::IPlatform> m_ownedPlatform;
    QImage m_shot;                 // 整屏，物理像素
    // QML 的 Image 只能吃 URL，所以这一份要落成 PNG。路径与 URL 缓存住：同一张图只存一次，
    // 否则 QML 每求值一次 imageSourceUrl() 就多一张几十 MB 的临时文件（且永不删除）。
    mutable QString m_shotPath;    // 落盘的那一张；releaseShotFile() 会删它
    mutable QString m_shotUrl;     // imageSourceUrl() 是 const，缓存只能 mutable
    QRect m_virtualLogical;        // 抓屏那一刻的虚拟桌面逻辑矩形
    qreal m_pixelScale = 1.0;
    QRect m_selectionLogical;      // 逻辑像素
    QString m_phase = QStringLiteral("Idle");
    QString m_errorText;
    QString m_statusText;
    QString m_engineLabel;
    QVariantList m_blocks;
    QVariantList m_lastWords;      // OCR 原始结果，重新翻译时不再 OCR
    bool m_showOriginal = false;
    bool m_busy = false;
    // 本地 LLM 冷启动时的自动重试次数（QuickTranslate 说「等一下就好」时用）。
    int m_retryLeft = 0;
    // 识别中：OCR 在 工作线程跑，这段时间 m_busy 还没置位（它要到 startTranslation 才置），
    // 所以必须单独挡一次 —— 否则 Recognizing 阶段再按热键会用同一个无锁 OcrEngine 并发识别。
    // 由识别回调清（取消也清不了：线程收不回来，只能等它自己结束）。
    bool m_recognizing = false;
    // 用户点了「取消」：QuickTranslate::cancel() 会同步发一个「已取消」的 finished，
    // 那不是错误，别让它把浮层推进错误态。
    bool m_cancelling = false;
    quint64 m_shotToken = 0;
    // 在飞的那一个 OCR watcher（同一时刻最多一个，见 m_recognizing）。
    QFutureWatcherBase *m_ocrWatcher = nullptr;

    // shared_ptr 而不是 unique_ptr：工作线程的 lambda 自己也持一份，
    // 这样即使本对象析构，引擎也会活到那次 recognize() 返回为止（不受析构顺序影响）。
    std::shared_ptr<studious::ocr::OcrEngine> m_ocr;
    QuickTranslate *m_qt = nullptr;
    QString m_sourceLabel = QStringLiteral("capture");
};

} // namespace studious::ui
