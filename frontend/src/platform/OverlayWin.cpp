#include "platform/OverlayWin.h"

#include <QCoreApplication>
#include <QDebug>
#include <QWindow>

#if defined(Q_OS_WIN)
// 平台头只允许出现在 platform/ 目录的实现文件里（见 Desktop.cpp / ProcessGuard.cpp 的同一约定）。
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>

// WDA_EXCLUDEFROMCAPTURE（0x11）是 Windows 10 2004（build 19041）才加的，
// 老版本 SDK 的 winuser.h 里没有这个枚举值，所以自己补一个。数值是稳定的 ABI，不会变。
#    ifndef WDA_EXCLUDEFROMCAPTURE
#        define WDA_EXCLUDEFROMCAPTURE 0x00000011
#    endif
// WDA_NONE 是 Vista 就有的，按理说一定有；照样补一个，免得老 mingw 头把这段编译掉。
#    ifndef WDA_NONE
#        define WDA_NONE 0x00000000
#    endif
#endif

namespace studious::platform {

namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

#if defined(Q_OS_WIN)

// 浮层恒定的扩展样式：
//   WS_EX_TOOLWINDOW  —— 不进 Alt-Tab、不在任务栏留按钮（浮层不是"窗口"）；
//   WS_EX_NOACTIVATE  —— 点它也不抢前台焦点（翻译浮层一抢焦点就把用户正在输入的框踢掉）。
// 这两个必须**始终**带上，只有 clickThrough 才额外加 LAYERED | TRANSPARENT。
constexpr LONG_PTR kAlwaysOverlayExStyles = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
constexpr LONG_PTR kClickThroughExStyles = WS_EX_LAYERED | WS_EX_TRANSPARENT;

QString errorText(const QString &what, DWORD code)
{
    return QCoreApplication::translate("OverlayWin", "%1（错误码 %2）")
        .arg(what, QString::number(static_cast<qulonglong>(code)));
}

// winId() 会强制 Qt 创建原生窗口（QQuickWindow 走 QRhi 的合成器路径）。
// 它只保证「有 HWND 了」，不保证窗口已经显示 —— 调用方必须先 show() 再调下面几个函数，
// 否则拿到的是一个还没算过尺寸/可见性的句柄。
HWND hwndOf(QWindow *w)
{
    if (w == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(w->winId());
}

#endif // Q_OS_WIN

} // namespace

bool winApplyOverlayFlags(QWindow *w, bool clickThrough, bool excludeFromCapture, QString *error)
{
#if defined(Q_OS_WIN)
    HWND hwnd = hwndOf(w);
    if (hwnd == nullptr) {
        setError(error, QCoreApplication::translate("OverlayWin", "窗口为空，无法应用浮层属性。"));
        return false;
    }

    // 只有 GWL_EXSTYLE 为 0 且确实出错才算失败（extended style 为 0 理论上合法，不能只判返回值）。
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        setError(error, errorText(QCoreApplication::translate("OverlayWin", "读取窗口扩展样式失败"),
                                  ::GetLastError()));
        return false;
    }

    bool ok = true;

    // ① 先给「录屏排除」拍一张快照。
    //    改 GWL_EXSTYLE 在部分系统/窗口状态下会连带把 display affinity 清成 WDA_NONE，
    //    以前就是这里把锁定时的 WDA_EXCLUDEFROMCAPTURE 弄丢的（浮层直接进了录屏）。
    //    所以 affinity 在这里一律当外部状态：不要求改就原样保留，需要时按快照回填。
    DWORD previousAffinity = WDA_NONE;
    const bool haveAffinity = ::GetWindowDisplayAffinity(hwnd, &previousAffinity) != FALSE;

    // ② 只改扩展样式里跟穿透有关的那两位（TOOLWINDOW/NOACTIVATE 恒定带上）。
    // 直接改扩展样式，不走 QWindow::setFlags()：
    // setFlags 会 destroy + recreate 原生窗口，QML 场景、渲染后端、已建好的纹理全丢，
    // 浮层会变成一块白板。
    LONG_PTR wanted = previous | kAlwaysOverlayExStyles;
    if (clickThrough) {
        wanted |= kClickThroughExStyles;
    } else {
        wanted &= ~kClickThroughExStyles;
    }

    if (wanted != previous) {
        ::SetLastError(ERROR_SUCCESS);
        if (::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted) == 0
            && ::GetLastError() != ERROR_SUCCESS) {
            setError(error, errorText(QCoreApplication::translate("OverlayWin", "设置浮层属性失败"),
                                      ::GetLastError()));
            ok = false;   // 继续往下做：截图排除和"显示但不激活"仍然是能救回来的部分
        }
    }

    // SW_SHOWNOACTIVATE：显示，但不激活、不抢前台。
    // （配合 WS_EX_NOACTIVATE；只靠 ShowWindow 不够，用户点一下还是会激活。）
    // 只在该显示的窗口上做 —— 以前无条件 ShowWindow，把 QML 里 visible=false 的
    // 浮层/控制条硬生生弹了出来（打开软件就看见一个 160x160 的空框）。
    // 显隐一律由 QML 的 visible 决定，这里只负责样式。
    if (w->isVisible())
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    // ③ affinity：要排除就设 WDA_EXCLUDEFROMCAPTURE；没要求就**保持/回填**快照值。
    //    写入前先读一次当前值，值没变就不写（一次 SetWindowDisplayAffinity 会打散
    //    正在进行的抓屏，能省则省）。
    DWORD wantedAffinity = previousAffinity;
    bool haveWantedAffinity = haveAffinity;
    if (excludeFromCapture) {
        wantedAffinity = WDA_EXCLUDEFROMCAPTURE;
        haveWantedAffinity = true;
    }

    if (haveWantedAffinity) {
        DWORD currentAffinity = WDA_NONE;
        const bool haveCurrent = ::GetWindowDisplayAffinity(hwnd, &currentAffinity) != FALSE;
        if (!haveCurrent || currentAffinity != wantedAffinity) {
            if (::SetWindowDisplayAffinity(hwnd, wantedAffinity) == FALSE) {
                const DWORD code = ::GetLastError();
                // 非致命：老系统不认这个 affinity，浮层照常工作，只是可能被截进自己的截图里。
                // 保留第一个失败的文案，别让后一步把更重要的错误覆盖掉。
                if (ok) {
                    setError(error,
                             excludeFromCapture
                                 ? QCoreApplication::translate(
                                       "OverlayWin",
                                       "设置截图排除失败：当前系统可能低于 Windows 10 2004，"
                                       "浮层可能被截进画面（错误码 %1）")
                                       .arg(QString::number(static_cast<qulonglong>(code)))
                                 : QCoreApplication::translate(
                                       "OverlayWin",
                                       "恢复截图排除状态失败，浮层可能被录进屏幕（错误码 %1）")
                                       .arg(QString::number(static_cast<qulonglong>(code))));
                }
                ok = false;
            }
        }
    }

    return ok;
#else
    Q_UNUSED(w);
    Q_UNUSED(clickThrough);
    Q_UNUSED(excludeFromCapture);
    setError(error, QCoreApplication::translate("OverlayWin", "当前平台尚未实现浮层。"));
    return false;
#endif
}

bool winPlaceOverlay(QWindow *w, const QRect &globalRectPhysical, QString *error)
{
#if defined(Q_OS_WIN)
    HWND hwnd = hwndOf(w);
    if (hwnd == nullptr) {
        setError(error, QCoreApplication::translate("OverlayWin", "窗口为空，无法放置浮层。"));
        return false;
    }
    if (globalRectPhysical.isEmpty()) {
        // 传 0 宽/0 高给 SetWindowPos 会把窗口设成不可见尺寸，后面很难察觉是这里错了。
        setError(error,
                 QCoreApplication::translate("OverlayWin", "目标矩形为空，无法放置浮层。"));
        return false;
    }

    // globalRectPhysical 是**物理像素**（调用方按 devicePixelRatio 换算过），
    // 这里直接喂给 SetWindowPos —— 它吃的就是物理像素，中间不再乘任何 DPR。
    // 所以**不要**再调 w->setGeometry()：那是逻辑坐标，等于把 DPR 又乘一遍，
    // 150% 缩放下浮层会偏出去、尺寸也会大一圈。
    // 前提：进程已声明 per-monitor DPI 感知（IPlatform::initProcessDpi），
    //       否则 Windows 会替我们做一次虚拟化缩放，坐标照样对不上。
    //
    // 绝不带 SWP_SHOWWINDOW：那是「摆放」在替 QML 做「显示」的决定。
    // 以前带上它，Idle 之后任何一次重摆（换选区、切屏、回前台）都会把
    // visible=false 的浮层重新弹出来，用户关不掉。可见性只归 QML 的 visible 管：
    // 隐藏窗口照样能移动/改尺寸（只是不会因此变可见），下次 show 时位置就是对的。
    const UINT flags = SWP_NOACTIVATE;
    if (::SetWindowPos(hwnd,
                       HWND_TOPMOST,
                       globalRectPhysical.x(),
                       globalRectPhysical.y(),
                       globalRectPhysical.width(),
                       globalRectPhysical.height(),
                       flags)
        == FALSE) {
        setError(error, errorText(QCoreApplication::translate("OverlayWin", "放置浮层失败"),
                                  ::GetLastError()));
        return false;
    }
    return true;
#else
    Q_UNUSED(w);
    Q_UNUSED(globalRectPhysical);
    setError(error, QCoreApplication::translate("OverlayWin", "当前平台尚未实现浮层。"));
    return false;
#endif
}

void winClearOverlayFlags(QWindow *w)
{
#if defined(Q_OS_WIN)
    HWND hwnd = hwndOf(w);
    if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) {
        return;   // 窗口已经在销毁了，没什么可清的
    }

    // 先把截图排除摘掉再改样式：顺序反过来的话，中间那几个毫秒的窗口状态没意义，
    // 但 affinity 失败不该拦住样式回滚。
    if (::SetWindowDisplayAffinity(hwnd, WDA_NONE) == FALSE) {
        qWarning("[overlay] 清除截图排除失败：%lu",
                 static_cast<unsigned long>(::GetLastError()));
    }

    // 读样式同样按「值为 0 且确实出错才算失败」判 —— 与 winApplyOverlayFlags 保持一致。
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (previous == 0 && ::GetLastError() != ERROR_SUCCESS) {
        qWarning("[overlay] 读取窗口扩展样式失败（回滚跳过）：%lu",
                 static_cast<unsigned long>(::GetLastError()));
        return;
    }

    const LONG_PTR wanted =
        previous & ~(kClickThroughExStyles | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    if (wanted != previous) {
        ::SetLastError(ERROR_SUCCESS);
        if (::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted) == 0
            && ::GetLastError() != ERROR_SUCCESS) {
            qWarning("[overlay] 回滚浮层扩展样式失败：%lu",
                     static_cast<unsigned long>(::GetLastError()));
        }
    }
#else
    Q_UNUSED(w);
#endif
}

} // namespace studious::platform
