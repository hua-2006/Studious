import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

// 浮层设置的三组控件（小窗口 / 截图翻译 / 实时翻译）。
//
// 唯一实现：设置页「浮层」与浮层里那颗齿轮面板都实例化它 —— 以前这两处各手抄一份
// 控件树，改一边另一边就漂（「自动复制译文」对「翻译完自动复制」那种）。现在文案、
// 后端绑定、数值范围只在下面写一次，两边只是 mode 不同。
//
// 两份宿主的差别只有宽度：页面里一行宽宽松松，说明字跟在控件右边；齿轮面板只有
// 328 逻辑宽，同样一行会被挤成一条竖缝。所以给窄宿主留了两个旋钮：
//   · compact:true   说明字从控件右侧挪到行下方（走 FieldRow 自带的 hint 槽），
//                    步进器换 22 高的紧凑款（NumStepper.compact）；
//   · labelWidth     标签列宽，窄宿主传小一点，把地方让给控件。
// 默认值（compact:false、labelWidth = Theme.formLabelWidth）就是「浮层」页原来那一版，
// 控件、qsTr 文案、backend 绑定、数值范围逐字未改。
ColumnLayout {
    id: root

    // mini | capture | live
    property string mode: "capture"

    // 窄宿主（浮层里那颗齿轮面板）打开：说明字下沉到行下方、步进器用紧凑款。
    property bool compact: false
    // 标签列宽；compact 时默认收窄（面板里 132 的标签列会把控件列挤没）。
    property int labelWidth: compact ? 88 : Theme.formLabelWidth

    spacing: Theme.s4

    // 表单下面那行说明字：左缩进对齐标签列、tiny、次要色。从 OverlayPage 搬过来，
    // 缩进改成跟着 root.labelWidth 走 —— 窄宿主里标签列更窄，说明字才不会悬空。
    component Hint: Text {
        Layout.fillWidth: true
        Layout.leftMargin: root.labelWidth + Theme.s3
        wrapMode: Text.WordWrap
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: Theme.inkVariant
    }

    // ---- 小窗口 ----
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        spacing: Theme.s3
        visible: root.mode === "mini"
        opacity: visible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

        FieldRow {
            id: miniTopRow

            label: qsTr("固定在最前")
            labelWidth: root.labelWidth
            // 说明字只写一遍：宽宿主跟在控件右边（原样），窄宿主下沉成 hint。
            property string note: qsTr("关掉就跟随普通窗口，被别的程序盖住。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.miniAlwaysOnTop
                onToggled: {
                    backend.miniAlwaysOnTop = checked
                    checked = backend.miniAlwaysOnTop
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: miniTopRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        FieldRow {
            id: miniSourceRow

            label: qsTr("译文带原文")
            labelWidth: root.labelWidth
            property string note: qsTr("每段译文上方多一行小字原文，方便对照。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.showSource
                onToggled: {
                    backend.showSource = checked
                    checked = backend.showSource
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: miniSourceRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        // 小窗自己的字号：和「外观」页的正文阅读字号分开，两个场景一句话的字数差很多。
        FieldRow {
            label: qsTr("字号")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 11
                to: 28
                step: 1
                unit: qsTr(" px")
                value: backend.miniFontSize
                onModified: function (v) { backend.miniFontSize = v }
            }
        }

        Hint { text: qsTr("只作用于小窗里的译文正文，不影响翻译页和另外两个浮层。") }
    }

    // ---- 截图翻译 ----
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        spacing: Theme.s3
        visible: root.mode === "capture"
        opacity: visible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

        FieldRow {
            id: captureRunRow

            label: qsTr("选完直接翻译")
            labelWidth: root.labelWidth
            property string note: qsTr("关掉就是二次确认：框完自己点工具栏上的「翻译」。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.captureAutoRun
                onToggled: {
                    backend.captureAutoRun = checked
                    checked = backend.captureAutoRun
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: captureRunRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        FieldRow {
            id: captureCopyRow

            label: qsTr("翻译完自动复制")
            labelWidth: root.labelWidth
            property string note: qsTr("出结果就把整段译文放进剪贴板。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.captureAutoCopy
                onToggled: {
                    backend.captureAutoCopy = checked
                    checked = backend.captureAutoCopy
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: captureCopyRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        FieldRow {
            id: captureCopyCloseRow

            label: qsTr("复制后关闭")
            labelWidth: root.labelWidth
            property string note: qsTr("点工具栏上的「复制」之后立刻收起浮层，不再等核对。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.captureCopyClose
                onToggled: {
                    backend.captureCopyClose = checked
                    checked = backend.captureCopyClose
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: captureCopyCloseRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        FieldRow {
            label: qsTr("译文不透明度")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 40
                to: 100
                step: 5
                unit: "%"
                value: backend.overlayOpacity
                onModified: function (v) { backend.overlayOpacity = v }
            }
        }

        Hint { text: qsTr("越低越能看见底下的原图。") }

        FieldRow {
            label: qsTr("译文字号")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 10
                to: 20
                step: 1
                unit: qsTr(" px")
                value: backend.overlayFontSize
                onModified: function (v) { backend.overlayFontSize = v }
            }
        }

        Hint { text: qsTr("按行高自动收一点，行太窄时不会再缩小到看不清。") }

        FieldRow {
            label: qsTr("复制后延迟关闭")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 0
                to: 10
                step: 1
                unit: qsTr(" 秒")
                value: backend.overlayAutoHideDelay
                onModified: function (v) { backend.overlayAutoHideDelay = v }
            }
        }

        Hint { text: qsTr("0 = 复制完立刻收浮层。") }
    }

    // ---- 实时翻译 ----
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.s2
        spacing: Theme.s3
        visible: root.mode === "live"
        opacity: visible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }

        FieldRow {
            label: qsTr("刷新间隔")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 400
                to: 1500
                step: 100
                unit: qsTr(" 毫秒")
                value: backend.liveIntervalMs
                // 点击只发 modified，回写交给后端 —— 自己改 value 会打断上面这根绑定。
                onModified: function (v) { backend.liveIntervalMs = v }
            }
        }

        Hint { text: qsTr("每帧之间等多久。太短会让引擎一直忙，太长就不像实时了。") }

        FieldRow {
            id: liveChangeRow

            label: qsTr("只在内容变化时翻译")
            labelWidth: root.labelWidth
            property string note: qsTr("画面没变就不重新翻译。关掉后每帧都会重新请求，比较费。")
            hint: root.compact ? note : ""

            AppToggle {
                Layout.alignment: Qt.AlignVCenter
                checked: backend.liveOnlyOnChange
                onToggled: {
                    backend.liveOnlyOnChange = checked
                    // 回灌：开关内部先改了自己的 checked，绑定已被打断；
                    // 按后端实际值拉回来，界面才不会跟设置打架。
                    checked = backend.liveOnlyOnChange
                }
            }

            Text {
                visible: !root.compact
                Layout.fillWidth: true
                text: liveChangeRow.note
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }

        FieldRow {
            label: qsTr("区域底色")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 0
                to: 100
                step: 10
                unit: "%"
                value: backend.overlayBackdrop
                onModified: function (v) { backend.overlayBackdrop = v }
            }
        }

        Hint { text: qsTr("整块选区铺一层底色的浓度；0 = 不铺底，只留译文。") }

        FieldRow {
            label: qsTr("译文不透明度")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 40
                to: 100
                step: 5
                unit: "%"
                value: backend.overlayOpacity
                onModified: function (v) { backend.overlayOpacity = v }
            }
        }

        FieldRow {
            label: qsTr("译文字号")
            labelWidth: root.labelWidth

            NumStepper {
                compact: root.compact
                from: 10
                to: 20
                step: 1
                unit: qsTr(" px")
                value: backend.overlayFontSize
                onModified: function (v) { backend.overlayFontSize = v }
            }
        }
    }
}
