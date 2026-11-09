import QtQuick
import Studious

// 统一的 ✕：22 × 26、无边框、悬浮才出台面色、字形 ✕ 用 inkVariant。
//
// 三处浮层（截图 / 小窗 / 设置面板）和实时控制条原本各写一份，尺寸与圆角已经
// 开始漂移（rXs vs rSm、有无颜色过渡）。差异用属性表达，别再复制代码：
//   · 实时控制条：buttonRadius: Theme.rXs、animated: false（它的行高跟着内容走）
//   · 其它三处：默认值即可

Rectangle {
    id: cb

    signal clicked()

    property bool animated: true
    property int buttonRadius: Theme.rSm

    implicitWidth: 22
    implicitHeight: 26
    width: 22
    height: 26
    radius: cb.buttonRadius
    color: cbHover.hovered ? Theme.surfaceHighest : "transparent"

    Behavior on color {
        enabled: cb.animated
        ColorAnimation { duration: Theme.durFast }
    }

    Text {
        anchors.centerIn: parent
        text: "✕"
        color: Theme.inkVariant
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
    }

    HoverHandler {
        id: cbHover
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        onTapped: cb.clicked()
    }
}
