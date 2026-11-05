import QtQuick
import Studious

// 胶囊开关（模板 .sw）。
Rectangle {
    id: sw

    property bool checked: false
    property bool toggleEnabled: true
    signal toggled(bool checked)

    implicitWidth: 44
    implicitHeight: 24
    radius: Theme.rFull
    color: checked ? Theme.primary : Theme.surfaceHighest
    border.width: checked ? 0 : 1
    border.color: Theme.outline
    opacity: toggleEnabled ? 1.0 : 0.38

    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Rectangle {
        width: 18
        height: 18
        radius: Theme.rFull
        y: 3
        x: sw.checked ? sw.width - width - 3 : 3
        color: sw.checked ? Theme.primaryText : Theme.outline

        Behavior on x { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }
    }

    MouseArea {
        anchors.fill: parent
        enabled: sw.toggleEnabled
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            sw.checked = !sw.checked
            sw.toggled(sw.checked)
        }
    }
}
