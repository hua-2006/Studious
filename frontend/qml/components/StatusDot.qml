import QtQuick
import Studious

// 状态点 + 名称 + 说明。
Row {
    id: dot

    property string label: ""
    property string detail: ""
    // 0 = 就绪 1 = 警告 2 = 错误 3 = 未就绪
    property int state: 3
    // 说明文字的上限宽度。四个引擎并排时后端可能给出很长的原因
    //（"本地 MT 需要明确的源语言（不支持自动检测后再选模型）"），
    // 不设上限会把整条状态栏撑出屏幕，这里改成超长省略。
    property real detailMaxWidth: 220

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
        width: Math.min(implicitWidth, dot.detailMaxWidth)
        elide: Text.ElideRight
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.inkVariant
    }
}
