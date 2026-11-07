// 浮层的 Win32 落地：置顶 / 点击穿透 / 截图排除 / 物理像素定位。
//
// 只跟 QWindow 打交道，头文件里不出现任何 Win32 类型 —— 调用方（QML 侧）
// 不需要 include <windows.h>，也不需要知道 HWND 长什么样。
#pragma once

#include <QRect>
#include <QString>

class QWindow;

namespace studious::platform {

// 把已有窗口变成浮层。w 必须已经 show() 过（有原生句柄：w->winId() 会强制创建）。
// clickThrough = true → 只加/减 WS_EX_LAYERED | WS_EX_TRANSPARENT（外加恒定的
//                      TOOLWINDOW / NOACTIVATE），鼠标事件穿透到下面的程序（实时翻译硬约束）。
// excludeFromCapture = true → SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)，
//                      让抓屏拍不到浮层自己（Win10 2004+；老系统返回 false 但不报错）。
//
// **两项互不干扰**：excludeFromCapture == false 时只改扩展样式，窗口原有的
// display affinity 先读回、改完样式再回填 —— 改 GWL_EXSTYLE 有可能把 affinity
// 顺手清掉，以前就是这样：锁定选区时设了排除，解锁时 setClickThrough(false)
// 一改样式，浮层立刻又进了录屏。反过来，excludeFromCapture == true 也不碰穿透位。
//
// 返回 false 表示至少一步失败，error 给人话中文（可为 nullptr）。
bool winApplyOverlayFlags(QWindow *w, bool clickThrough, bool excludeFromCapture, QString *error);

// 置顶（HWND_TOPMOST）+ 放到全局物理像素矩形上。内部处理 DPR：
// 传进来的 globalRect 是**物理像素**，用 SetWindowPos 直接设物理像素，
// 不要经 Qt 的逻辑坐标换算。
bool winPlaceOverlay(QWindow *w, const QRect &globalRectPhysical, QString *error);

// 解除浮层状态（关闭时调用，窗口即将销毁，失败可忽略）。
void winClearOverlayFlags(QWindow *w);

} // namespace studious::platform
