import QtQuick
import QtQuick.Layouts
import Studious

// MenuPanel 里的一行。28 高、圆角悬浮底、左侧一个圆点槽。
//
// 为什么不用 MenuItem / Popup：这些是**窗内**浮层（浮层窗口本身很小），
// Popup 会被浮层窗口裁掉，只能自己画。
//
// enabled 用的是 Item 自带的那个（不重声明，重声明会撞属性名）：
// enabled = false 时 MouseArea 收不到事件、文字压暗。
Rectangle {
    id: menuRow

    property bool selected: false
    // 圆点是否显示：默认跟选中走；引擎菜单里想「每行都显示状态灯」就传 showDot: true。
    property bool showDot: selected
    // 选中圆点的颜色：跟引擎状态走（Theme.engineStateColor(state)）。
    property color dotColor: Theme.good
    property string text: ""
    signal activated()

    implicitWidth: content.implicitWidth + Theme.s4
    implicitHeight: 28
    // 放进 MenuPanel 的 ColumnLayout 时自动撑满整行（调用方不用再写一遍）。
    Layout.fillWidth: true
    radius: Theme.rSm
    // 悬浮底色。surfaceHighest 是 Theme 里现成的最深一档表面色。
    color: mouse.containsMouse && menuRow.enabled ? Theme.surfaceHighest : "transparent"

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: Theme.s2
        anchors.rightMargin: Theme.s2
        spacing: Theme.s2

        // 左侧槽位：固定宽度，选中画点、未选中留白，文字不会左右跳。
        Item {
            Layout.preferredWidth: 8
            Layout.preferredHeight: 8
            Layout.alignment: Qt.AlignVCenter

            Rectangle {
                anchors.centerIn: parent
                width: 6
                height: 6
                radius: 3
                visible: menuRow.showDot
                color: menuRow.dotColor
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: menuRow.text
            elide: Text.ElideRight
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            font.weight: menuRow.selected ? Font.DemiBold : Font.Normal
            color: menuRow.selected ? Theme.primary : Theme.ink
            opacity: menuRow.enabled ? 1.0 : 0.45
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: menuRow.enabled
        cursorShape: menuRow.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: menuRow.activated()
    }
}
