// 平台层 — Windows 屏幕抓取（对外纯 C++ 函数，不含 QML / 不依赖 IPlatform 派生类）。
//
// 铁律（见 platform/README.md）：本文件不引入任何原生 SDK 头，也不出现原生句柄 /
// 原生矩形类型名（连名字都不写进代码，和 Types.h 同规矩）。所有原生细节锁在 .cpp 的
// #if defined(Q_OS_WIN) 分支里，这里只有 bool / QRect / QVector / Qt 数据类型。
//
// 坐标契约：本模块所有矩形都是**物理像素**（不是 Qt 逻辑像素），原点为虚拟桌面左上角。
// 副屏位于主屏左侧时 x 为负值，上下排列时 y 为负值 —— 抓取层原样返回，不做归一化。
// 需要逻辑坐标的地方由调用方自己用 QScreen::devicePixelRatio() 换算。
//
// 线程：四个函数都没有共享可变状态，可在任意线程（含 QtConcurrent / std::thread）
// 直接调用；每次调用自行创建并释放 GDI 句柄，不要在工作线程里再套 GUI 线程的 DC 缓存。
#pragma once

#include "platform/Types.h"

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

namespace studious::platform {

// 平台能力探测：非 Windows 恒 false。Windows 下会真的拿一次屏幕 DC 试手，
// 拿不到（例如服务会话 / 无桌面）也返回 false。
bool winCaptureAvailable();

// 单个屏幕/虚拟桌面的几何信息（物理像素坐标，左上角是虚拟桌面原点）。
//
// 字段来源（Types.h 的 MonitorInfo 目前只有这些字段）：
//   id               = 枚举序号（0 起），不是系统句柄，别存盘当稳定标识
//   label            = 系统显示设备名，形如 "\\.\DISPLAY1"
//   geometry         = 整个屏幕的物理矩形（含任务栏区域，即显示器固有边界）
//   devicePixelRatio = dpi / 96.0（每显示器有效 DPI；系统不给就回落 96 == 1.0）
//   primary          = 是否为系统「主显示器」
// 注意：可用工作区（去掉任务栏后的区域）在 MonitorInfo 里**还没有**字段，本实现读出来
// 就直接丢弃；Types.h 补一个 QRect workRect 之后，.cpp 的显示器枚举回调里有一行现成的可填。
QVector<MonitorInfo> winMonitors();

// 整个虚拟桌面的包围盒（物理像素）。空 QRect = 拿不到（无显示器 / 无桌面）。
QRect winVirtualDesktopRect();

// 抓取虚拟桌面里的一块矩形（物理像素）。返回 ok=false 时 error 给人话中文。
//
// 语义：
//   · 入参会自动与虚拟桌面求交；完全落在屏幕外 → status=Failed，error="选区不在任何屏幕内"。
//   · 宽或高 <= 0 → status=Failed，error="选区为空（宽高必须大于 0）"。
//   · 成功 → status=Ok，image 为 Format_RGB32，image 尺寸 == 返回值 rect 的宽高，
//     逐像素与物理坐标 1:1（没有做任何缩放）；rect 是**求交之后**的实际区域。
//   · devicePixelRatio = 该区域所在屏幕的 dpi/96；跨了不同 DPI 的屏幕时置 1.0
//     （拼图没有单一缩放，调用方按 winMonitors() 逐屏换算）。
//   · 光标不会被画进图里（设计稿没有光标）。
CaptureResult winCaptureRect(const QRect &physicalRect);

// 抓整个虚拟桌面（多屏拼接）。等价于 winCaptureRect(winVirtualDesktopRect())，
// 但省掉一次几何查询，并在虚拟桌面为空时给专门的错误文案。
CaptureResult winCaptureVirtualDesktop();

} // namespace studious::platform
