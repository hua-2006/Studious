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
    // 写死 1650×1050 会被再乘 1.5 变成 2475×1575，几乎铺满整屏 —— 这就是之前"窗口很大"的原因。
    // 所以改成按可用屏幕比例算，并给一个上限，换分辨率也不会失控。
    width: Math.min(1440, Math.round(Screen.desktopAvailableWidth * 0.85))
    height: Math.min(900, Math.round(Screen.desktopAvailableHeight * 0.88))
    minimumWidth: 980
    minimumHeight: 620
    visible: true
    title: "Studious"

    flags: Qt.Window | Qt.FramelessWindowHint
    // 窗口本身透明，圆角外的区域才会露出桌面（不透明的话四角永远是方块）。
    color: "transparent"

    // --ui-shot 用：外部直接设这个属性切页。
    property int currentPage: 0

    readonly property bool maximized: visibility === Window.Maximized
    readonly property int cornerRadius: 12

    readonly property var navItems: [
        { icon: "qrc:/qt/qml/Studious/resources/icons/translate.svg", label: "翻译" },
        { icon: "qrc:/qt/qml/Studious/resources/icons/keyboard.svg",  label: "快捷键" },
        { icon: "qrc:/qt/qml/Studious/resources/icons/tune.svg",      label: "配置" },
        { icon: "qrc:/qt/qml/Studious/resources/icons/settings.svg",  label: "设置" },
        { icon: "qrc:/qt/qml/Studious/resources/icons/palette.svg",   label: "外观" }
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
    Component.onCompleted: {
        Theme.systemDark = systemPrefersDark
        Theme.applyMode(backend.themeMode)
        // 强调色：启动就按用户配置生成整套调色板（非法值由 Theme 内部回落默认紫）。
        Theme.applySeed(backend.accentColor)
    }

    Connections {
        target: backend
        function onThemeModeChanged() { Theme.applyMode(backend.themeMode) }
        function onAccentColorChanged() { Theme.applySeed(backend.accentColor) }
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

                        delegate: Rectangle {
                            id: railItem
                            required property int index
                            required property var modelData

                            Layout.fillWidth: true
                            Layout.preferredHeight: 60
                            radius: Theme.rMd
                            color: root.currentPage === index ? Theme.primaryContainer : "transparent"

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
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: railItem.modelData.label
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeTiny
                                    font.weight: root.currentPage === railItem.index ? Font.DemiBold : Font.Normal
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
                            text: "截图 / 文本翻译"
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
                                { glyph: "\u2212", tip: "最小化" },
                                { glyph: "\u25A1", tip: "最大化" },
                                { glyph: "\u2715", tip: "关闭" }
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
                                            root.showMinimized()
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
                    ConfigPage { }
                    SettingsPage { }
                    AppearancePage { }
                }
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
