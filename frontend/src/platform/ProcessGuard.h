// ProcessGuard — 子进程孤儿守护。
//
// 场景：本进程被任务管理器强杀 / 崩溃时，析构函数根本不会执行，
// 已经拉起的 llama-server 就会变成孤儿进程，占着端口和几个 GB 内存。
// 这里把子进程挂进一个「父句柄一消失就杀光成员」的容器，让系统替我们收尸。
//
// 铁律同 IPlatform.h：本头文件**不** include 任何平台原生头，
// 句柄以 void * 形式持有；真正的 Win32 调用只出现在 ProcessGuard.cpp。
// 以后补 Linux 时只改那个 .cpp（prctl(PR_SET_PDEATHSIG) 或 PR_SET_CHILD_SUBREAPER）。
#pragma once

#include <QtGlobal>

namespace studious::platform {

class ProcessGuard {
public:
    ProcessGuard() = default;
    ~ProcessGuard();

    ProcessGuard(const ProcessGuard &) = delete;
    ProcessGuard &operator=(const ProcessGuard &) = delete;

    // 把 pid 挂进守护容器。已有句柄会先释放（等于换一个父容器）。
    // 失败不抛异常、不返回错误：孤儿守护是尽力而为的兜底，
    // 装不上只是少一层保险，功能本身照常。
    void attach(qint64 pid);

    // 主动释放守护。子进程已经正常停掉时必须调，否则句柄会短暂留下。
    void release();

    // 当前是否真的装上了兜底（用于日志 / 自检）。
    bool active() const;

private:
    void *m_handle = nullptr;   // Windows: HANDLE；其它平台恒为 nullptr
};

// 杀掉本进程的所有直接子进程。给「关闭软件」用：引擎可能正在启动（还没拿到
// runner 句柄），这时走正常 stop 会漏掉它，留在后台一直占显存。
// 只按父进程号匹配，不碰别人的进程；非 Windows 上是空实现。
void killChildProcesses();

} // namespace studious::platform
