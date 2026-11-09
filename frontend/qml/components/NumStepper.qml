import QtQuick
import QtQuick.Layouts
import Studious

// 「−  值  单位  +」：设置页、浮层页、浮层里那颗齿轮面板共用的一种数字步进。
//
// 契约（和页内旧实现一致，别改）：
//   · 只发 modified(int)，绝不自己给 value 赋值 —— value 通常直接绑在
//     backend 属性上，组件内赋值会把绑定打断，切页回来就显示旧值；
//   · 到边界只把按钮置灰，不发信号。
//
// 两种密度：
//   · 默认（34 高、AppButton 样式）给页面里的表单用；
//   · `compact: true`（22 高、描边小方块 + 窄数字块）给浮层里的设置面板用，
//     那颗面板只有 328 逻辑宽，34 高的按钮挤不下。
// 以前这三处各抄一份（OverlayPage / CaptureSettingsPanel / ConfigPage），
// 现在只有这一个实现。

RowLayout {
    id: stepper

    property int from: 0
    property int to: 100
    property int step: 1
    property int value: 0
    property string unit: ""

    // 数字块宽度：调用方偶尔要给窄一点（比如挤在一行里两个步进）。
    property int valueWidth: 66

    property bool compact: false

    signal modified(int value)

    function clampTo(v) {
        return Math.max(stepper.from, Math.min(stepper.to, v))
    }

    spacing: stepper.compact ? Theme.s1 : Theme.s2

    // 34 高那一款直接借 AppButton（它的 hover / 禁用态已经调好了）。
    AppButton {
        visible: !stepper.compact
        text: "\u2212"
        buttonEnabled: stepper.enabled && stepper.value > stepper.from
        onClicked: stepper.modified(stepper.clampTo(stepper.value - stepper.step))
    }

    Rectangle {
        visible: stepper.compact
        implicitWidth: 22
        implicitHeight: 22
        radius: Theme.rXs
        color: stepper.enabled && stepper.value > stepper.from && minusHover.hovered
               ? Theme.surfaceHighest : "transparent"
        border.width: 1
        border.color: Theme.outlineVariant
        opacity: stepper.enabled && stepper.value > stepper.from ? 1.0 : 0.4

        Text {
            anchors.centerIn: parent
            text: "\u2212"
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }

        HoverHandler {
            id: minusHover
            enabled: stepper.enabled && stepper.value > stepper.from
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            enabled: stepper.enabled && stepper.value > stepper.from
            onTapped: stepper.modified(stepper.clampTo(stepper.value - stepper.step))
        }
    }

    Rectangle {
        Layout.preferredWidth: stepper.compact
                               ? Math.max(58, valueLabel.implicitWidth + Theme.s2 * 2)
                               : stepper.valueWidth
        Layout.preferredHeight: stepper.compact ? 22 : 34
        radius: stepper.compact ? Theme.rXs : Theme.rSm
        color: stepper.compact ? Theme.surfaceHighest : "transparent"

        Text {
            id: valueLabel
            anchors.centerIn: parent
            text: stepper.value + stepper.unit
            font.family: Theme.fontMono
            font.pixelSize: stepper.compact ? Theme.fontSizeSmall : Theme.fontSizeBody
            color: Theme.ink
        }
    }

    AppButton {
        visible: !stepper.compact
        text: "+"
        buttonEnabled: stepper.enabled && stepper.value < stepper.to
        onClicked: stepper.modified(stepper.clampTo(stepper.value + stepper.step))
    }

    Rectangle {
        visible: stepper.compact
        implicitWidth: 22
        implicitHeight: 22
        radius: Theme.rXs
        color: stepper.enabled && stepper.value < stepper.to && plusHover.hovered
               ? Theme.surfaceHighest : "transparent"
        border.width: 1
        border.color: Theme.outlineVariant
        opacity: stepper.enabled && stepper.value < stepper.to ? 1.0 : 0.4

        Text {
            anchors.centerIn: parent
            text: "+"
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }

        HoverHandler {
            id: plusHover
            enabled: stepper.enabled && stepper.value < stepper.to
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            enabled: stepper.enabled && stepper.value < stepper.to
            onTapped: stepper.modified(stepper.clampTo(stepper.value + stepper.step))
        }
    }
}
