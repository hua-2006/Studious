#include "platform/Desktop.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>


#include <cstring>

#ifdef Q_OS_WIN
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <shellapi.h>
#endif

namespace studious::platform {

#ifndef Q_OS_WIN
namespace {

// XDG autostart：$XDG_CONFIG_HOME/autostart/studious.desktop（默认 ~/.config/autostart）。
// GNOME / KDE / XFCE / Cinnamon 都读这个目录，不需要任何平台头。
QString xdgAutoStartFile()
{
    const QString configHome =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(configHome).filePath(QStringLiteral("autostart/studious.desktop"));
}

} // namespace
#endif

#ifdef Q_OS_WIN
namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 101;
constexpr UINT kMenuShow = 1;
constexpr UINT kMenuQuit = 2;
constexpr int kTrayIconSize = 32;

const wchar_t *const kTrayClass = L"StudiousTrayWindow";

// 从 Qt 资源里的 PNG 造一个 HICON。
// 走 DIB 而不是 LoadImage：PNG 不是 .ico，LoadImage 只认 exe 里嵌的图标资源。
HICON iconFromResource(const QString &path, int size)
{
    QImage source(path);
    if (source.isNull()) {
        return nullptr;
    }

    QImage image = source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) {
        return nullptr;
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = image.width();
    header.bV5Height = -image.height();   // 负高度 = top-down，和 QImage 的行序一致
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void *bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    // 分开判：先判空是为了拿错误码，再判断 bits —— 只判后者的话 color 已经建出来
    // 却直接 return，HBITMAP 就漏了（GDI 对象上限 10000，切几次主题就能耗掉一截）。
    if (color == nullptr) {
        return nullptr;
    }
    if (bits == nullptr) {
        ::DeleteObject(color);
        return nullptr;
    }

    // QImage 的 Format_ARGB32 在小端上是 BGRA 排列，正好对上上面的掩码。
    const qsizetype bytes = qsizetype(image.width()) * image.height() * 4;
    std::memcpy(bits, image.constBits(), static_cast<size_t>(bytes));

    // 单色掩码图。建失败就没法 CreateIconIndirect（info.hbmMask 为空会直接失败），
    // 这里把已经建好的彩色位图回收掉再返回，别把半个图标漏在 GDI 堆里。
    HBITMAP mask = ::CreateBitmap(image.width(), image.height(), 1, 1, nullptr);
    if (mask == nullptr) {
        ::DeleteObject(color);
        return nullptr;
    }

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&info);

    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;   // CreateIconIndirect 失败返回 nullptr，句柄此时都已回收
}

Desktop *desktopOf(HWND window)
{
    // 0 既可能是「没设过」也可能是读取失败；用 LastError 区分，
    // 免得读失败时让调用方拿着 nullptr 继续走（那样托盘点击就静默失效了）。
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR user = ::GetWindowLongPtrW(window, GWLP_USERDATA);
    if (user == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return nullptr;
    }
    return reinterpret_cast<Desktop *>(user);
}

void showTrayMenu(HWND window, Desktop *desktop)
{
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    // 用 QCoreApplication::translate 而不是后端的 i18n::s：平台层不依赖 backend 库，
    // 文案统一走 .ts（Desktop 上下文），跟 QML 的 qsTr 吃同一份翻译。
    const QString showText = QCoreApplication::translate("Desktop", "显示主窗口");
    const QString quitText = QCoreApplication::translate("Desktop", "退出");

    ::AppendMenuW(menu, MF_STRING, kMenuShow, reinterpret_cast<LPCWSTR>(showText.utf16()));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuQuit, reinterpret_cast<LPCWSTR>(quitText.utf16()));

    POINT cursor{};
    ::GetCursorPos(&cursor);
    // 不先抢前台，菜单点外面不会自动消失（Win32 托盘菜单的经典坑）。
    ::SetForegroundWindow(window);
    const UINT command = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    ::PostMessageW(window, WM_NULL, 0, 0);
    ::DestroyMenu(menu);

    if (command == kMenuShow && desktop != nullptr) {
        emit desktop->restoreRequested();
    } else if (command == kMenuQuit && desktop != nullptr) {
        emit desktop->quitRequested();
    }
}

// 托盘图标的鼠标消息序列（Windows 判定双击时）：
//   WM_LBUTTONUP(第 1 下松开) → WM_LBUTTONDBLCLK(第 2 下按下) → WM_LBUTTONUP(第 2 下松开)
// 三个事件都发 restoreRequested 的话，一次双击会连发两次以上，主窗口在
// 「显示 / 收起」之间抖。这里用两个时刻去重（进程内只有一个托盘窗口，用静态就够）：
//   · g_lastRestoreTick —— 上一次真正发过 restoreRequested 的时刻；
//   · g_swallowUpTick   —— 刚处理过 DBLCLK，紧跟着的那个 UP 要吞掉。
ULONGLONG g_lastRestoreTick = 0;
ULONGLONG g_swallowUpTick = 0;

inline ULONGLONG doubleClickMillis()
{
    // GetDoubleClickTime() 是控制面板里用户可调的（默认 500ms）。
    return static_cast<ULONGLONG>(::GetDoubleClickTime());
}

LRESULT CALLBACK trayWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == kTrayCallbackMessage) {
        Desktop *desktop = desktopOf(window);
        const UINT event = LOWORD(lparam);
        const ULONGLONG now = ::GetTickCount64();

        if (event == WM_LBUTTONDBLCLK) {
            // 第 1 下的 UP 已经发过一次「显示」了；只有当它确实没发（时间戳空或过期）
            // 才在这里补一次，否则一次双击就是两次 restoreRequested。
            const bool alreadyRestored =
                (g_lastRestoreTick != 0) && (now - g_lastRestoreTick <= doubleClickMillis());
            g_swallowUpTick = now;
            if (!alreadyRestored && desktop != nullptr) {
                emit desktop->restoreRequested();
            }
            g_lastRestoreTick = now;
            return 0;
        }

        if (event == WM_LBUTTONUP) {
            // 双击尾随的那个 UP：同一次手势里已经恢复过了，吞掉（只吞一次）。
            if (g_swallowUpTick != 0 && (now - g_swallowUpTick) <= doubleClickMillis()) {
                g_swallowUpTick = 0;
                return 0;
            }
            g_lastRestoreTick = now;
            if (desktop != nullptr) {
                emit desktop->restoreRequested();
            }
            return 0;
        }

        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            showTrayMenu(window, desktop);
        }
        return 0;
    }

    if (message == WM_COMMAND) {
        return 0;   // 菜单命令在 TrackPopupMenu 的返回值里处理，这里只是吞掉
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

bool registerTrayClass()
{
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = trayWndProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpszClassName = kTrayClass;

    if (::RegisterClassExW(&cls) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    registered = true;
    return true;
}

} // namespace
#endif   // Q_OS_WIN

Desktop::Desktop(QObject *parent)
    : QObject(parent)
{
}

Desktop::~Desktop()
{
    shutdown();
}

void Desktop::setTrayEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    // 托盘图标一律先建出来（建的时候就是隐藏态），需要时再靠 NIS_HIDDEN 开关显隐。
    // 这样「能装托盘吗」是个确定的事实，而不是"用到了才知道"。
    if (!createTray()) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(m_trayWindow);
    data.uID = 1;
    data.uFlags = NIF_STATE;
    data.dwStateMask = NIS_HIDDEN;
    data.dwState = enabled ? 0 : NIS_HIDDEN;
    const BOOL ok = ::Shell_NotifyIconW(NIM_MODIFY, &data);
    qInfo("[tray] %s -> %d", enabled ? "show" : "hide", int(ok));
#else
    Q_UNUSED(enabled);
#endif
}

bool Desktop::trayAvailable()
{
    // 真正去建一次：Explorer 没起来 / 被策略禁用的会话里 NIM_ADD 会失败，
    // 这时候界面该把托盘相关开关置灰，而不是让人开了却没反应。
    return createTray() && m_trayReady;
}

bool Desktop::createTray()
{
#ifdef Q_OS_WIN
    if (m_trayReady) {
        return true;
    }
    if (!registerTrayClass()) {
        return false;
    }

    HWND window = ::CreateWindowExW(0,
                                    kTrayClass,
                                    L"Studious",
                                    0,
                                    0,
                                    0,
                                    0,
                                    0,
                                    HWND_MESSAGE,
                                    nullptr,
                                    ::GetModuleHandleW(nullptr),
                                    this);
    if (window == nullptr) {
        return false;
    }
    // SetWindowLongPtrW 返回的是**旧值**，设 GWLP_USERDATA 时旧值本来就是 0，
    // 所以必须靠 LastError 判成败，不能看返回值。
    ::SetLastError(ERROR_SUCCESS);
    if (::SetWindowLongPtrW(window, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(this)) == 0
        && ::GetLastError() != ERROR_SUCCESS) {
        // 挂不上 this，托盘消息就找不到 Desktop：拆掉窗口，别留一个哑的。
        ::DestroyWindow(window);
        return false;
    }
    m_trayWindow = window;

    HICON icon = iconFromResource(QStringLiteral(":/qt/qml/Studious/resources/brand.png"),
                                  kTrayIconSize);
    if (icon == nullptr) {
        ::DestroyWindow(window);
        m_trayWindow = nullptr;
        return false;
    }
    m_trayIcon = icon;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    // NIF_STATE + NIS_HIDDEN：先藏着。真正要显示时由 setTrayEnabled(true) 取消隐藏。
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_STATE;
    data.dwState = NIS_HIDDEN;
    data.dwStateMask = NIS_HIDDEN;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = icon;
    const QString tip = QStringLiteral("Studious");
    ::lstrcpynW(data.szTip, reinterpret_cast<LPCWSTR>(tip.utf16()), int(std::size(data.szTip)));

    m_trayReady = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (!m_trayReady) {
        ::DestroyIcon(icon);
        m_trayIcon = nullptr;
        ::DestroyWindow(window);
        m_trayWindow = nullptr;
    }
    return m_trayReady;
#else
    // 其它平台暂不实现托盘：界面会把对应开关置灰。
    return false;
#endif
}

void Desktop::destroyTray()
{
#ifdef Q_OS_WIN
    if (m_trayWindow != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = static_cast<HWND>(m_trayWindow);
        data.uID = 1;
        ::Shell_NotifyIconW(NIM_DELETE, &data);
        ::DestroyWindow(static_cast<HWND>(m_trayWindow));
        m_trayWindow = nullptr;
    }
    if (m_trayIcon != nullptr) {
        ::DestroyIcon(static_cast<HICON>(m_trayIcon));
        m_trayIcon = nullptr;
    }
#endif
    m_trayReady = false;
}

bool Desktop::setAutoStart(bool enabled, QString *error)
{
#ifdef Q_OS_WIN
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (enabled && !QFileInfo::exists(exePath)) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("Desktop",
                                                 "找不到程序自身路径，无法设置开机自启。");
        }
        return false;
    }

    const QString runKey = QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QSettings store(runKey, QSettings::NativeFormat);

    if (enabled) {
        store.setValue(QStringLiteral("Studious"), QStringLiteral("\"%1\"").arg(exePath));
    } else {
        store.remove(QStringLiteral("Studious"));
    }
    store.sync();

    if (store.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = QCoreApplication::translate(
                "Desktop", "写入注册表启动项失败（可能被安全软件拦截）。");
        }
        return false;
    }
    return true;
#else
    // 类 Unix：走 freedesktop 的 XDG autostart（GNOME / KDE / XFCE 都认）。
    // 只写一个 .desktop 文件，不引任何平台头，也不需要守护进程。
    const QString exePath = QCoreApplication::applicationFilePath();
    if (enabled && !QFileInfo::exists(exePath)) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("Desktop",
                                                 "找不到程序自身路径，无法设置开机自启。");
        }
        return false;
    }

    const QString desktopFile = xdgAutoStartFile();
    if (!enabled) {
        if (QFileInfo::exists(desktopFile) && !QFile::remove(desktopFile)) {
            if (error != nullptr) {
                *error = QCoreApplication::translate("Desktop", "删除自启动项失败，请检查文件权限。");
            }
            return false;
        }
        return true;
    }

    if (!QDir().mkpath(QFileInfo(desktopFile).absolutePath())) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("Desktop", "创建自启动目录失败。");
        }
        return false;
    }

    QFile file(desktopFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("Desktop", "写入自启动项失败，请检查文件权限。");
        }
        return false;
    }
    // Exec 里的路径必须加引号：/opt/my apps/studious 之类带空格是常态。
    const QString contents =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Studious\n"
                       "Comment=Studious text & screenshot translation\n"
                       "Exec=\"%1\"\n"
                       "Terminal=false\n"
                       "X-GNOME-Autostart-enabled=true\n"
                       "Categories=Utility;\n")
            .arg(exePath);
    file.write(contents.toUtf8());
    file.close();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("Desktop", "写入自启动项失败，请检查文件权限。");
        }
        return false;
    }
    return true;
#endif
}

bool Desktop::autoStartRegistered()
{
#ifdef Q_OS_WIN
    const QString runKey = QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    QSettings store(runKey, QSettings::NativeFormat);
    return store.contains(QStringLiteral("Studious"));
#else
    return QFileInfo::exists(xdgAutoStartFile());
#endif
}

bool Desktop::startSingleInstance(const QString &name)
{
    if (m_instanceServer != nullptr) {
        return true;
    }

    auto *server = new QLocalServer(this);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name)) {
        // 名字被占用有两种可能：真有一个实例在跑（调用方应该先 ping 过），
        // 或者上次异常退出留下了死 socket。走到这里说明 ping 不通，
        // 那就当残留清掉再试一次 —— 反过来先 removeServer 会把活着的实例踢下线。
        QLocalServer::removeServer(name);
        if (!server->listen(name)) {
            delete server;
            return false;
        }
    }

    connect(server, &QLocalServer::newConnection, this, [this, server]() {
        while (QLocalSocket *socket = server->nextPendingConnection()) {
            socket->deleteLater();
            emit restoreRequested();
        }
    });

    m_instanceServer = server;
    return true;
}

bool Desktop::pingRunningInstance(const QString &name)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(600)) {
        return false;
    }
    socket.write("restore");
    socket.flush();
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

void Desktop::shutdown()
{
    destroyTray();
    if (m_instanceServer != nullptr) {
        m_instanceServer->close();
        QLocalServer::removeServer(m_instanceServer->fullServerName());
        m_instanceServer->deleteLater();
        m_instanceServer = nullptr;
    }
}

} // namespace studious::platform
