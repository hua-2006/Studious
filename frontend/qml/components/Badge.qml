import QtQuick
import Studious

// 小圆角标签（模板 .badge）。
Rectangle {
    id: badge

    property string text: ""
    property bool filled: false
    // "neutral"（默认）| "danger"（缺东西）| "good"（已装好/可用，绿色）
    property string tone: "neutral"

    readonly property bool isDanger: tone === "danger"
    readonly property bool isGood: tone === "good"

    implicitWidth: label.implicitWidth + Theme.s2 * 2
    implicitHeight: 20
    radius: Theme.rFull
    color: {
        if (badge.isDanger)
            return badge.filled ? Theme.danger : "transparent"
        if (badge.isGood)
            return badge.filled ? Theme.goodContainer : "transparent"
        return badge.filled ? Theme.primaryContainer : "transparent"
    }
    border.width: filled ? 0 : 1
    border.color: badge.isDanger ? Theme.danger
                : badge.isGood ? Theme.good
                : Theme.outlineVariant

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: {
            if (badge.isDanger)
                return badge.filled ? Theme.dangerText : Theme.danger
            if (badge.isGood)
                return Theme.good
            return badge.filled ? Theme.primaryContainerText : Theme.inkVariant
        }
    }
}
