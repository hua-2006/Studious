// 截图翻译的「设置」小窗：点工具栏「设置」时弹出来，只放「这一次怎么翻」的快捷开关；
// 更细的选项走页脚的「更多设置」→ 主界面「浮层」页。
//
// 它是浮层的**子窗口**（在 CaptureOverlay.qml 里声明，owner 就是浮层）：跟着浮层一起出现、
// 一起收起，位置也永远压在浮层上面。所以这里不要 `transientParent: null`，也不加
// WindowStaysOnTopHint —— 归属窗口本来就恒在 owner 之上，再加置顶反而会盖住系统弹窗。
//
// 为什么 stepper 点了只发 modified()、不自己写 value：value 直接绑在 backend 属性上，
// 组件内部赋值会把绑定打断，关掉再打开就停在旧值；发信号让宿主的绑定继续生效才是单向数据流。
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import Studious

Window {
    id: panel

    objectName: "captureSettingsPanel"

    signal closed()
    // 页脚「更多设置」：关掉小窗，回主界面的「浮层」页（那里是完整版）。
    signal moreSettingsRequested()

    // 谁在用它："capture" = 截屏翻译浮层（默认），"live" = 实时翻译控制条。
    // 两边的可调项不一样（截图有「选完直接翻译」，实时有「刷新间隔」），
    // 所以同一个面板按 mode 只显示对应那一组，而不是各写一份窗口。
    property string mode: "capture"

    // 浮层的子窗口：无边框 + 透明底（圆角与描边由里面的 card 画），Tool 位不进任务栏。
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
    color: "transparent"

    // 宽度固定：两行中文标签 + 一组 stepper 刚好放得下，再宽就压住选区了。
    width: card.width
    // 高度交给内容撑：字号缩放改的是 Theme.fontSize*，这里跟着变。
    height: card.height

    // 从 (screenX, screenY) 这个锚点（「设置」按钮的左上角）弹出来：
    // 右边缘对齐锚点右边；优先落在按钮**下沿之下**（用 anchorHeight 让开工具栏，
    // 否则会压住工具栏的那一排按钮），下面放不下才翻到按钮上沿之上，最后夹进可用桌面。
    // 只摆位置、不管显示与否 —— 显示由宿主（浮层）的 settingsOpen 统一控制，
    // 这样点第二下就能收起来，浮层一关它也自然跟着关。
    function openAt(screenX, screenY, anchorWidth, anchorHeight) {
        const deskW = Screen.desktopAvailableWidth;
        const deskH = Screen.desktopAvailableHeight;
        const gap = Theme.s2;
        const barH = anchorHeight > 0 ? anchorHeight : 30;
        let px = Math.round(screenX + anchorWidth - width);
        px = Math.max(gap, Math.min(px, deskW - width - gap));
        let py = Math.round(screenY + barH + gap);
        if (py + height + gap > deskH)
            py = Math.max(gap, Math.round(screenY - height - gap));
        x = px;
        y = py;
    }

    // Esc 关掉小窗（子窗口自己先拿到键盘；浮层那边也有同样的分支兜底）。
    onVisibleChanged: if (visible) requestActivate()

    Rectangle {
        id: card

        width: 328
        height: content.implicitHeight + Theme.s3 * 2
        color: Theme.surfaceHigh
        radius: Theme.rMd
        border.width: 1
        border.color: Theme.outline

        focus: true
        Keys.onEscapePressed: panel.closed()

            // 吃掉落在卡片空白处的点击：窗口是透明的，不拦一下会点到桌面。
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
            }

        ColumnLayout {
            id: content
            x: Theme.s3
            y: Theme.s3
            width: parent.width - Theme.s3 * 2
            spacing: Theme.s2

            // ------------------------------------------------------------ 标题行（可拖动）
            RowLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.s1
                spacing: Theme.s2

                // 标题兼拖拽把手：无边框窗口没有系统标题栏，按住标题条走 startSystemMove()，
                // 由系统接管移动（比自己在 onPositionChanged 里算坐标顺、也不抖）。
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(22, titleLabel.implicitHeight)

                    Text {
                        id: titleLabel
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("浮层设置")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSection
                        font.weight: Font.DemiBold
                        verticalAlignment: Text.AlignVCenter
                    }

                    MouseArea {
                        id: titleDrag
                        anchors.fill: parent
                        cursorShape: Qt.SizeAllCursor
                        property bool usingSystemMove: false
                        property real grabX: 0
                        property real grabY: 0

                        onPressed: function (mouse) {
                            grabX = mouse.x;
                            grabY = mouse.y;
                            // 有系统拖拽就走系统的（不抖、能跨屏、贴边有系统反馈）；
                            // 拿不到（个别平台/组合）就退回手工跟随，至少不会纹丝不动。
                            usingSystemMove = panel.startSystemMove();
                        }

                        onPositionChanged: function (mouse) {
                            if (!pressed || usingSystemMove)
                                return;
                            panel.x = Math.round(panel.x + mouse.x - grabX);
                            panel.y = Math.round(panel.y + mouse.y - grabY);
                        }
                    }
                }

                Rectangle {
                    implicitWidth: 22
                    implicitHeight: 26
                    radius: Theme.rSm
                    color: closeHover.hovered ? Theme.surfaceHighest : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: Theme.inkVariant
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                    }

                    HoverHandler {
                        id: closeHover
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: panel.closed()
                    }
                }
            }

            // ------------------------------------------------------------ 截图翻译（真控件）
            ColumnLayout {
                id: captureSection

                Layout.fillWidth: true
                spacing: Theme.s2
                visible: panel.mode === "capture"

            Text {
                Layout.fillWidth: true
                text: qsTr("截图翻译")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("选完直接翻译")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                AppToggle {
                    Layout.alignment: Qt.AlignVCenter
                    checked: backend.captureAutoRun
                    onToggled: {
                        backend.captureAutoRun = checked
                        // 开关内部先改了自己的 checked，绑定已被打断；
                        // 按后端实际值拉回来，界面才不会跟设置打架。
                        checked = backend.captureAutoRun
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("关掉就是先框选，再点工具栏的「翻译」按钮。")
                wrapMode: Text.WordWrap
                color: Theme.inkVariant
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("保留原文标注")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                AppToggle {
                    Layout.alignment: Qt.AlignVCenter
                    checked: backend.overlayKeepOriginal
                    onToggled: {
                        backend.overlayKeepOriginal = checked
                        checked = backend.overlayKeepOriginal
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("译文不透明度")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                SetStepper {
                    Layout.alignment: Qt.AlignVCenter
                    from: 40
                    to: 100
                    step: 5
                    suffix: "%"
                    value: backend.overlayOpacity
                    onModified: function (v) { backend.overlayOpacity = v }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("译文字号")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                SetStepper {
                    Layout.alignment: Qt.AlignVCenter
                    from: 10
                    to: 20
                    step: 1
                    suffix: "px"
                    value: backend.overlayFontSize
                    onModified: function (v) { backend.overlayFontSize = v }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("自动复制译文")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                AppToggle {
                    Layout.alignment: Qt.AlignVCenter
                    checked: backend.captureAutoCopy
                    onToggled: {
                        backend.captureAutoCopy = checked
                        checked = backend.captureAutoCopy
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: qsTr("复制后延迟关闭")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    verticalAlignment: Text.AlignVCenter
                }

                SetStepper {
                    Layout.alignment: Qt.AlignVCenter
                    from: 0
                    to: 10
                    step: 1
                    suffix: qsTr("秒")
                    value: backend.overlayAutoHideDelay
                    onModified: function (v) { backend.overlayAutoHideDelay = v }
                }
            }
            }

            // ------------------------------------------------------------ 小窗翻译（mode="mini"）
            ColumnLayout {
                id: miniSection

                Layout.fillWidth: true
                spacing: Theme.s2
                visible: panel.mode === "mini"

                Text {
                    Layout.fillWidth: true
                    text: qsTr("小窗翻译")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("固定在最前")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                    }

                    AppToggle {
                        checked: backend.miniAlwaysOnTop
                        onToggled: function (on) {
                            backend.miniAlwaysOnTop = on
                            checked = backend.miniAlwaysOnTop
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("译文带原文")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                    }

                    AppToggle {
                        checked: backend.showSource
                        onToggled: function (on) {
                            backend.showSource = on
                            checked = backend.showSource
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("「译文带原文」打开后，每段译文上方会多一行原文；关掉就只看译文。")
                    color: Theme.inkVariant
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    wrapMode: Text.Wrap
                }
            }

            // ------------------------------------------------------------ 实时翻译（mode="live"）
            // 和截图翻译那一组互斥显示：同一个窗口，按调用方是谁换内容。
            ColumnLayout {
                id: liveSection

                Layout.fillWidth: true
                spacing: Theme.s2
                visible: panel.mode === "live"

                Text {
                    Layout.fillWidth: true
                    text: qsTr("实时翻译")
                    color: Theme.ink
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("刷新间隔")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    // 400~1500：再快引擎跟不上（每帧都会堆积一样的结果），
                    // 再慢就不像"实时"了。和后端 clamp 对齐。
                    SetStepper {
                        Layout.alignment: Qt.AlignVCenter
                        from: 400
                        to: 1500
                        step: 100
                        suffix: qsTr("ms")
                        value: backend.liveIntervalMs
                        onModified: function (v) { backend.liveIntervalMs = v }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("只在内容变化时翻译")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    AppToggle {
                        Layout.alignment: Qt.AlignVCenter
                        checked: backend.liveOnlyOnChange
                        onToggled: {
                            backend.liveOnlyOnChange = checked
                            checked = backend.liveOnlyOnChange
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("区域底色")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    // 0 = 不铺底（原文会透过贴纸显出来）。
                    SetStepper {
                        Layout.alignment: Qt.AlignVCenter
                        from: 0
                        to: 100
                        step: 10
                        suffix: "%"
                        value: backend.overlayBackdrop
                        onModified: function (v) { backend.overlayBackdrop = v }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("译文不透明度")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    SetStepper {
                        Layout.alignment: Qt.AlignVCenter
                        from: 40
                        to: 100
                        step: 5
                        suffix: "%"
                        value: backend.overlayOpacity
                        onModified: function (v) { backend.overlayOpacity = v }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("译文字号")
                        color: Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    SetStepper {
                        Layout.alignment: Qt.AlignVCenter
                        from: 10
                        to: 20
                        step: 1
                        suffix: "px"
                        value: backend.overlayFontSize
                        onModified: function (v) { backend.overlayFontSize = v }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("改完立刻生效，不用重开这一单。")
                    wrapMode: Text.WordWrap
                    color: Theme.inkVariant
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                }
            }

            // ------------------------------------------------------------ 页脚
            // 这里只放「这一次怎么翻」的快捷开关；完整版在主页面的「浮层」页。
            // 样式跟上面的行保持一致：不加边框、不加底色，只有悬浮时铺一层 surfaceHighest，
            // 文字走强调色 —— 卡片的底色本来就是 surfaceHigh，再套一个盒子会显得是另一个控件。
            Rectangle {
                id: moreButton

                Layout.fillWidth: true
                Layout.preferredHeight: 30
                Layout.topMargin: Theme.s1
                radius: Theme.rXs
                color: moreHover.hovered ? Theme.surfaceHighest : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s1
                    anchors.rightMargin: Theme.s1
                    spacing: Theme.s1

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("更多设置")
                        color: Theme.primary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        text: "›"
                        color: Theme.primary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                HoverHandler {
                    id: moreHover
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: panel.moreSettingsRequested()
                }
            }
        }
    }

    // 「−  12 px  ＋」：components/ 里没有现成的 stepper，这里内联一份。
    // 点击只发 modified(clamped)，绝不自己给 value 赋值 —— 否则会打断宿主
    // `value: backend.xxx` 的绑定，卡片重开就显示旧值。
    component SetStepper: RowLayout {
        id: stepper

        property int value: 0
        property int from: 0
        property int to: 100
        property int step: 1
        property string suffix: ""

        signal modified(int value)

        function clamped(next) {
            return Math.max(stepper.from, Math.min(stepper.to, next));
        }

        spacing: Theme.s1

        StepButton {
            glyph: "−"
            atLimit: stepper.value <= stepper.from
            onActivated: stepper.modified(stepper.clamped(stepper.value - stepper.step))
        }

        Rectangle {
            implicitWidth: Math.max(58, valueLabel.implicitWidth + Theme.s2 * 2)
            implicitHeight: 22
            radius: Theme.rXs
            color: Theme.surfaceHighest

            Text {
                id: valueLabel
                anchors.centerIn: parent
                text: stepper.value + stepper.suffix
                color: Theme.ink
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        StepButton {
            glyph: "+"
            atLimit: stepper.value >= stepper.to
            onActivated: stepper.modified(stepper.clamped(stepper.value + stepper.step))
        }
    }

    // stepper 上的小方块按钮：22×22，到边界就淡到 0.4 并且不再响应。
    component StepButton: Rectangle {
        id: control

        property string glyph: ""
        property bool atLimit: false

        signal activated()

        implicitWidth: 22
        implicitHeight: 22
        radius: Theme.rXs
        color: !control.atLimit && controlHover.hovered ? Theme.surfaceHighest : "transparent"
        border.width: 1
        border.color: Theme.outlineVariant
        opacity: control.atLimit ? 0.4 : 1.0

        Text {
            anchors.centerIn: parent
            text: control.glyph
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }

        HoverHandler {
            id: controlHover
            enabled: !control.atLimit
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            enabled: !control.atLimit
            onTapped: control.activated()
        }
    }
}
