import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Effects
import Studious

// 主窗口：无边框自绘外壳（左侧导航 + 顶栏 + 五页堆栈）。
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

    readonly property var navItems: [
        { icon: "qrc:/qt/qml/Studious/resources/icons/translate.svg", label: qsTr("翻译") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/keyboard.svg",  label: qsTr("快捷键") },
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
                            sourceSize.width: 22
                            sourceSize.height: 22
                            // SVG 描边是写死的深紫，换主题不会变色，这里按主色底重新上色。
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                colorizationColor: Theme.primaryText
                                colorization: 1.0
                            }
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
                        onNavigateRequested: function (index) { root.currentPage = index }
                    }
                    HotkeysPage { }
                    ConfigPage {
                        // 本地 MT 卡的「下载 / 导入」：切到设置页的资产分区并预选 mt 分类。
                        onOpenAssetsRequested: function (kind) {
                            root.currentPage = 3
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
            onActivated: {
                noticeTimer.stop()
                noticeBar.shown = false
                root.currentPage = 3
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
}
