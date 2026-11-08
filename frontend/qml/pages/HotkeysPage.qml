import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects
import Studious

// 快捷键页。
// 三个入口各一张卡（和翻译页的三张入口卡同一套观感）：点卡就是开始录制，
// 录到的组合键交给 backend.hotkeyXxx，由 main.cpp 重新注册并把结果写回 hotkeyStatus。
// 默认一律留空 —— 预置组合键容易和用户机器上已有的软件撞车。
Item {
    id: page

    // 正在录制哪一个："" / "capture" / "mini" / "live"
    property string recordingKey: ""
    // 录制失败时的临时提示（"至少要带一个修饰键…"），2 秒后自己消失。
    property string hint: ""

    readonly property var cards: [
        {
            key: "mini",
            iconSource: "qrc:/qt/qml/Studious/resources/icons/mini.svg",
            title: qsTr("小窗翻译"),
            description: qsTr("敲一下就弹出小窗，取剪贴板里的文字直接翻。"),
            combo: backend.hotkeyMini
        },
        {
            key: "capture",
            iconSource: "qrc:/qt/qml/Studious/resources/icons/screenshot.svg",
            title: qsTr("截图翻译"),
            description: qsTr("敲一下就进入框选，框完按工具栏上的「翻译」。"),
            combo: backend.hotkeyCapture
        },
        {
            key: "live",
            iconSource: "qrc:/qt/qml/Studious/resources/icons/realtime.svg",
            title: qsTr("实时翻译"),
            description: qsTr("敲一下就框选一块区域，之后它会按刷新间隔自己翻。"),
            combo: backend.hotkeyLive
        }
    ]

    // 把按键事件拼成 "Ctrl+Alt+S"（修饰键顺序固定，主键大写）。
    // 只有修饰键时返回空串 —— 那种半成品不该被记下来。
    function comboFromEvent(event) {
        const parts = []
        if (event.modifiers & Qt.ControlModifier)
            parts.push("Ctrl")
        if (event.modifiers & Qt.AltModifier)
            parts.push("Alt")
        if (event.modifiers & Qt.ShiftModifier)
            parts.push("Shift")
        if (event.modifiers & Qt.MetaModifier)
            parts.push("Win")

        const k = event.key
        // 修饰键本身（按下 Ctrl 会先来一个 key=Ctrl 的事件）
        if (k === Qt.Key_Control || k === Qt.Key_Alt || k === Qt.Key_Shift
                || k === Qt.Key_Meta || k === Qt.Key_AltGr)
            return ""

        let name = ""
        if (k >= Qt.Key_A && k <= Qt.Key_Z)
            name = String.fromCharCode(k)
        else if (k >= Qt.Key_0 && k <= Qt.Key_9)
            name = String.fromCharCode(k)
        else if (k >= Qt.Key_F1 && k <= Qt.Key_F24)
            name = "F" + (k - Qt.Key_F1 + 1)
        else
            name = page.specialKeyName(k)

        if (name.length === 0)
            return ""
        parts.push(name)
        return parts.join("+")
    }

    function specialKeyName(k) {
        switch (k) {
        case Qt.Key_Space: return "Space"
        case Qt.Key_Left: return "Left"
        case Qt.Key_Right: return "Right"
        case Qt.Key_Up: return "Up"
        case Qt.Key_Down: return "Down"
        case Qt.Key_Home: return "Home"
        case Qt.Key_End: return "End"
        case Qt.Key_PageUp: return "PageUp"
        case Qt.Key_PageDown: return "PageDown"
        case Qt.Key_Insert: return "Insert"
        case Qt.Key_Delete: return "Delete"
        case Qt.Key_Return: return "Enter"
        case Qt.Key_Enter: return "Enter"
        case Qt.Key_Backspace: return "Backspace"
        case Qt.Key_Tab: return "Tab"
        case Qt.Key_Print: return "Print"
        case Qt.Key_Pause: return "Pause"
        case Qt.Key_Comma: return "Comma"
        case Qt.Key_Period: return "Period"
        case Qt.Key_Slash: return "Slash"
        case Qt.Key_Semicolon: return "Semicolon"
        case Qt.Key_Minus: return "Minus"
        case Qt.Key_Equal: return "Equal"
        case Qt.Key_BracketLeft: return "BracketLeft"
        case Qt.Key_BracketRight: return "BracketRight"
        case Qt.Key_Backslash: return "Backslash"
        case Qt.Key_QuoteLeft: return "QuoteLeft"
        default: return ""
        }
    }

    function comboFor(key) {
        if (key === "capture")
            return backend.hotkeyCapture
        if (key === "mini")
            return backend.hotkeyMini
        if (key === "selection")
            return backend.hotkeySelection
        if (key === "home")
            return backend.hotkeyHome
        return backend.hotkeyLive
    }

    function applyHotkey(key, combo) {
        const normalized = backend.normalizeHotkey(combo)
        if (normalized.length === 0) {
            page.hint = qsTr("至少要带一个修饰键（Ctrl / Alt / Shift / Win），F13–F24 例外。")
            hintTimer.restart()
            return
        }
        if (key === "capture")
            backend.hotkeyCapture = normalized
        else if (key === "mini")
            backend.hotkeyMini = normalized
        else if (key === "selection")
            backend.hotkeySelection = normalized
        else if (key === "home")
            backend.hotkeyHome = normalized
        else
            backend.hotkeyLive = normalized
        page.hint = ""
    }

    function clearHotkey(key) {
        page.recordingKey = ""
        if (key === "capture")
            backend.hotkeyCapture = ""
        else if (key === "mini")
            backend.hotkeyMini = ""
        else if (key === "selection")
            backend.hotkeySelection = ""
        else if (key === "home")
            backend.hotkeyHome = ""
        else
            backend.hotkeyLive = ""
    }

    function startRecording(key) {
        page.recordingKey = page.recordingKey === key ? "" : key
        page.hint = ""
    }

    // 注册失败（被别的程序占用）时给一句人话；成功或未设置返回空串。
    function statusText(key) {
        const info = backend.hotkeyStatus[key]
        if (!info || !info.ok)
            return info && info.message ? info.message : ""
        return ""
    }

    Timer {
        id: hintTimer
        interval: 2400
        onTriggered: page.hint = ""
    }

    // 录制期间由它接键（页面里只要它拿焦点）。
    // 注意：不可见的 Item 拿不到 activeFocus，所以这里必须跟着录制状态 true/false，
    // 而不是一直 visible:false —— 那样 forceActiveFocus() 会静默失败，按键一个都收不到。
    Item {
        id: keyCatcher
        anchors.fill: parent
        z: 50
        focus: false
        visible: page.recordingKey.length > 0

        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Escape) {
                page.recordingKey = ""
                event.accepted = true
                return
            }
            const combo = page.comboFromEvent(event)
            if (combo.length > 0) {
                page.applyHotkey(page.recordingKey, combo)
                page.recordingKey = ""
            }
            // 只有修饰键的按下事件也要吃掉，否则会漏给下面的控件。
            event.accepted = true
        }
    }

    onRecordingKeyChanged: {
        if (recordingKey.length > 0) {
            // visible 的翻转是真事件循环之后的，直接 force 会打空。
            Qt.callLater(function () { keyCatcher.forceActiveFocus() })
        } else {
            keyCatcher.focus = false
        }
    }

    // 点击卡片以外的地方、或者切页，都算放弃这一次录制。
    Connections {
        target: page
        function onVisibleChanged() {
            if (!page.visible)
                page.recordingKey = ""
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s6
        spacing: Theme.s4

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s2

            Text {
                text: qsTr("快捷键")
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeDisplay
                font.weight: Font.Bold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("三个入口各留一个全局键位。默认都是空的：点卡片开始录制，按 Esc 放弃。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: !platformCanHotkey
            implicitHeight: warnText.implicitHeight + Theme.s3 * 2
            radius: Theme.rSm
            color: Theme.cautionContainer

            Text {
                id: warnText
                anchors.fill: parent
                anchors.margins: Theme.s3
                text: qsTr("当前平台没有全局热键能力，键位可以先记下来，但不会被注册。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.caution
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s4

            Repeater {
                model: page.cards

                delegate: Rectangle {
                    id: hotkeyCard
                    required property var modelData

                    readonly property string cardKey: modelData.key
                    readonly property string combo: page.comboFor(cardKey)
                    readonly property bool recording: page.recordingKey === cardKey
                    readonly property string problem: page.statusText(cardKey)

                    Layout.fillWidth: true
                    // 卡片不跟着页面长高：内容就那么点，拉满会留一大片空白。
                    Layout.fillHeight: false
                    Layout.minimumWidth: 240
                    Layout.preferredHeight: 300

                    radius: Theme.rXl
                    color: hover.hovered || hotkeyCard.recording ? Theme.surfaceHigh : Theme.surface
                    border.width: hotkeyCard.recording ? 2 : 1
                    border.color: hotkeyCard.recording
                                  ? Theme.primary
                                  : (hover.hovered ? Theme.primary : Theme.outlineVariant)
                    // 这张卡里放着按钮，绝不能做缩放/位移：放大后按钮跟着挪，
                    // 指针正好压在按钮边缘时 hover 会在"进/出"之间反复翻，
                    // 按钮底色随之闪（实测『录制』『清除』两颗最明显）。
                    // 翻译页的入口卡没有按钮，才允许 scale 1.012。

                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

                    HoverHandler {
                        id: hover
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: page.startRecording(hotkeyCard.cardKey)
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.s5
                        spacing: Theme.s3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s3

                            Item {
                                Layout.alignment: Qt.AlignTop
                                implicitWidth: 32
                                implicitHeight: 32

                                Image {
                                    anchors.fill: parent
                                    source: hotkeyCard.modelData.iconSource
                                    // 按 2 倍光栅化再缩到 32：DPR 1.5 下描边才不会发糊。
                                    sourceSize.width: 64
                                    sourceSize.height: 64
                                    fillMode: Image.PreserveAspectFit
                                    smooth: true
                                    layer.enabled: true
                                    layer.effect: MultiEffect {
                                        colorizationColor: hover.hovered || hotkeyCard.recording
                                                           ? Theme.primary : Theme.inkVariant
                                        colorization: 1.0
                                        Behavior on colorizationColor {
                                            ColorAnimation { duration: Theme.durFast }
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Badge {
                                visible: hotkeyCard.combo.length > 0 && hotkeyCard.problem.length === 0
                                text: qsTr("已绑定")
                                tone: "neutral"
                            }
                            Badge {
                                visible: hotkeyCard.problem.length > 0
                                text: qsTr("未生效")
                                tone: "danger"
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: hotkeyCard.modelData.title
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSection
                            font.weight: Font.DemiBold
                            color: Theme.ink
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.fillWidth: true
                            text: hotkeyCard.modelData.description
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                            lineHeight: 1.25
                        }

                        Item { Layout.fillHeight: true }

                        // 键位显示 / 录制态提示
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 40
                            radius: Theme.rSm
                            color: hotkeyCard.recording ? Theme.primaryContainer : Theme.surfaceLow
                            border.width: 1
                            border.color: hotkeyCard.recording
                                          ? Theme.primary
                                          : Theme.outlineVariant

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - Theme.s3 * 2
                                horizontalAlignment: Text.AlignHCenter
                                text: hotkeyCard.recording
                                      ? qsTr("按组合键…（Esc 放弃）")
                                      : (hotkeyCard.combo.length > 0
                                         ? hotkeyCard.combo
                                         : qsTr("未设置"))
                                elide: Text.ElideRight
                                font.family: hotkeyCard.recording || hotkeyCard.combo.length === 0
                                             ? Theme.fontUi : Theme.fontMono
                                font.pixelSize: Theme.fontSizeBody
                                color: hotkeyCard.recording
                                       ? Theme.primaryContainerText
                                       : (hotkeyCard.combo.length > 0 ? Theme.ink : Theme.outline)
                            }
                        }

                        // 固定两行高：三张卡是同一行，谁多出一行字就会把下面那排按钮顶出去。
                        Text {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.fontSizeTiny * 2.8
                            text: hotkeyCard.problem
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.danger
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s2

                            AppButton {
                                text: hotkeyCard.recording ? qsTr("取消录制")
                                                           : (hotkeyCard.combo.length > 0
                                                              ? qsTr("重新录制") : qsTr("录制"))
                                variant: "text"
                                buttonEnabled: platformCanHotkey
                                onClicked: page.startRecording(hotkeyCard.cardKey)
                            }

                            Item { Layout.fillWidth: true }

                            AppButton {
                                text: qsTr("清除")
                                variant: "text"
                                buttonEnabled: hotkeyCard.combo.length > 0
                                onClicked: page.clearHotkey(hotkeyCard.cardKey)
                            }
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: page.hint.length > 0
            text: page.hint
            wrapMode: Text.WordWrap
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.danger
        }

        SectionCard {
            Layout.fillWidth: true
            Layout.fillHeight: false
            title: qsTr("场景快捷键")

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.s4

                // 打开主页：把主窗口从托盘/后台叫到最前。
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s1

                        Text {
                            text: qsTr("打开主页")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Font.DemiBold
                            color: Theme.ink
                        }

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("不管主窗口是收进托盘还是被别的窗口盖住，按一下就把它叫到最前。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: page.statusText("home").length > 0
                            text: page.statusText("home")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.danger
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 200
                        Layout.preferredHeight: 36
                        radius: Theme.rSm
                        color: page.recordingKey === "home" ? Theme.primaryContainer : Theme.surfaceLow
                        border.width: 1
                        border.color: page.recordingKey === "home"
                                      ? Theme.primary
                                      : Theme.outlineVariant

                        Text {
                            anchors.centerIn: parent
                            width: parent.width - Theme.s3 * 2
                            horizontalAlignment: Text.AlignHCenter
                            text: page.recordingKey === "home"
                                  ? qsTr("按组合键…（Esc 放弃）")
                                  : (backend.hotkeyHome.length > 0
                                     ? backend.hotkeyHome : qsTr("未设置"))
                            elide: Text.ElideRight
                            font.family: page.recordingKey === "home"
                                         || backend.hotkeyHome.length === 0
                                         ? Theme.fontUi : Theme.fontMono
                            font.pixelSize: Theme.fontSizeBody
                            color: page.recordingKey === "home"
                                   ? Theme.primaryContainerText
                                   : (backend.hotkeyHome.length > 0 ? Theme.ink : Theme.outline)
                        }
                    }

                    AppButton {
                        text: page.recordingKey === "home"
                              ? qsTr("取消录制")
                              : (backend.hotkeyHome.length > 0 ? qsTr("重新录制") : qsTr("录制"))
                        variant: "text"
                        buttonEnabled: platformCanHotkey
                        onClicked: page.startRecording("home")
                    }

                    AppButton {
                        text: qsTr("清除")
                        variant: "text"
                        buttonEnabled: backend.hotkeyHome.length > 0
                        onClicked: page.clearHotkey("home")
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.outlineVariant
                }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s3

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s1

                    Text {
                        text: qsTr("划词翻译")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Font.DemiBold
                        color: Theme.ink
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("打开小窗、把剪贴板里的文字填进去并直接翻。图片不接，富文本只取文字；超过 1500 字只填不翻，留给你确认后再点「翻译」。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: page.statusText("selection").length > 0
                        text: page.statusText("selection")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.danger
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 200
                    Layout.preferredHeight: 36
                    radius: Theme.rSm
                    color: page.recordingKey === "selection" ? Theme.primaryContainer : Theme.surfaceLow
                    border.width: 1
                    border.color: page.recordingKey === "selection"
                                  ? Theme.primary
                                  : Theme.outlineVariant

                    Text {
                        anchors.centerIn: parent
                        width: parent.width - Theme.s3 * 2
                        horizontalAlignment: Text.AlignHCenter
                        text: page.recordingKey === "selection"
                              ? qsTr("按组合键…（Esc 放弃）")
                              : (backend.hotkeySelection.length > 0
                                 ? backend.hotkeySelection : qsTr("未设置"))
                        elide: Text.ElideRight
                        font.family: page.recordingKey === "selection"
                                     || backend.hotkeySelection.length === 0
                                     ? Theme.fontUi : Theme.fontMono
                        font.pixelSize: Theme.fontSizeBody
                        color: page.recordingKey === "selection"
                               ? Theme.primaryContainerText
                               : (backend.hotkeySelection.length > 0 ? Theme.ink : Theme.outline)
                    }
                }

                AppButton {
                    text: page.recordingKey === "selection"
                          ? qsTr("取消录制")
                          : (backend.hotkeySelection.length > 0 ? qsTr("重新录制") : qsTr("录制"))
                    variant: "text"
                    buttonEnabled: platformCanHotkey
                    onClicked: page.startRecording("selection")
                }

                AppButton {
                    text: qsTr("清除")
                    variant: "text"
                    buttonEnabled: backend.hotkeySelection.length > 0
                    onClicked: page.clearHotkey("selection")
                }
            }
            }
        }
    }
}
