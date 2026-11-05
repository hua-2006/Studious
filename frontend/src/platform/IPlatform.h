// 平台抽象层 — 唯一接缝。
//
// 铁律：本文件（以及 Types.h）**绝不** include 任何平台原生头
// （Win32 SDK 头 / Xlib 头 / Wayland 协议头 / POSIX 系统头）。
//
// QWidget 只在返回值里出现指针，故这里用**前向声明**而不是 #include <QWidget>：
// 这样 studious_platform 只需要链 Qt6::Core + Qt6::Gui，不用把 Widgets 拖进平台层。
// 需要真正创建浮层的实现（platform/win、platform/linux）自己在 .cpp 里 include。
#pragma once

#include "platform/Types.h"

#include <QString>
#include <QStringList>
#include <QRect>
#include <QImage>
#include <QVector>

class QWidget; // 前向声明，勿改成 #include <QWidget>

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
    virtual QWidget *createOverlay(const OverlayOptions &) = 0;

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
