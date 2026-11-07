#include "platform/NullPlatform.h"

namespace studious::platform {
namespace {

// 唯一的用户可见文案来源。以后 win/linux 实现各自给原因，不在这里改。
QString unsupportedReason()
{
    return QStringLiteral("当前平台尚未实现");
}

Capability unsupportedCapability()
{
    Capability c;
    c.available = false;
    c.status = PlatformStatus::Unsupported;
    c.reason = unsupportedReason();
    return c;
}

CaptureResult unsupportedCapture()
{
    CaptureResult r;
    r.status = PlatformStatus::Unsupported;
    r.error = unsupportedReason();
    r.devicePixelRatio = 1.0;
    return r;
}

} // namespace

CapabilityReport NullPlatform::capabilities() const
{
    CapabilityReport report;
    report.capture            = unsupportedCapability();
    report.interactiveCapture = unsupportedCapability();
    report.overlay            = unsupportedCapability();
    report.hotkey             = unsupportedCapability();
    report.ocr                = unsupportedCapability();
    report.multiMonitor       = unsupportedCapability();
    report.backendName        = QStringLiteral("null");
    return report;
}

QString NullPlatform::backendName() const
{
    return QStringLiteral("null");
}

QVector<MonitorInfo> NullPlatform::monitors() const
{
    return {};
}

QRect NullPlatform::virtualDesktopRect() const
{
    // 空 QRect 表示"无法得知"，调用方必须当成事实而不是错误。
    return {};
}

CaptureResult NullPlatform::captureRect(const CaptureRect &)
{
    return unsupportedCapture();
}

CaptureResult NullPlatform::captureMonitor(int)
{
    return unsupportedCapture();
}

CaptureResult NullPlatform::captureInteractive()
{
    // 真实实现铁律：先抓整屏、再显示遮罩。空实现什么都不做，所以不存在顺序问题。
    return unsupportedCapture();
}

void NullPlatform::cancelInteractive()
{
    // 没有进行中的交互，无事可做。
}

QWindow *NullPlatform::createOverlay(const OverlayOptions &)
{
    // nullptr = 不支持浮层，调用方据此降级（例如改用普通窗口或状态栏提示）。
    return nullptr;
}

bool NullPlatform::setClickThrough(QWindow *, bool, QString *error)
{
    if (error) {
        *error = unsupportedReason();
    }
    return false;
}

bool NullPlatform::setExcludeFromCapture(QWindow *, bool, QString *error)
{
    if (error) {
        *error = unsupportedReason();
    }
    return false;
}

bool NullPlatform::placeOverlay(QWindow *, const QRect &, QString *error)
{
    if (error) {
        *error = unsupportedReason();
    }
    return false;
}

bool NullPlatform::registerHotkey(const HotkeySpec &)
{
    return false;
}

void NullPlatform::unregisterHotkey(const QString &)
{
    // 没注册成功过，无需注销。
}

QStringList NullPlatform::ocrLanguages() const
{
    return {};
}

OcrResult NullPlatform::recognize(const QImage &, const QStringList &)
{
    OcrResult r;
    r.status = PlatformStatus::Unsupported;
    r.error = unsupportedReason();
    return r;
}

} // namespace studious::platform
