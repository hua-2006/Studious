import QtQuick
import Studious

// 小圆角标签（模板 .badge）。
Rectangle {
    id: badge

    property string text: ""
    property bool filled: false
    // "neutral"（默认）| "danger"：缺东西的那种提示用 danger，别跟普通标签混在一起。
    property string tone: "neutral"

    readonly property bool isDanger: tone === "danger"

    implicitWidth: label.implicitWidth + Theme.s2 * 2
    implicitHeight: 20
    radius: Theme.rFull
    color: {
        if (badge.isDanger)
            return badge.filled ? Theme.danger : "transparent"
        return badge.filled ? Theme.primaryContainer : "transparent"
    }
    border.width: filled ? 0 : 1
    border.color: badge.isDanger ? Theme.danger : Theme.outlineVariant

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: {
            if (badge.isDanger)
                return badge.filled ? Theme.dangerText : Theme.danger
            return badge.filled ? Theme.primaryContainerText : Theme.inkVariant
        }
    }
}
