#include "platform/ProcessGuard.h"

#include <QDebug>

#if defined(Q_OS_WIN)
// Windows 的作业对象（Job Object）。这里放宽「不引平台头」的约束是安全的：
// 本文件属于 platform/ 目录，平台相关代码的唯一合法落脚点。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace studious::platform {

#if defined(Q_OS_WIN)

ProcessGuard::~ProcessGuard()
{
    release();
}

void ProcessGuard::attach(qint64 pid)
{
    release();

    if (pid <= 0) {
        return;
    }

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        qWarning("[guard] CreateJobObject failed (%lu), orphan guard disabled", ::GetLastError());
        return;
    }

    // KILL_ON_JOB_CLOSE：最后一个 Job 句柄关闭时（包括持有它的进程消失），
    // 系统会把 Job 里的所有进程一起杀掉。这就是我们要的行为。
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))
        == FALSE) {
        qWarning("[guard] SetInformationJobObject failed (%lu)", ::GetLastError());
        ::CloseHandle(job);
        return;
    }

    HANDLE child =
        ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (child == nullptr) {
        qWarning("[guard] OpenProcess(%lld) failed (%lu)", static_cast<long long>(pid),
                 ::GetLastError());
        ::CloseHandle(job);
        return;
    }

    if (::AssignProcessToJobObject(job, child) == FALSE) {
        // 两个已知原因：父进程自己已在别的 Job 里（Win8+ 允许嵌套，
        // Win7 返回 ERROR_ACCESS_DENIED），或子进程已经退出。
        // 都不算错误，只是这次没有兜底。
        qWarning("[guard] AssignProcessToJobObject failed (%lu), orphan guard disabled",
                 ::GetLastError());
        ::CloseHandle(child);
        ::CloseHandle(job);
        return;
    }
    ::CloseHandle(child);

    // 句柄故意留着不关：它一关，Job 就结束，成员进程全被杀。
    m_handle = job;
}

void ProcessGuard::release()
{
    if (m_handle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool ProcessGuard::active() const
{
    return m_handle != nullptr;
}

#else

// POSIX 对应物是 prctl(PR_SET_PDEATHSIG, SIGKILL)（Linux）或设置父进程为 init。
// 目前本地引擎的 llama 运行时资产只有 Windows 版是真条目，Linux 侧等
// manifest 补上 runtime.llama.*.linux-x64 的下载地址后再实现，避免写一段永远跑不到的代码。
ProcessGuard::~ProcessGuard() = default;

void ProcessGuard::attach(qint64 pid)
{
    Q_UNUSED(pid);
}

void ProcessGuard::release() {}

bool ProcessGuard::active() const
{
    return false;
}

#endif

} // namespace studious::platform
