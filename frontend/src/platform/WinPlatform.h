// Windows 平台实现 —— 组合 ScreenCaptureWin / HotkeyWin / OverlayWin。
//
// 这一层只做「把三个单一职责的原生模块拼成 IPlatform」和「回答能力矩阵」，
// 自己一行 Win32 都不写（所以本文件也不 include 任何平台头）。
// 想加别的平台：照着这个文件复制一份 LinuxPlatform，把三个模块换成 X11/portal 版即可。
#pragma once

#include "platform/IPlatform.h"
#include "platform/HotkeyWin.h"

#include <QHash>
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
    bool registerHotkey(const HotkeySpec &, QString *error = nullptr) override;
    void unregisterHotkey(const QString &id) override;

    QStringList ocrLanguages() const override;
    OcrResult recognize(const QImage &, const QStringList &langs) override;

signals:
    void hotkeyPressed(const QString &id);

private:
    HotkeyWin m_hotkeys;
    HotkeyHandler m_hotkeyHandler;

    // 记下「本平台最后一次成功设下的穿透态」，setExcludeFromCapture 回填时用：
    // winApplyOverlayFlags 一次改两项，而排除截图不该顺手把穿透改掉（实时浮层解锁后
    // 是**不穿透**的，被它改回穿透，八个手柄就全点不动了）。
    // 默认 false（= 不穿透）：没设过时按「不穿透」兜底，比默认 true 安全 ——
    // 猜成穿透会让浮层接不到鼠标，猜成不穿透最多是浮层不该接的时候可点。
    // 窗口销毁时必须擦掉条目（见 rememberClickThrough），否则裸指针键会悬空，
    // 同一个地址被新窗口复用时，状态就套到别人的窗口上了。
    QHash<QWindow *, bool> m_clickThrough;

    // 记一次穿透态；第一次见到这个窗口时顺手接上它的 destroyed，销毁即擦除。
    void rememberClickThrough(QWindow *w, bool on);
};

} // namespace studious::platform
