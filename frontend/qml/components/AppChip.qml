import QtQuick
import Studious

// 可勾选的胶囊（模板 .chip）。
Rectangle {
    id: chip

    property string text: ""
    property string value: ""
    property bool checked: false
    property bool chipEnabled: true
    signal toggled()

    implicitWidth: row.implicitWidth + Theme.s4 * 1.5
    implicitHeight: 32
    radius: Theme.rFull
    color: !chipEnabled ? "transparent"
                        : checked ? Theme.primary
                                  : hover.hovered ? Theme.surfaceHigh : "transparent"
    border.width: 1
    border.color: checked ? Theme.primary : Theme.outlineVariant
    opacity: chipEnabled ? 1.0 : 0.38

    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.s1

        Text {
            visible: chip.checked
            text: "\u2713"
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeTiny
            font.weight: Font.Bold
            color: Theme.primaryText
        }

        Text {
            text: chip.text
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            font.weight: chip.checked ? Font.DemiBold : Font.Normal
            color: chip.checked ? Theme.primaryText : Theme.inkVariant
        }
    }

    HoverHandler { id: hover; enabled: chip.chipEnabled }

    MouseArea {
        anchors.fill: parent
        enabled: chip.chipEnabled
        cursorShape: Qt.PointingHandCursor
        onClicked: chip.toggled()
    }
}
