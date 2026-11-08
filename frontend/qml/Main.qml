import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Effects
import Studious

// 主窗口：无边框自绘外壳（左侧导航 + 顶栏 + 六页堆栈）。
//
// 为什么用 FramelessWindowHint：
//   系统标题栏没法改圆角、也没法跟我们的配色统一，只能整条去掉自己画。
//   代价是拖拽、缩放、窗口按钮全要自己接 —— 都放在下面了。
Window {
    id: root
    // 尺寸单位是「逻辑像素」，不是截图里的物理像素。
    // 本机屏幕 2560×1600 @150% → 逻辑 1707×1067。
    // 用户量的 1680×1080 是微信截图的物理像素，÷1.5 才是这里的数：1120×720。
    // 屏幕比这还小时按可用区域收缩（换台 1366×768 的本子不会开出屏幕外）。
    width: Math.min(1120, Math.round(Screen.desktopAvailableWidth * 0.92))
    height: Math.min(720, Math.round(Screen.desktopAvailableHeight * 0.92))
    // 最小尺寸按「翻译页双栏 + 页脚状态条」的实测需求抬上来：
    // 980×620 时原文/译文面板的正文区会被压成 0 高，状态条右侧也会被窗口边缘裁掉。
    minimumWidth: 1040
    minimumHeight: 660
    visible: true
    title: "Studious"

    flags: Qt.Window | Qt.FramelessWindowHint
    // 窗口本身透明，圆角外的区域才会露出桌面（不透明的话四角永远是方块）。
    color: "transparent"

    // --ui-shot 用：外部直接设这个属性切页。
    property int currentPage: 0

    readonly property bool maximized: visibility === Window.Maximized
    readonly property int cornerRadius: 12

    // 关闭行为：设置里选了「收进托盘」就不真退，只隐藏窗口。
    // 托盘装不上时照样真退出 —— 否则窗口一藏，用户就再也点不到它了。
    onClosing: function (close) {
        if (backend.closeToTray && backend.trayAvailable && !root.forceQuit) {
            close.accepted = false
            root.hide()
        }
    }
    // 托盘菜单「退出」/ 设置页「重置应用」最终都走 C++ 的 quit，这里只做个显式开关，
    // 万一以后有人想走 window.close() 退出时不被上面那条拦下来。
    property bool forceQuit: false

    // 导航顺序 = StackLayout 顺序，两处必须同步改：
    //   0 翻译 / 1 快捷键 / 2 浮层 / 3 配置 / 4 设置 / 5 外观
    // 「浮层」插在「快捷键」之后：热键怎么按完，下一页就是按完弹出来的东西长什么样。
    readonly property var navItems: [
        { icon: "qrc:/qt/qml/Studious/resources/icons/translate.svg", label: qsTr("翻译") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/keyboard.svg",  label: qsTr("快捷键") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/overlay.svg",   label: qsTr("浮层") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/tune.svg",      label: qsTr("配置") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/settings.svg",  label: qsTr("设置") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/palette.svg",   label: qsTr("外观") }
    ]

    // 字体：main.cpp 注册完字体文件后注入族名，这里绑进 Theme 单例。
    // 用 Binding 而不是 onCompleted 赋值，是为了在子项创建前就生效，避免首帧跳字。
    Binding { target: Theme; property: "fontUi"; value: bundledFontUi }
    Binding { target: Theme; property: "fontContent"; value: bundledFontContent }
    Binding { target: Theme; property: "fontMono"; value: bundledFontMono }
    // 字号缩放：后端存的是百分比，Theme 用的是倍数。
    Binding { target: Theme; property: "fontScale"; value: backend.fontScale / 100.0 }

    // 主题：启动时按设置落一次，之后后端一变就跟着切。
    // 所有 Theme.xxx 都是 `dark ? 暗 : 亮` 的绑定，改 Theme.dark 会整体重算，无需重建界面。
    // 把当前主题色推给 QPalette。
    // QML 是配色唯一来源，但这个 app 里还有些东西是系统绘制的（文件对话框、
    // 滚动条、右键菜单、ToolTip），它们只认 QPalette。不同步就会出现
    // 「浅色界面 + 深色对话框」这种割裂。
    function syncInterfacePalette() {
        backend.applyPalette({
            "window": Theme.surface,
            "windowText": Theme.ink,
            "base": Theme.surfaceLow,
            "alternateBase": Theme.surfaceMid,
            "text": Theme.ink,
            "button": Theme.surfaceHigh,
            "buttonText": Theme.ink,
            "highlight": Theme.primaryContainer,
            "highlightedText": Theme.primaryContainerText,
            "toolTipBase": Theme.surfaceHigh,
            "toolTipText": Theme.ink,
            "placeholderText": Theme.inkVariant
        })
    }

    Component.onCompleted: {
        Theme.systemDark = systemPrefersDark
        Theme.applyMode(backend.themeMode)
        // 强调色：启动就按用户配置生成整套调色板（非法值由 Theme 内部回落默认紫）。
        Theme.applySeed(backend.accentColor)
        syncInterfacePalette()
        // 先把日志窗口造出来（隐藏），双击 logo 时只是 show，不用等首帧。
        ensureLogWindow()
    }

    Connections {
        target: backend
        function onThemeModeChanged() {
            Theme.applyMode(backend.themeMode)
            syncInterfacePalette()
        }
        function onAccentColorChanged() {
            Theme.applySeed(backend.accentColor)
            syncInterfacePalette()
        }
        // 后端的错误出口就这一个信号：void errorRaised(const QString &title, const QString &detail)
        // （签名见 frontend/src/AppBackend.h 的 signals 段）。以前 QML 侧没有消费者，
        // 出错误等于静默失败，现在统一弹顶部横幅。
        // 不去轮询 statusText / statusIsError：状态行本来就会自己显示，信号才是「新事件」。
        function onErrorRaised(title, detail) {
            root.showErrorNotice(title, detail)
        }
    }

    // ---------------- 全局错误通知条 ----------------
    // 同一时间只显示一条：新错误直接覆盖旧的，并重置 8 秒自动收起计时。
    Timer {
        id: noticeTimer
        interval: 8000
        repeat: false
        onTriggered: noticeBar.shown = false
    }

    function showErrorNotice(title, detail) {
        noticeBar.title = title.length > 0 ? title : qsTr("出错了")
        noticeBar.detail = detail
        noticeBar.tone = "error"
        noticeBar.shown = true
        noticeTimer.restart()
    }

    // ---------------- 外框（圆角 + 描边）----------------
    Rectangle {
        id: frame
        anchors.fill: parent
        radius: root.maximized ? 0 : root.cornerRadius
        color: Theme.surface
        // 窗口本身不加描边：圆角外侧直接是桌面，加边反而像贴了一圈蓝线。
        border.width: 0
        // 不要在这里开 clip：Qt Quick 的 clip 只按外接矩形裁，不会跟着 radius 走，
        // 开了也拦不住导航条和顶栏把四角盖成直角。改成让贴着角的那两个子元素自己带圆角。

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // ---------------- 左侧导航 ----------------
            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 88
                color: Theme.railSurface
                // 贴着窗口左侧两个角：跟着外框一起圆。
                topLeftRadius: root.maximized ? 0 : root.cornerRadius
                bottomLeftRadius: root.maximized ? 0 : root.cornerRadius

                Rectangle {
                    width: 1
                    height: parent.height
                    anchors.right: parent.right
                    color: Theme.outlineVariant
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: Theme.s4
                    anchors.bottomMargin: Theme.s4
                    anchors.leftMargin: Theme.s2
                    anchors.rightMargin: Theme.s2
                    spacing: Theme.s2

                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        width: 40
                        height: 40
                        radius: Theme.rMd
                        color: Theme.primary

                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/qt/qml/Studious/resources/icons/brand.svg"
                            // brand.svg 的 viewBox 是 40×24（横向字标），原来把 sourceSize
                            // 写成 22×22 会让它被拉伸成方的：竖笔画被拉长，看着像歪的。
                            // 按原始比例给 24×14.4，并开 PreserveAspectFit 兜住小数误差。
                            sourceSize.width: 52
                            sourceSize.height: 32
                            width: 26
                            height: 15.6
                            fillMode: Image.PreserveAspectFit
                            // SVG 描边是写死的深紫，换主题不会变色，这里按主色底重新上色。
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: Theme.primaryText
                                colorization: 1.0
                            }
                        }

                        // 双击 logo 开日志窗口。这里既不在 dragArea（顶栏）里、
                        // 也没有别的 MouseArea，双击不会被 startSystemMove 吞掉。
                        HoverHandler { cursorShape: Qt.PointingHandCursor }

                        TapHandler {
                            acceptedButtons: Qt.LeftButton
                            onDoubleTapped: root.openLogWindow()
                        }
                    }

                    Item { Layout.preferredHeight: Theme.s3 }

                    Repeater {
                        model: root.navItems

                        delegate: ColumnLayout {
                            id: railItem
                            required property int index
                            required property var modelData

                            Layout.fillWidth: true
                            spacing: 0

                            // 翻译（第 0 项）和快捷键（第 1 项）之间加一条分割线。
                            // 放在下标的 delegate 里而不是单独一条，是因为 Repeater 的
                            // delegate 必须连续排布，插独立节点会破坏 Layout 顺序。
                            // 下标保持 1：插入「浮层」是往快捷键**之后**加，这条线不该跟着挪。
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: Theme.s3
                                Layout.rightMargin: Theme.s3
                                Layout.topMargin: Theme.s2
                                Layout.bottomMargin: Theme.s2
                                Layout.preferredHeight: 1
                                visible: railItem.index === 1
                                color: Theme.outlineVariant
                            }

                            Rectangle {
                                id: railItemBody

                                Layout.fillWidth: true
                                Layout.preferredHeight: 60
                                radius: Theme.rMd
                                color: root.currentPage === railItem.index
                                       ? Theme.primaryContainer : "transparent"

                                Column {
                                    anchors.centerIn: parent
                                    spacing: Theme.s1

                                    Image {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        source: railItem.modelData.icon
                                        sourceSize.width: 20
                                        sourceSize.height: 20
                                        // SVG 里的描边色是写死的，换主题不会跟着变。
                                        // 这里用 MultiEffect 按调色板重新上色：
                                        // 选中用 on-surface（深色主题下是近白），未选中用 on-surface-variant。
                                        // 两个都是强调色推导出来的，不是硬编码白色。
                                        opacity: root.currentPage === railItem.index ? 1.0 : 0.85
                                        layer.enabled: true
                                        layer.effect: MultiEffect {
                                            colorizationColor: root.currentPage === railItem.index
                                                               ? Theme.railIconActive
                                                               : Theme.railIconIdle
                                            colorization: 1.0
                                        }
                                    }
                                    // 导航栏只有 88px 宽，中文两字够用，英文单词不一定
                                    // （Configuration 比栏宽还宽，会溢出选中胶囊）。
                                    // 按基础字号实测宽度等比缩一点，任何语言都不溢出。
                                    FontMetrics {
                                        id: railLabelMetrics
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeTiny
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: railItem.modelData.label
                                        font.family: Theme.fontUi
                                        readonly property real wanted: railLabelMetrics.advanceWidth(text)
                                        font.pixelSize: wanted > railItemBody.width
                                                        ? Theme.fontSizeTiny * railItemBody.width / wanted
                                                        : Theme.fontSizeTiny
                                        font.weight: root.currentPage === railItem.index
                                                     ? Font.DemiBold : Font.Normal
                                        color: root.currentPage === railItem.index
                                               ? Theme.railIconActive : Theme.railIconIdle
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.currentPage = railItem.index
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------- 右侧：顶栏 + 页面 ----------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // 顶栏：兼作拖动区
                Rectangle {
                    id: appBar
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    color: Theme.surface
                    // 贴着窗口右上角：跟着外框一起圆。
                    topRightRadius: root.maximized ? 0 : root.cornerRadius

                    Rectangle {
                        width: parent.width
                        height: 1
                        anchors.bottom: parent.bottom
                        color: Theme.outlineVariant
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s6
                        anchors.rightMargin: Theme.s2
                        spacing: Theme.s2

                        Text {
                            text: "Studious"
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSection + 2
                            font.weight: Font.DemiBold
                            color: Theme.ink
                        }
                        Text {
                            text: qsTr("截图 / 文本翻译")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }

                        // 中间空档：按住拖动窗口（双击切最大化）。
                        Item {
                            id: dragArea
                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.ArrowCursor
                                onPressed: root.startSystemMove()
                                onDoubleClicked: root.maximized
                                                     ? root.showNormal()
                                                     : root.showMaximized()
                            }
                        }

                        // 窗口按钮
                        Repeater {
                            model: [
                                { glyph: "\u2212", tip: qsTr("最小化") },
                                { glyph: "\u25A1", tip: qsTr("最大化") },
                                { glyph: "\u2715", tip: qsTr("关闭") }
                            ]

                            delegate: Rectangle {
                                id: winBtn
                                required property int index
                                required property var modelData

                                Layout.preferredWidth: 36
                                Layout.preferredHeight: 28
                                radius: Theme.rSm
                                color: winBtn.index === 2
                                       ? (winBtnHover.hovered ? Theme.dangerContainer : "transparent")
                                       : (winBtnHover.hovered ? Theme.surfaceHigh : "transparent")

                                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                                Text {
                                    anchors.centerIn: parent
                                    text: winBtn.modelData.glyph
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: winBtn.index === 2 && winBtnHover.hovered
                                           ? Theme.danger : Theme.inkVariant
                                }

                                HoverHandler { id: winBtnHover }

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (winBtn.index === 0) {
                                            // 「最小化到托盘」开着就只隐藏窗口，进程留着。
                                            // 托盘装不上时退回普通最小化，别把窗口变成够不着的幽灵。
                                            if (backend.minimizeToTray && backend.trayAvailable) {
                                                root.hide()
                                            } else {
                                                root.showMinimized()
                                            }
                                        } else if (winBtn.index === 1) {
                                            root.maximized ? root.showNormal() : root.showMaximized()
                                        } else {
                                            root.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.currentPage

                    TranslatePage {
                        objectName: "translatePage"
                        // TranslatePage 里的「查看引擎状态」还是按插入「浮层」之前的旧顺序
                        // 发的 2（当时 2 = 配置）。浮层占了 2，配置后移到 3，
                        // 那页不在这次改动范围内，字面量改不了，只能在这条通道上平移一位。
                        onNavigateRequested: function (index) { root.currentPage = index >= 2 ? index + 1 : index }
                        // 三张入口卡：小窗与实时的界面还没接（先明确说一句），截图直接进流程。
                        onLaunchRequested: function (kind) { root.launch(kind) }
                    }
                    HotkeysPage { }
                    OverlayPage { }
                    ConfigPage {
                        // 本地 MT 卡的「下载 / 导入」：切到设置页的资产分区并预选 mt 分类。
                        onOpenAssetsRequested: function (kind) {
                            root.currentPage = 4
                            settingsPage.openAssets(kind)
                        }
                    }
                    // objectName 给 --ui-section 用：截图工具靠它找到设置页并切分区。
                    SettingsPage {
                        id: settingsPage
                        objectName: "settingsPage"
                    }
                    AppearancePage { }
                }
            }
        }

        // ---------------- 全局错误通知条（覆盖层）----------------
        // 不参与上面的 RowLayout：绝对定位 + 顶部边距，弹出来不挤动页面布局。
        NoticeBar {
            id: noticeBar
            z: 100
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: Theme.s4
            anchors.leftMargin: Theme.s4
            anchors.rightMargin: Theme.s4

            onDismissed: {
                noticeTimer.stop()
                noticeBar.shown = false
            }
            // 「去处理」：后端已有的 errorRaised 绝大多数来自资产安装/卸载与本地 LLM 启动，
            // 落点放在设置页（资产分区就在这一页），用户一眼能看到缺什么。
            // 下标 4 = 设置（0 翻译 / 1 快捷键 / 2 浮层 / 3 配置 / 4 设置 / 5 外观）。
            onActivated: {
                noticeTimer.stop()
                noticeBar.shown = false
                root.currentPage = 4
            }
        }
    }

    // ---------------- 无边框窗口的缩放边 ----------------
    // 系统标题栏没了，四条边要自己接 startSystemResize，否则窗口拖不大。
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 5
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 5
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.BottomEdge)
    }

    // ---------------- 截屏翻译的遮罩 / 结果浮层 ----------------
    // 它是**独立顶层窗口**：铺满虚拟桌面、置顶、无边框。主窗口自己不能当遮罩
    // （会裁到 1120×720 的窗口范围内，选区一出界就废）。
    CaptureOverlay {
        id: captureOverlay
        // 几何从平台层拿（QML 的 Screen.virtualWidth 在 Qt 6.12 里取不到值，会变 NaN）。
        desktopRect: screenFlow.desktopRect

        // 「设置」小窗是浮层自己的子窗口，摆在哪儿、什么时候收都由浮层管；
        // 这里只接它页脚那句「更多设置」：收掉这一单、回主界面的「浮层」页。
        onMoreSettingsRequested: {
            screenFlow.cancel();
            root.openFloatingSettings();
        }

        // 实时模式：框选完点了「开始实时翻译」，把选区交给 LiveSession。
        // 浮层自己会退回 Idle，接下来屏幕上就只剩贴纸窗口 + 控制条。
        onLiveStartRequested: function (region) {
            liveSession.start(region);
        }
    }

    // ---------------- 日志窗口 ----------------
    // 左上角 logo 双击打开（见上面的 TapHandler）。
    // 关键：用 createObject(null) 现造一个**根对象**窗口。直接写成 `Window { }`
    // 子项的话，QML 会把它挂成主窗口的 transient 子窗口 —— 主窗口最小化它就跟着
    // 消失、还永远压在主窗口上面，那不叫独立窗口。
    property var logWindow: null

    Component {
        id: logWindowComponent
        LogWindow {}
    }

    function ensureLogWindow() {
        if (logWindow === null) {
            logWindow = logWindowComponent.createObject(null)
        }
        return logWindow
    }

    function openLogWindow() {
        var win = ensureLogWindow()
        if (win === null) {
            return
        }
        win.show()
        win.raise()
        win.requestActivate()
    }

    // ---------------- 实时翻译的两块界面 ----------------
    // 贴纸窗口：完全透明、点击穿透、不被自己截图拍到（这三条都在 platform 层设，
    // QML 这边只负责画行）。它永远不该拿到焦点，所以没有任何可点元素。
    LiveOverlay {
        id: liveOverlay
        // 必须是**独立**顶层窗口：声明在 Main 里时 Qt 会自动把它认成主窗口的
        // transient 子窗口，主窗口一最小化它就跟着消失（用户实测提过这点）。
        // 置空之后它只受自己的 visible 管。
        transientParent: null
    }

    // 控制条：贴纸窗口点不到，所以暂停/停止/退出这些必须放在这个独立小窗里。
    LiveControlBar {
        id: liveControlBar
        transientParent: null

        // 页脚「更多设置」：控制条不切页，回主界面的「浮层」页。
        // 不暂停这一单 —— 用户只是去看设置，回来还要接着看译文。
        onMoreSettingsRequested: root.openFloatingSettings()
    }

    // ---------------- 小窗翻译 ----------------
    // 一个常驻置顶的小窗（设计稿 B 页）。同样是独立顶层窗口：主窗口最小化时
    // 它得留在屏幕上，而且它自己的关闭按钮只关它自己。
    MiniWindow {
        id: miniWindow
        transientParent: null

        onMoreSettingsRequested: root.openFloatingSettings()
    }

    // 语言对是全局一份（settings/lang/src、settings/lang/dst）：主页、截屏、实时
    // 读的是 AppBackend 的内存副本，小窗读的是 MiniSession 的内存副本。任一侧改
    // 完都落盘，然后由这里把另一侧的副本拉平 —— 否则界面上会同时存在两套语言。
    // reload* 都是"值不同才 emit"，所以来回拉不会打环。
    Connections {
        target: miniSession
        function onLanguagesChanged() { backend.reloadLanguagesFromSettings() }
    }

    Connections {
        target: backend
        function onSrcLangChanged() { miniSession.reloadLanguagesFromSettings() }
        function onDstLangsChanged() { miniSession.reloadLanguagesFromSettings() }
    }

    Connections {
        target: liveSession
        // 会话结束（停止 / 连续无文字 / 出错）时整组写一条 live.db 记录。
        // historyEnabled 关着的时候 AppBackend 自己会挡掉，这里不用判断。
        function onSessionFinished(originals, blocks) {
            backend.saveLiveRecord(originals, blocks, liveSession.engineLabel)
        }
    }

    // 三个浮层窗（截图 / 实时 / 小窗）的「设置 → 更多设置」都到这里：
    // 收起各自的面板，把主窗口叫到最前，切到「浮层」页（页序见 rail：
    // 0 翻译 / 1 快捷键 / 2 浮层 / 3 配置 / 4 设置 / 5 外观）。
    function openFloatingSettings() {
        root.show()
        root.raise()
        root.requestActivate()
        root.currentPage = 2
    }

    // 入口分发：三张卡片与全局热键都从这里进来。
    // 小窗还没做界面，先用通知条明确说一句，别让人以为点了没反应。
    function launch(kind) {
        if (kind === "capture") {
            captureOverlay.purpose = "capture"
            screenFlow.begin()
        } else if (kind === "live") {
            // 实时先借用同一个选区浮层，只是工具栏换成「开始实时翻译 / 取消」。
            captureOverlay.purpose = "live"
            screenFlow.begin()
        } else if (kind === "mini") {
            // 小窗就一个：已经在开就抬到最前，别再开第二个。
            miniWindow.show()
            miniWindow.raise()
            miniWindow.requestActivate()
        } else if (kind === "selection") {
            // 划词翻译：开小窗 → 读剪贴板填进去 → 短文本直接翻（长文只填不翻）。
            miniWindow.show()
            miniWindow.raise()
            miniWindow.requestActivate()
            miniSession.grabSelection()
        } else if (kind === "home") {
            // 打开主页：托盘/后台时也叫到最前，并切回翻译页。
            root.show()
            root.raise()
            root.requestActivate()
        }
    }

    Connections {
        target: screenFlow
        // 热键走的是 C++ → QML 这条：capture 已经在 C++ 里直接 begin()，
        // 这里只接过 mini / live。
        function onLaunchRequested(kind) { root.launch(kind) }

        // 「保存」：存储层在 AppBackend（一份 db、一个连接），浮层只交出这一屏的数据。
        function onSaveRecordRequested(source, imagePath, region, originals, blocks) {
            backend.saveCaptureRecord(source, imagePath, region, originals, blocks)
        }
    }
}
