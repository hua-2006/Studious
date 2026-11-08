#pragma once

// 进程级日志汇：Qt 消息处理器 + 环形缓冲 + 落盘 + 变更通知。
//
// 设计边界：
//   · 只做「收集 / 落盘 / 告诉外面有新行」，不做节流；
//     UI 侧的合并（200 ms 一次 logTextChanged）由 AppBackend 负责 ——
//     handler 跑在任意线程，控制刷新节奏的东西应该留在有事件循环的那一侧。
//   · 安装后仍会调用原来的 handler，终端输出（--smoke 的 [OK]/[DONE]）不受影响。
//   · 路径口径复用 paths::assetsRoot()：默认 <AppData>/studious/assets，
//     日志落在同级的 <AppData>/studious/logs/studious.log。

#include <QString>
#include <QStringList>

#include <functional>

namespace studious::ui::logsink {

// 装 qInstallMessageHandler（幂等，重复调用是空操作）。
void install();

// 日志文件绝对路径（目录不存在时会先建出来）。
QString filePath();

// 缓冲区全文（最近 N 行，带时间戳），多行用 '\n' 连接。
QString text();

// 最近 maxLines 行；maxLines <= 0 时返回空。
QStringList tail(int maxLines = 500);

// 清空内存缓冲（不删磁盘文件）。
void clear();

// 有新日志时回调。**可能来自任意线程**，实现方自己转投到目标线程。
// 传空函数即注销。
void setNotify(std::function<void()> notify);

// 立即把缓冲区落盘（退出 / 崩溃前用）。
void flushToDisk();

}   // namespace studious::ui::logsink
