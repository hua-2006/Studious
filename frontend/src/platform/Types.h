// 平台抽象层 — 纯数据类型。
//
// 铁律：本文件（以及 IPlatform.h）**绝不** include 任何平台原生头
// （Win32 SDK 头 / Xlib 头 / Wayland 协议头 / POSIX 系统头）。
// 连名字都不写进代码：本目录下这两个头被 grep 到任何平台头字面量都算违约。
// 只允许 Qt Core / Qt Gui 的类型，这样 UI 层与平台层共享同一套结构体，
// 而 NOMINMAX / UNICODE 之类的宏污染不会漏到 UI 编译单元里。
#pragma once

#include <QString>
#include <QStringList>
#include <QRect>
#include <QImage>
#include <QVector>
#include <QtGlobal>

namespace studious::platform {

// 抓取矩形，物理像素坐标，允许负值（副屏在主屏左侧时 x 为负）
// 带默认初值：抓屏失败路径会把「没填过」的 CaptureRect 直接返回给调用方，
// 不留初值就是一路带着栈垃圾走（见 memsafety #10）。
struct CaptureRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class PlatformStatus { Ok, Unsupported, Denied, Failed };

struct Capability {
    bool available = false;
    PlatformStatus status = PlatformStatus::Unsupported;
    QString reason;   // 面向用户的中文原因，可直接显示
};

struct CapabilityReport {
    Capability capture;            // 静态抓取
    Capability interactiveCapture; // 交互式框选
    Capability overlay;            // 结果浮层
    Capability hotkey;             // 全局热键
    Capability ocr;
    Capability multiMonitor;
    QString backendName;           // "null" / "win-dxgi" / "x11-shm" / "wayland-portal"
};

struct MonitorInfo {
    int id = 0;
    QString label;
    QRect geometry;     // 整屏，物理像素（含任务栏）
    QRect workRect;     // 可用区（扣掉任务栏与停靠栏），物理像素
    qreal devicePixelRatio = 1.0;
    bool primary = false;
};

struct CaptureResult {
    PlatformStatus status = PlatformStatus::Unsupported;
    QImage image;
    CaptureRect rect {};
    qreal devicePixelRatio = 1.0;
    QString error;
};

struct OcrWord {
    QString text;
    QRect box;          // 物理像素
    qreal confidence = 0.0;
};

struct OcrResult {
    PlatformStatus status = PlatformStatus::Unsupported;
    QString language;
    QVector<OcrWord> words;
    QString error;
};

struct HotkeySpec {
    QString id;         // 调用方自定义，回调时回传
    int modifiers = 0;  // 平台无关的位掩码，具体映射由实现层负责
    int key = 0;
    QString description;
};

struct OverlayItem {
    QRect box;
    QString text;
    int fontPixelSize = 0;   // 0 = 由实现层自适应
    quint32 backgroundArgb = 0;
    quint32 foregroundArgb = 0;
};

struct OverlayOptions {
    bool clickThrough = true;        // 默认穿透
    bool excludeFromCapture = true;  // 不被自己的截图拍到
    int targetScreenId = -1;
};

} // namespace studious::platform
