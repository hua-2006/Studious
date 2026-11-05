#include "platform/PlatformFactory.h"

#include "platform/NullPlatform.h"

namespace studious::platform {

std::unique_ptr<IPlatform> createPlatform()
{
    // 以后这里按平台分支：
    //   #ifdef Q_OS_WIN
    //       return std::make_unique<WinPlatform>();      // platform/win/WinPlatform.cpp
    //   #elif defined(Q_OS_LINUX) && defined(STUDIOUS_HAVE_X11)
    //       return std::make_unique<X11Platform>();      // platform/linux/X11Platform.cpp
    //   #elif defined(Q_OS_LINUX)
    //       return std::make_unique<WaylandPortalPlatform>(); // platform/linux/WaylandPortalPlatform.cpp
    //   #else
    //       return std::make_unique<NullPlatform>();
    //   #endif
    // 本任务只定接缝：无条件 NullPlatform。
    return std::make_unique<NullPlatform>();
}

// ---------------------------------------------------------------------------
// IPlatform 的静态入口。
//
// 放在工厂这个翻译单元里，是因为"平台选择"和"平台命名"是同一件事：
// 以后加 win/linux 实现时，这三个函数的 #ifdef 分支也在这里一起长。
// 注意：这里只用 Q_OS_* 宏，不 include 任何平台头，保持平台层头文件干净。
// ---------------------------------------------------------------------------

void IPlatform::initProcessDpi()
{
    // 真实实现（Windows）：在 QApplication 构造前 SetProcessDpiAwarenessContext，
    // 且只生效一次，所以调用方必须在 main() 里抢在 QApplication 之前调。
    // 空实现 = 保持 Qt 默认行为，对 NullPlatform 来说已经足够。
}

QString IPlatform::runtimeAssetSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("win-x64");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux-x64");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos-arm64");
#else
    return QStringLiteral("unknown");
#endif
}

QString IPlatform::serverExecutableName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("llama-server.exe");
#else
    return QStringLiteral("llama-server");
#endif
}

} // namespace studious::platform
