#include "platform/PlatformFactory.h"

#include "platform/NullPlatform.h"

#if defined(Q_OS_WIN)
#    include "platform/WinPlatform.h"
#endif

namespace studious::platform {

std::unique_ptr<IPlatform> createPlatform()
{
#if defined(Q_OS_WIN)
    // Windows：ScreenCaptureWin（GDI BitBlt）+ HotkeyWin（RegisterHotKey）
    //            + OverlayWin（WS_EX_* / WDA_EXCLUDEFROMCAPTURE）
    return std::make_unique<WinPlatform>();
#else
    // Linux：X11 版（XShm 抓屏 + XGrabKey + override-redirect 浮层）与
    // Wayland portal 版都还没写。这两条路在 platform/README.md 里记了取舍：
    //   · X11 没有 WDA_EXCLUDEFROMCAPTURE 的等价物（要 XComposite 直取窗口 pixmap，
    //     且做不到跨窗口排除）→ 实时翻译的「浮层不被自己拍进去」无法保证；
    //   · Wayland 既不能排除、也不能定位窗口、也没有全局热键 → 实时翻译不可做。
    // 所以这里先恒返回 NullPlatform（所有能力返回 Unsupported + 人话原因），
    // 等 X11 / portal 实现落地再改这一行。
    return std::make_unique<NullPlatform>();
#endif
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
