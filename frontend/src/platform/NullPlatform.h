// NullPlatform — 空实现。
//
// 本任务只定接口、不写真实能力：所有能力一律 Unsupported，
// 调用方据此走降级路径（隐藏菜单项 / 显示"当前平台尚未实现"）。
#pragma once

#include "platform/IPlatform.h"

namespace studious::platform {

class NullPlatform final : public IPlatform {
public:
    NullPlatform() = default;
    ~NullPlatform() override = default;

    CapabilityReport capabilities() const override;
    QString backendName() const override;

    QVector<MonitorInfo> monitors() const override;
    QRect virtualDesktopRect() const override;

    CaptureResult captureRect(const CaptureRect &) override;
    CaptureResult captureMonitor(int monitorId) override;

    CaptureResult captureInteractive() override;
    void cancelInteractive() override;

    QWindow *createOverlay(const OverlayOptions &) override;

    bool setClickThrough(QWindow *w, bool on, QString *error) override;
    bool setExcludeFromCapture(QWindow *w, bool on, QString *error) override;
    bool placeOverlay(QWindow *w, const QRect &globalRect, QString *error) override;

    void setHotkeyHandler(HotkeyHandler) override {}
    bool registerHotkey(const HotkeySpec &) override;
    void unregisterHotkey(const QString &id) override;

    QStringList ocrLanguages() const override;
    OcrResult recognize(const QImage &, const QStringList &langs) override;
};

} // namespace studious::platform
