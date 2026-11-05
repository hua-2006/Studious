import QtQuick
import Studious

// 小圆角标签（模板 .badge）。
Rectangle {
    id: badge

    property string text: ""
    property bool filled: false

    implicitWidth: label.implicitWidth + Theme.s2 * 2
    implicitHeight: 20
    radius: Theme.rFull
    color: filled ? Theme.primaryContainer : "transparent"
    border.width: filled ? 0 : 1
    border.color: Theme.outlineVariant

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: badge.filled ? Theme.primaryContainerText : Theme.inkVariant
    }
}
