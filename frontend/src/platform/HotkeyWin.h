// 全局热键 — Windows 实现（RegisterHotKey + 消息专用窗口）。
//
// 铁律同 Types.h / IPlatform.h：本头文件里**不出现任何 Win32 类型**，
// HWND 一律以 void* 传递，真正的 windows.h 只在 HotkeyWin.cpp 里 include。
#pragma once

#include "platform/Types.h"

#include <QHash>
#include <QObject>
#include <QString>

namespace studious::platform {

// 全局热键。Windows: RegisterHotKey；其它平台: 全部返回 false + Unsupported 语义。
// 一个实例持有一个消息专用窗口，热键回调统一从该窗口的窗口过程转成 Qt 信号。
//
// 线程：全部方法都必须在 GUI 线程调用 —— WM_HOTKEY 投递到注册它的那个线程的消息队列，
// 窗口也只在这个线程上创建。
class HotkeyWin final : public QObject {
    Q_OBJECT
public:
    explicit HotkeyWin(QObject *parent = nullptr);
    ~HotkeyWin() override;

    // spec.id 为空或重复 → false。spec.modifiers/键名解析失败 → false 并写 *error。
    // 系统里已被别的程序占用（ERROR_HOTKEY_ALREADY_REGISTERED）→ false，
    // *error 写「热键 %1 已被其它程序占用」（含用户可读的组合键，如 Ctrl+Alt+S）。
    //
    // 组合键的两种给法（key 非 0 优先，两种都大小写不敏感、允许空格）：
    //   · 数值式：key != 0 时按 Qt::Key 解释（A-Z / 0-9 / F1-F24 / Space / Enter …），
    //     1..0xFF 的原始虚拟键码也认；modifiers 按 Qt::KeyboardModifiers 解释，
    //     同时兼容 Win32 的 MOD_* 位（两组位互不重叠，可以混着给）。
    //   · 文本式：key == 0 时把 description 当组合键文本解析，如 "Ctrl+Alt+S"。
    bool registerHotkey(const HotkeySpec &spec, QString *error);
    void unregisterHotkey(const QString &id);
    void unregisterAll();
    bool isRegistered(const QString &id) const;

signals:
    // id 是 registerHotkey 时给的那个字符串 id，不是一个序号。
    void activated(const QString &id);

private:
    // 私有实现细节（HWND / 表）用 void* 与 QHash，别把头文件污染成 Win32。
    bool createMessageWindow();    // 懒创建：第一次注册热键时才建
    void destroyMessageWindow();

    void *m_hwnd = nullptr;             // HWND，消息专用窗口（HWND_MESSAGE）
    QHash<QString, int> m_ids;          // id（字符串） -> 系统热键 id
    QHash<int, QString> m_bySystemId;   // 系统热键 id -> id，WM_HOTKEY 回查用
    int m_nextSystemId = 1;             // 系统热键 id 从 1 开始递增

private Q_SLOTS:
    // 窗口过程里只排队，真正的 emit 留到事件循环里做：
    // 界面在 activated 里回调 registerHotkey/unregisterAll 是常态，不能在系统消息里重入。
    void dispatchHotkey(int systemId);
};

} // namespace studious::platform
