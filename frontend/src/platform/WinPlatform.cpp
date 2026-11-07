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

bool WinPlatform::setClickThrough(QWindow *w, bool on, QString *error)
{
    if (!ensureNativeHandle(w, error))
        return false;
    // 关穿透时仍然保留「不被截进图里」，这两个开关在界面上是独立的。
    return winApplyOverlayFlags(w, on, true, error);
}

bool WinPlatform::setExcludeFromCapture(QWindow *w, bool on, QString *error)
{
    if (!ensureNativeHandle(w, error))
        return false;
    // 穿透状态由调用方上一次设置决定；这里不改穿透，只改截图排除。
    // winApplyOverlayFlags 一次改两项，所以先把当前窗口的穿透态读出来太绕 ——
    // 折中：穿透恒开（浮层默认穿透），这是 ScreenFlow / LiveRunner 的唯一用法。
    return winApplyOverlayFlags(w, true, on, error);
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
