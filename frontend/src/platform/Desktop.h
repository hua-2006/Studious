// 桌面集成 — 托盘 / 开机自启 / 单实例。
//
// 与 IPlatform 分开：那个接缝管的是截图、热键、OCR 这类"能力"，
// 这里是"应用在操作系统里怎么活着"的三件事，实现方式完全不同。
//
// 铁律：本文件（含注释）只出现 Qt Core 的类型，平台头一律只进 .cpp。
// 托盘刻意用 Shell_NotifyIcon 手搓而不是 QSystemTrayIcon ——
// 后者在 QtWidgets 里，为了一个托盘图标把整个 Widgets 拖进这个 QML 应用不值。
#pragma once

#include <QObject>
#include <QString>

// 前向声明必须放在**全局作用域**：写在 namespace 里会就地声明一个
// studious::platform::QLocalServer，跟真类不是同一个东西（踩过一次）。
class QLocalServer;

namespace studious::platform {

class Desktop final : public QObject {
    Q_OBJECT

public:
    explicit Desktop(QObject *parent = nullptr);
    ~Desktop() override;

    // ---- 托盘 ----

    // 托盘是否真的装上了。会顺手把图标建出来（建出来就是隐藏态，不会凭空多个图标），
    // 所以这里不是 const ——"能不能装"只能靠真装一次才知道。
    // 没 shell（远程 / 精简会话）时返回 false，界面据此把托盘相关开关置灰。
    bool trayAvailable();

    // 显示 / 隐藏托盘图标。创建在第一次调用（或 trayAvailable()）时发生。
    void setTrayEnabled(bool enabled);

    // ---- 开机自启 ----

    // 写 / 删 HKCU 的 Run 键。失败返回 false 并写 *error（已本地化）。
    static bool setAutoStart(bool enabled, QString *error);
    static bool autoStartRegistered();

    // ---- 单实例 ----

    // 抢占实例名。false = 已有实例在跑（调用方应该 ping 一下它然后退出）。
    // 成功时内部持有 QLocalServer，收到新连接就发 restoreRequested()。
    bool startSingleInstance(const QString &name);
    // 通知已在运行的实例把窗口叫出来（连不上就静默放弃）。
    // 连得上就说明已经有一个实例在跑，返回 true（顺手让它把窗口叫到前面）。
    static bool pingRunningInstance(const QString &name);

    // 应用退出前调用：解绑实例名 + 卸托盘。可重复调用。
    void shutdown();

signals:
    // 托盘双击 / 菜单「显示主窗口」/ 第二条实例 ping 过来。
    void restoreRequested();
    // 托盘菜单「退出」。
    void quitRequested();

private:
    // 建一次托盘图标（隐藏态）。已建过直接返回 true。
    bool createTray();
    void destroyTray();

    bool m_trayReady = false;
    void *m_trayIcon = nullptr;    // HICON
    void *m_trayWindow = nullptr;  // HWND（消息窗口，托盘回调靠它收消息）
    QLocalServer *m_instanceServer = nullptr;
};

} // namespace studious::platform
