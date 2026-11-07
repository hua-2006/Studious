// Windows 平台实现 —— 组合 ScreenCaptureWin / HotkeyWin / OverlayWin。
//
// 这一层只做「把三个单一职责的原生模块拼成 IPlatform」和「回答能力矩阵」，
// 自己一行 Win32 都不写（所以本文件也不 include 任何平台头）。
// 想加别的平台：照着这个文件复制一份 LinuxPlatform，把三个模块换成 X11/portal 版即可。
#pragma once

#include "platform/IPlatform.h"
#include "platform/HotkeyWin.h"

#include <QObject>
#include <memory>

namespace studious::platform {

class WinPlatform final : public QObject, public IPlatform {
    Q_OBJECT

public:
    explicit WinPlatform(QObject *parent = nullptr);
    ~WinPlatform() override;

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

    void setHotkeyHandler(HotkeyHandler handler) override;
    bool registerHotkey(const HotkeySpec &) override;
    void unregisterHotkey(const QString &id) override;

    QStringList ocrLanguages() const override;
    OcrResult recognize(const QImage &, const QStringList &langs) override;

signals:
    void hotkeyPressed(const QString &id);

private:
    HotkeyWin m_hotkeys;
    HotkeyHandler m_hotkeyHandler;
};

} // namespace studious::platform
