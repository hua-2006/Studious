// 平台抽象层 — 唯一接缝。
//
// 铁律：本文件（以及 Types.h）**绝不** include 任何平台原生头
// （Win32 SDK 头 / Xlib 头 / Wayland 协议头 / POSIX 系统头）。
//
// QWindow 只在返回值里出现指针，故这里用**前向声明**而不是 #include <QWindow>，
// 免得平台层的头文件把 GUI 模块的细节（乃至 QWidget）拖进来。
// 需要真正创建浮层的实现（platform/win、platform/linux）自己在 .cpp 里 include。
#pragma once

#include "platform/Types.h"

#include <QString>
#include <QStringList>
#include <QRect>
#include <QImage>
#include <QVector>

#include <functional>

class QWindow; // 前向声明，勿改成 #include <QWindow>

namespace studious::platform {

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual CapabilityReport capabilities() const = 0;
    virtual QString backendName() const = 0;

    virtual QVector<MonitorInfo> monitors() const = 0;
    virtual QRect virtualDesktopRect() const = 0;   // Wayland 返回空 QRect 是"事实"，不是错误

    virtual CaptureResult captureRect(const CaptureRect &) = 0;
    virtual CaptureResult captureMonitor(int monitorId) = 0;

    // 交互式框选。实现铁律：必须先抓整屏、再显示遮罩，顺序反了遮罩会拍进截图。
    virtual CaptureResult captureInteractive() = 0;
    virtual void cancelInteractive() = 0;

    // 浮层。不支持时返回 nullptr，调用方据此降级。
    // 返回 QWindow 而不是 QWidget：界面是 Qt Quick / QML，浮层要用 QQuickWindow 画，
    // 而 Widgets 从没进过这个项目的链接表。实现方负责设置窗口标志与生命周期
    // （调用方拿到后自己 setParent / deleteLater）。
    virtual QWindow *createOverlay(const OverlayOptions &) = 0;

    // 浮层窗口的三个附加能力。拆成独立方法而不是塞进 createOverlay() 的选项，
    // 是因为窗口是 QML 建的（QQuickWindow），平台层只负责"改造它"，不负责创建。
    // w 必须已经 show() 过（有原生句柄）；失败返回 false，原因通过 *error 给人话。
    virtual bool setClickThrough(QWindow *w, bool on, QString *error) = 0;
    virtual bool setExcludeFromCapture(QWindow *w, bool on, QString *error) = 0;
    // globalRect 是**物理像素**（虚拟桌面坐标系），实现方不许再做逻辑坐标换算。
    virtual bool placeOverlay(QWindow *w, const QRect &globalRect, QString *error) = 0;

    // 热键回调。平台层不引用 Qt 信号（IPlatform 不是 QObject），用一个纯函数对象当出口：
    // 谁注册的谁接收，id 原样回传（capture / mini / live）。
    using HotkeyHandler = std::function<void(const QString &id)>;
    virtual void setHotkeyHandler(HotkeyHandler handler) = 0;

    virtual bool registerHotkey(const HotkeySpec &) = 0;
    virtual void unregisterHotkey(const QString &id) = 0;

    virtual QStringList ocrLanguages() const = 0;
    // 同步阻塞，调用方必须在工作线程调用，禁止 GUI 线程直调。
    virtual OcrResult recognize(const QImage &, const QStringList &langs) = 0;

    // 必须在 QApplication 构造之前调用（Windows 的 DPI 感知只生效一次）。
    static void initProcessDpi();

    // 平台相关的资产命名，用来去掉 UI 里的 win-x64 / llama-server.exe 硬编码。
    static QString runtimeAssetSuffix();    // "win-x64" / "linux-x64"
    static QString serverExecutableName();  // "llama-server.exe" / "llama-server"
};

} // namespace studious::platform
