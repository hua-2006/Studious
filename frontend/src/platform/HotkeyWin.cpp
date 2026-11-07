#include "platform/HotkeyWin.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QStringList>

#if defined(Q_OS_WIN)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>

// mingw-w64 老版本 SDK 可能没带上这条，补一个固定值（WinError.h 里就是 1409）。
#    ifndef ERROR_HOTKEY_ALREADY_REGISTERED
#        define ERROR_HOTKEY_ALREADY_REGISTERED 1409
#    endif

// Win7 起才有：长按热键时系统不按键盘重复率连发 WM_HOTKEY。值是稳定的 ABI。
#    ifndef MOD_NOREPEAT
#        define MOD_NOREPEAT 0x4000
#    endif
#endif

namespace studious::platform {

#if defined(Q_OS_WIN)

namespace {

// 和 Desktop.cpp 的 L"StudiousTrayWindow" 各自一个类，互不冲突。
const wchar_t *const kHotkeyClass = L"StudiousHotkeyWindow";

// RegisterHotKey 的系统 id 上限：0xC000 以上留给 atom 形式，用满就是实现 bug 了。
constexpr int kMaxSystemId = 0xBFFF;

// ---- 键名 <-> VK -----------------------------------------------------------

struct NamedKey {
    const char *name;     // 匹配用，全大写
    const char *display;  // 给人看的写法
    UINT vk;
};

// 顺序即规范名：同一个 VK 的别名排在后面，反查时取第一个就是回显用的写法。
const NamedKey kNamedKeys[] = {
    { "SPACE", "Space", VK_SPACE },
    { "ENTER", "Enter", VK_RETURN },
    { "RETURN", "Enter", VK_RETURN },
    { "ESC", "Esc", VK_ESCAPE },
    { "ESCAPE", "Esc", VK_ESCAPE },
    { "TAB", "Tab", VK_TAB },
    { "LEFT", "Left", VK_LEFT },
    { "RIGHT", "Right", VK_RIGHT },
    { "UP", "Up", VK_UP },
    { "DOWN", "Down", VK_DOWN },
    { "HOME", "Home", VK_HOME },
    { "END", "End", VK_END },
    { "PAGEUP", "PageUp", VK_PRIOR },
    { "PGUP", "PageUp", VK_PRIOR },
    { "PAGEDOWN", "PageDown", VK_NEXT },
    { "PGDN", "PageDown", VK_NEXT },
    { "INSERT", "Insert", VK_INSERT },
    { "INS", "Insert", VK_INSERT },
    { "DELETE", "Delete", VK_DELETE },
    { "DEL", "Delete", VK_DELETE },
    { "PRINTSCREEN", "PrintScreen", VK_SNAPSHOT },
    { "PRTSC", "PrintScreen", VK_SNAPSHOT },
    { "SNAPSHOT", "PrintScreen", VK_SNAPSHOT },
};

bool modifierFromName(const QString &token, UINT *flag)
{
    const QString name = token.toUpper();
    if (name == QLatin1String("CTRL") || name == QLatin1String("CONTROL")) {
        *flag = MOD_CONTROL;
        return true;
    }
    if (name == QLatin1String("ALT")) {
        *flag = MOD_ALT;
        return true;
    }
    if (name == QLatin1String("SHIFT")) {
        *flag = MOD_SHIFT;
        return true;
    }
    if (name == QLatin1String("WIN") || name == QLatin1String("META")) {
        *flag = MOD_WIN;
        return true;
    }
    return false;
}

bool keyFromName(const QString &token, UINT *vk, QString *display)
{
    const QString name = token.toUpper();

    if (name.size() == 1) {
        const QChar ch = name.at(0);
        // A-Z / 0-9 的 VK 值正好等于 ASCII 码。
        if ((ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            *vk = UINT(ch.unicode());
            *display = name;
            return true;
        }
        return false;
    }

    if (name.size() >= 2 && name.at(0) == QLatin1Char('F')) {
        bool ok = false;
        const int number = name.mid(1).toInt(&ok);
        if (ok && number >= 1 && number <= 24) {
            *vk = UINT(VK_F1 + number - 1);
            *display = QStringLiteral("F%1").arg(number);
            return true;
        }
        // F0 / F25 / F1x 落到下面当未知键处理。
    }

    for (const NamedKey &entry : kNamedKeys) {
        if (name == QLatin1String(entry.name)) {
            *vk = entry.vk;
            *display = QString::fromLatin1(entry.display);
            return true;
        }
    }
    return false;
}

QString keyNameFromVk(UINT vk)
{
    // 字母/数字的 VK 值就是 ASCII：winuser.h 只给了 VK_0..VK_9，A-Z 得自己写。
    if ((vk >= UINT('A') && vk <= UINT('Z')) || (vk >= UINT('0') && vk <= UINT('9'))) {
        return QString(QChar(ushort(vk)));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return QStringLiteral("F%1").arg(int(vk - VK_F1) + 1);
    }
    for (const NamedKey &entry : kNamedKeys) {
        if (entry.vk == vk) {
            return QString::fromLatin1(entry.display);
        }
    }
    QString hex = QString::number(uint(vk), 16).toUpper();
    if (hex.size() < 2) {
        hex.prepend(QLatin1Char('0'));
    }
    return QStringLiteral("VK-") + hex;
}

// 裸键（没有 Ctrl/Alt/Shift/Win）能不能注册？
// 一般不能：RegisterHotKey 是**全局**抢占，注册一个裸 "S" 等于让全系统再也打不出 s，
// 裸 F1 则会让所有程序的帮助键失效。唯一放行的是 F13~F24 —— 普通键盘上没有这些键，
// 只有扩展键盘/宏键盘才发得出来，抢走它们不会误伤正常输入。
bool bareKeyAllowed(UINT vk)
{
    return vk >= VK_F13 && vk <= VK_F24;
}

// 用户能读的组合键回显，例如 "Ctrl+Alt+S"。冲突提示里用它。
QString comboDisplay(UINT modifiers, UINT vk)
{
    QStringList parts;
    if ((modifiers & MOD_CONTROL) != 0) {
        parts << QStringLiteral("Ctrl");
    }
    if ((modifiers & MOD_ALT) != 0) {
        parts << QStringLiteral("Alt");
    }
    if ((modifiers & MOD_SHIFT) != 0) {
        parts << QStringLiteral("Shift");
    }
    if ((modifiers & MOD_WIN) != 0) {
        parts << QStringLiteral("Win");
    }
    parts << keyNameFromVk(vk);
    return parts.join(QLatin1Char('+'));
}

// ---- 两种入参形式 ---------------------------------------------------------

// Qt::KeyboardModifiers 与 Win32 MOD_* 两组位不重叠，混着给也认。
UINT winModifiersFromMask(int mask)
{
    UINT flags = 0;
    if ((mask & int(Qt::ControlModifier)) != 0) {
        flags |= MOD_CONTROL;
    }
    if ((mask & int(Qt::AltModifier)) != 0) {
        flags |= MOD_ALT;
    }
    if ((mask & int(Qt::ShiftModifier)) != 0) {
        flags |= MOD_SHIFT;
    }
    if ((mask & int(Qt::MetaModifier)) != 0) {
        flags |= MOD_WIN;
    }
    if ((mask & MOD_CONTROL) != 0) {
        flags |= MOD_CONTROL;
    }
    if ((mask & MOD_ALT) != 0) {
        flags |= MOD_ALT;
    }
    if ((mask & MOD_SHIFT) != 0) {
        flags |= MOD_SHIFT;
    }
    if ((mask & MOD_WIN) != 0) {
        flags |= MOD_WIN;
    }
    return flags;
}

// Qt::Key -> VK。认不出来返回 0。
UINT vkFromQtKey(int key)
{
    if (key >= int(Qt::Key_A) && key <= int(Qt::Key_Z)) {
        return UINT(key);   // Qt::Key_A == VK_A
    }
    if (key >= int(Qt::Key_0) && key <= int(Qt::Key_9)) {
        return UINT(key);   // Qt::Key_0 == VK_0
    }
    if (key >= int(Qt::Key_F1) && key <= int(Qt::Key_F24)) {
        return UINT(VK_F1 + (key - int(Qt::Key_F1)));
    }

    switch (key) {
    case Qt::Key_Space:    return VK_SPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter:    return VK_RETURN;
    case Qt::Key_Escape:   return VK_ESCAPE;
    case Qt::Key_Tab:      return VK_TAB;
    case Qt::Key_Left:     return VK_LEFT;
    case Qt::Key_Right:    return VK_RIGHT;
    case Qt::Key_Up:       return VK_UP;
    case Qt::Key_Down:     return VK_DOWN;
    case Qt::Key_Home:     return VK_HOME;
    case Qt::Key_End:      return VK_END;
    case Qt::Key_PageUp:   return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Insert:   return VK_INSERT;
    case Qt::Key_Delete:   return VK_DELETE;
    case Qt::Key_Print:    return VK_SNAPSHOT;
    default:               break;
    }

    // 调用方直接塞虚拟键码（1..0xFF）也放行。
    if (key > 0 && key <= 0xFF) {
        return UINT(key);
    }
    return 0;
}

// 文本式："Ctrl+Alt+S"。失败时 *error 指明是哪一段不认识。
bool parseComboText(const QString &text, UINT *modifiers, UINT *vk, QString *display,
                    QString *error)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("HotkeyWin", "组合键为空");
        }
        return false;
    }

    const QStringList parts = trimmed.split(QLatin1Char('+'));
    UINT mods = 0;

    for (int i = 0; i < parts.size(); ++i) {
        const QString token = parts.at(i).trimmed();
        const bool isLast = (i == parts.size() - 1);

        if (token.isEmpty()) {
            if (error != nullptr) {
                *error = QCoreApplication::translate("HotkeyWin", "组合键多写了一个「+」");
            }
            return false;
        }

        if (!isLast) {
            UINT flag = 0;
            if (!modifierFromName(token, &flag)) {
                if (error != nullptr) {
                    *error = QCoreApplication::translate("HotkeyWin", "无法识别的修饰键：%1")
                                 .arg(token);
                }
                return false;
            }
            mods |= flag;
            continue;
        }

        // 最后一段必须是主键。
        UINT keyVk = 0;
        QString keyName;
        if (keyFromName(token, &keyVk, &keyName)) {
            *modifiers = mods;
            *vk = keyVk;
            if (display != nullptr) {
                *display = comboDisplay(mods, keyVk);
            }
            return true;
        }

        UINT flag = 0;
        if (modifierFromName(token, &flag)) {
            if (error != nullptr) {
                *error = QCoreApplication::translate("HotkeyWin",
                                                     "组合键缺少主键：%1 只是修饰键")
                             .arg(token);
            }
            return false;
        }

        if (error != nullptr) {
            *error = QCoreApplication::translate("HotkeyWin", "无法识别的按键：%1").arg(token);
        }
        return false;
    }

    return false;   // 循环里最后一段必然 return，这里只是给编译器一个出口。
}

// ---- 窗口 ---------------------------------------------------------------

HotkeyWin *hotkeyOf(HWND window)
{
    // 0 有两种含义：还没设过（初始态）与读取失败。用 LastError 区分，
    // 别把「读失败」当成「没设过」继续往下走。
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR user = ::GetWindowLongPtrW(window, GWLP_USERDATA);
    if (user == 0 && ::GetLastError() != ERROR_SUCCESS) {
        return nullptr;
    }
    return reinterpret_cast<HotkeyWin *>(user);
}

LRESULT CALLBACK hotkeyWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_HOTKEY) {
        HotkeyWin *self = hotkeyOf(window);
        if (self != nullptr) {
            // 窗口过程里只排队：activated 的槽可能反手 registerHotkey/unregisterAll，
            // 在系统消息里同步跑会重入。
            QMetaObject::invokeMethod(self,
                                      "dispatchHotkey",
                                      Qt::QueuedConnection,
                                      Q_ARG(int, static_cast<int>(wparam)));
        }
        return 0;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

bool registerHotkeyClass()
{
    static bool registered = false;
    if (registered) {
        return true;
    }

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = hotkeyWndProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpszClassName = kHotkeyClass;

    if (::RegisterClassExW(&cls) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    registered = true;
    return true;
}

} // namespace

HotkeyWin::HotkeyWin(QObject *parent)
    : QObject(parent)
{
    // 窗口懒创建：没注册过热键的进程不该多一个 HWND。
}

HotkeyWin::~HotkeyWin()
{
    unregisterAll();
    destroyMessageWindow();
}

bool HotkeyWin::createMessageWindow()
{
    if (m_hwnd != nullptr) {
        return true;
    }
    if (!registerHotkeyClass()) {
        return false;
    }

    HWND window = ::CreateWindowExW(0,
                                    kHotkeyClass,
                                    L"",
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

    // SetWindowLongPtrW 返回旧值；GWLP_USERDATA 的旧值本来就是 0，只能靠 LastError 判成败。
    ::SetLastError(ERROR_SUCCESS);
    if (::SetWindowLongPtrW(window, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(this)) == 0
        && ::GetLastError() != ERROR_SUCCESS) {
        // 挂不上 this，WM_HOTKEY 就没人接：拆窗口按失败上报，别留个哑句柄。
        ::DestroyWindow(window);
        return false;
    }
    m_hwnd = window;
    return true;
}

void HotkeyWin::destroyMessageWindow()
{
    if (m_hwnd != nullptr) {
        ::DestroyWindow(static_cast<HWND>(m_hwnd));
        m_hwnd = nullptr;
    }
}

bool HotkeyWin::registerHotkey(const HotkeySpec &spec, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const QString id = spec.id.trimmed();
    if (id.isEmpty()) {
        return fail(QCoreApplication::translate("HotkeyWin", "热键 id 不能为空"));
    }
    if (m_ids.contains(id)) {
        return fail(QCoreApplication::translate("HotkeyWin", "热键 id 重复：%1").arg(id));
    }

    UINT modifiers = 0;
    UINT vk = 0;
    QString display;

    if (spec.key != 0) {
        modifiers = winModifiersFromMask(spec.modifiers);
        vk = vkFromQtKey(spec.key);
        if (vk == 0) {
            return fail(QCoreApplication::translate("HotkeyWin", "无法识别的按键代码：%1")
                            .arg(spec.key));
        }
        display = comboDisplay(modifiers, vk);
    } else {
        QString parseError;
        if (!parseComboText(spec.description, &modifiers, &vk, &display, &parseError)) {
            return fail(parseError);
        }
    }

    // 裸键直接拒掉：解析层只保证「认得出这个键」，不保证它适合做全局热键。
    // 两条入参路径（数值式 Qt::Key / 文本式 "Ctrl+Alt+S"）都在这里收口，
    // 免得 S 或 F1 这种单键在系统里注册成功、用户从此打不出那个字符。
    if (modifiers == 0 && !bareKeyAllowed(vk)) {
        return fail(QCoreApplication::translate(
                        "HotkeyWin",
                        "热键 %1 至少要带一个修饰键（Ctrl / Alt / Shift / Win）；"
                        "单独的字母、数字或功能键会抢走全系统的输入（F13~F24 除外）")
                        .arg(display));
    }

    const bool hadWindow = (m_hwnd != nullptr);
    if (!createMessageWindow()) {
        return fail(QCoreApplication::translate("HotkeyWin", "无法创建热键消息窗口"));
    }

    // 系统 id 从 1 开始递增，跳过已被占用的槽位。
    int systemId = m_nextSystemId;
    int guard = 0;
    while (m_bySystemId.contains(systemId)) {
        if (++systemId > kMaxSystemId) {
            systemId = 1;
        }
        if (++guard > kMaxSystemId) {
            if (!hadWindow) {
                destroyMessageWindow();
            }
            return fail(QCoreApplication::translate("HotkeyWin", "热键槽位已用尽"));
        }
    }

    // MOD_NOREPEAT：不带它，长按热键时系统按键盘重复率连发 WM_HOTKEY，
    // 一次长按能触发几十次「截图」。只在注册时加，modifiers 本身保持干净
    // （comboDisplay 与错误文案用的还是用户写的那个组合键）。
    const UINT registerModifiers = modifiers | MOD_NOREPEAT;
    if (::RegisterHotKey(static_cast<HWND>(m_hwnd), systemId, registerModifiers, vk) == FALSE) {
        const DWORD code = ::GetLastError();
        // 失败不留垃圾：窗口是这次才建的、表里也没别人，就一并拆掉。
        if (!hadWindow && m_ids.isEmpty()) {
            destroyMessageWindow();
        }
        if (code == ERROR_HOTKEY_ALREADY_REGISTERED) {
            return fail(QCoreApplication::translate("HotkeyWin", "热键 %1 已被其它程序占用")
                            .arg(display));
        }
        return fail(QCoreApplication::translate("HotkeyWin", "注册热键 %1 失败（系统错误 %2）")
                        .arg(display)
                        .arg(code));
    }

    m_ids.insert(id, systemId);
    m_bySystemId.insert(systemId, id);
    m_nextSystemId = (systemId >= kMaxSystemId) ? 1 : systemId + 1;
    return true;
}

void HotkeyWin::unregisterHotkey(const QString &id)
{
    const auto it = m_ids.find(id);
    if (it == m_ids.end()) {
        return;
    }

    const int systemId = it.value();
    if (m_hwnd != nullptr) {
        ::UnregisterHotKey(static_cast<HWND>(m_hwnd), systemId);
    }
    m_ids.erase(it);
    m_bySystemId.remove(systemId);
}

void HotkeyWin::unregisterAll()
{
    for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it) {
        if (m_hwnd != nullptr) {
            ::UnregisterHotKey(static_cast<HWND>(m_hwnd), it.value());
        }
    }
    m_ids.clear();
    m_bySystemId.clear();
    // 窗口留给下一次注册复用；真正 DestroyWindow 在析构里。
}

bool HotkeyWin::isRegistered(const QString &id) const
{
    return m_ids.contains(id);
}

void HotkeyWin::dispatchHotkey(int systemId)
{
    const QString id = m_bySystemId.value(systemId);
    if (!id.isEmpty()) {
        emit activated(id);
    }
}

#else   // !Q_OS_WIN

HotkeyWin::HotkeyWin(QObject *parent)
    : QObject(parent)
{
    // 其它平台（X11 / Wayland）尚未实现，没有消息窗口可用。
}

HotkeyWin::~HotkeyWin() = default;

bool HotkeyWin::createMessageWindow()
{
    return false;
}

void HotkeyWin::destroyMessageWindow()
{
}

bool HotkeyWin::registerHotkey(const HotkeySpec &spec, QString *error)
{
    Q_UNUSED(spec);
    if (error != nullptr) {
        *error = QCoreApplication::translate("HotkeyWin", "当前平台尚未实现全局热键");
    }
    return false;
}

void HotkeyWin::unregisterHotkey(const QString &id)
{
    Q_UNUSED(id);
}

void HotkeyWin::unregisterAll()
{
}

bool HotkeyWin::isRegistered(const QString &id) const
{
    Q_UNUSED(id);
    return false;
}

void HotkeyWin::dispatchHotkey(int systemId)
{
    Q_UNUSED(systemId);
}

#endif   // Q_OS_WIN

} // namespace studious::platform
