#pragma once

#include <QImage>
#include <QRect>
#include <QString>

namespace studious::platform {

// DXGI Output Duplication 抓屏 —— GDI BitBlt 的兜底路径。
//
// 什么时候需要它：目标窗口的画面被扫到硬件平面（MPO / independent flip，现代游戏
// 和部分 CEF 外壳很常见）时，BitBlt 和 PrintWindow 都读不到，整块是黑的；屏幕复制
// 走的是扫描输出，能拿到。
//
// physicalRect 用物理像素（和 winCapturePhysical 的入参同一套坐标）。
// 失败返回空 QImage，原因写进 *error（中文，可以直接显示给人看）。
QImage captureWithDxgi(const QRect &physicalRect, QString *error);

} // namespace studious::platform
