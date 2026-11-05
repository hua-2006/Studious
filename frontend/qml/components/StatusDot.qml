import QtQuick
import Studious

// 状态点 + 名称 + 说明。
Row {
    id: dot

    property string label: ""
    property string detail: ""
    // 0 = 就绪 1 = 警告 2 = 错误 3 = 未就绪
    property int state: 3

    readonly property color dotColor: state === 0 ? Theme.good
                                     : state === 1 ? Theme.caution
                                     : state === 2 ? Theme.danger
                                                   : Theme.outlineVariant

    spacing: Theme.s2

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 8
        height: 8
        radius: 4
        color: dot.dotColor
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: dot.label
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        font.weight: Font.DemiBold
        color: Theme.ink
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        visible: text.length > 0
        text: dot.detail
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.inkVariant
    }
}
