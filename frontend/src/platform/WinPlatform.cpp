#include "platform/WinPlatform.h"

#include "platform/OverlayWin.h"
#include "platform/ScreenCaptureWin.h"

#include <QCoreApplication>
#include <QDebug>
#include <QWindow>

#include <utility>

namespace studious::platform {
namespace {

QRect toQRect(const CaptureRect &r)
{
    return QRect(r.x, r.y, r.width, r.height);
}

CaptureRect toCaptureRect(const QRect &r)
{
    return CaptureRect { r.x(), r.y(), r.width(), r.height() };
}

Capability capability(bool available, QString reason = QString())
{
    Capability c;
    c.available = available;
    c.status = available ? PlatformStatus::Ok : PlatformStatus::Unsupported;
    c.reason = std::move(reason);
    return c;
}

// 浮层的三个能力都要求窗口已经有原生句柄。winId() 会把句柄逼出来，
// 所以这里显式调一次，而不是指望调用方记得先 show()。
bool ensureNativeHandle(QWindow *w, QString *error)
{
    if (!w) {
        if (error)
            *error = QCoreApplication::translate("WinPlatform", "浮层窗口不存在。");
        return false;
    }
    if (w->winId() == 0) {
        if (error)
            *error = QCoreApplication::translate("WinPlatform", "浮层窗口还没创建原生句柄，先 show() 再改它。");
        return false;
    }
    return true;
}

} // namespace

WinPlatform::WinPlatform(QObject *parent)
    : QObject(parent)
{
    // HotkeyWin 的信号必须在这里接上：窗口过程只负责排队，emit 留在事件循环里，
    // 这样槽里再去 registerHotkey/unregisterHotkey 也不会重入窗口过程。
    connect(&m_hotkeys, &HotkeyWin::activated, this, [this](const QString &id) {
        emit hotkeyPressed(id);
        if (m_hotkeyHandler)
            m_hotkeyHandler(id);
    });
}

WinPlatform::~WinPlatform() = default;

CapabilityReport WinPlatform::capabilities() const
{
    CapabilityReport report;

    const bool canCapture = winCaptureAvailable();
    report.capture = capability(canCapture,
                               canCapture ? QString()
                                          : QCoreApplication::translate("WinPlatform",
                                                                        "当前会话拿不到屏幕内容（无桌面或受保护）。"));
    // 交互式框选的「抓整屏」这一步和静态抓取共用同一个 BitBlt，
    // 区别只在遮罩由 QML 画（ScreenFlow），所以能力位跟静态抓取一致。
    report.interactiveCapture = report.capture;
    report.overlay = capability(true);
    report.hotkey = capability(true);
    // OCR 的模型与推理都在后端（studious::ocr::OcrEngine），平台层只报"能力在别处"。
    report.ocr = capability(false,
                            QCoreApplication::translate("WinPlatform",
                                                        "OCR 由后端引擎提供，模型需在资产页下载。"));
    report.multiMonitor = capability(winMonitors().size() > 1);
    report.backendName = backendName();
    return report;
}

QString WinPlatform::backendName() const
{
    // 现在是纯 GDI（BitBlt）；DXGI Desktop Duplication 作为独占全屏/HDR 的兜底还没接。
    return QStringLiteral("win-gdi");
}

QVector<MonitorInfo> WinPlatform::monitors() const
{
    return winMonitors();
}

QRect WinPlatform::virtualDesktopRect() const
{
    return winVirtualDesktopRect();
}

CaptureResult WinPlatform::captureRect(const CaptureRect &rect)
{
    return winCaptureRect(toQRect(rect));
}

CaptureResult WinPlatform::captureMonitor(int monitorId)
{
    const QVector<MonitorInfo> all = winMonitors();
    if (monitorId < 0 || monitorId >= all.size()) {
        CaptureResult r;
        r.status = PlatformStatus::Failed;
        r.error = QCoreApplication::translate("WinPlatform", "没有第 %1 块屏幕。").arg(monitorId);
        return r;
    }
    return winCaptureRect(all.at(monitorId).geometry);
}

CaptureResult WinPlatform::captureInteractive()
{
    // 框选本身是 Qt Quick 的活（SelectionOverlay 铺满虚拟桌面、画选框、收鼠标），
    // 平台层只提供「先把整屏抓下来」这一步 —— 顺序反了遮罩会拍进截图。
    // 所以这里直接返回整屏位图，选区由 ScreenFlow 在这张图上裁。
    return winCaptureVirtualDesktop();
}

void WinPlatform::cancelInteractive()
{
    // 平台层不持有框选状态（它没有遮罩窗口），取消动作由 ScreenFlow 关掉自己的窗口完成。
}

QWindow *WinPlatform::createOverlay(const OverlayOptions &)
{
    // 刻意不创建窗口：浮层是 QML 里的 QQuickWindow（需要 QQmlEngine 与场景图），
    // 平台层只负责改造调用方建好的窗口（setClickThrough / setExcludeFromCapture /
    // placeOverlay）。返回 nullptr 就是「本平台不支持由平台层创建」。
    return nullptr;
}

void WinPlatform::rememberClickThrough(QWindow *w, bool on)
{
    if (w == nullptr)
        return;

    if (!m_clickThrough.contains(w)) {
        // 只给每个窗口连一次。destroyed 在 QObject 析构里发，那时指针本身还有效，
        // 拿来当键擦除是安全的；连接以 this 为 context，本对象先死时自动断开，
        // 不会回调到已析构的 WinPlatform。
        connect(w, &QObject::destroyed, this, [this](QObject *obj) {
            m_clickThrough.remove(static_cast<QWindow *>(obj));
        });
    }
    m_clickThrough.insert(w, on);
}

bool WinPlatform::setClickThrough(QWindow *w, bool on, QString *error)
{
    if (!ensureNativeHandle(w, error))
        return false;
    // 只改穿透，不动「截图排除」。以前这里顺手把排除一起打开，导致控制条调
    // setClickThrough(false) 之后仍然拍不到（affinity 还是 0x11）——用户拿截图
    // 工具反馈问题时，看得见却拍不下来。
    // 需要排除截图的窗口（译文浮层）自己会再调 setExcludeFromCapture(true)。
    // 反过来的方向现在也是安全的：winApplyOverlayFlags 在 exclude=false 时会把
    // 窗口原有的 display affinity 读回来、改完样式再回填 —— 锁定/解锁切穿透
    // 不会再把 WDA_EXCLUDEFROMCAPTURE 顺手清掉（清掉就等于浮层进了录屏）。
    if (!winApplyOverlayFlags(w, on, /*excludeFromCapture=*/false, error))
        return false;
    // 只有真的设成功才记账：失败时下次 setExcludeFromCapture 回填的仍是旧值，
    // 而不是一个没落到窗口上的「我们以为的状态」。
    rememberClickThrough(w, on);
    return true;
}

bool WinPlatform::setExcludeFromCapture(QWindow *w, bool on, QString *error)
{
    if (!ensureNativeHandle(w, error))
        return false;
    // winApplyOverlayFlags 一次改两项。这里必须回填**该窗口当前的**穿透态，
    // 不能写死 true：实时译文浮层解锁后是不穿透的（要接鼠标拖手柄），
    // 写死 true 会把 WS_EX_TRANSPARENT 又加回去，手柄立刻点不动。
    // 没记录过就按 false 兜底（本平台没给它设过穿透，说明它不是穿透浮层）。
    const bool clickThrough = m_clickThrough.value(w, false);
    return winApplyOverlayFlags(w, clickThrough, on, error);
}

bool WinPlatform::placeOverlay(QWindow *w, const QRect &globalRect, QString *error)
{
    if (!ensureNativeHandle(w, error))
        return false;
    return winPlaceOverlay(w, globalRect, error);
}

void WinPlatform::setHotkeyHandler(HotkeyHandler handler)
{
    // 只存不转发：真正的注册在 registerHotkey() 里完成，回调统一在上面那个 lambda 出口。
    m_hotkeyHandler = std::move(handler);
}

bool WinPlatform::registerHotkey(const HotkeySpec &spec)
{
    QString error;
    const bool ok = m_hotkeys.registerHotkey(spec, &error);
    if (!ok)
        qWarning("[platform] registerHotkey(%s) failed: %s",
                 qUtf8Printable(spec.id),
                 qUtf8Printable(error));
    return ok;
}

void WinPlatform::unregisterHotkey(const QString &id)
{
    m_hotkeys.unregisterHotkey(id);
}

QStringList WinPlatform::ocrLanguages() const
{
    return { QStringLiteral("zh"), QStringLiteral("en") };
}

OcrResult WinPlatform::recognize(const QImage &, const QStringList &)
{
    OcrResult r;
    r.status = PlatformStatus::Unsupported;
    r.error = QCoreApplication::translate("WinPlatform",
                                          "OCR 由后端引擎提供，请通过 AppBackend 调用。");
    return r;
}

} // namespace studious::platform
