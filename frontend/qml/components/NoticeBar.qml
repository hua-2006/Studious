import QtQuick
import QtQuick.Layouts
import Studious

// 全局提示横幅（错误 / 警告 / 信息）。
//
// 用法（显隐由外部控制，组件本身不自己计时）：
//   NoticeBar {
//       title: "…"; detail: "…"; tone: "error"
//       shown: true
//       onDismissed: shown = false
//   }
//
// 设计取舍：
//   · 只读 Theme 令牌，颜色全走语义色（danger / caution / primary）三套 container + 强调色，
//     深色主题下 container 会自动跟 surface 混色，不用这里再判断。
//   · 高度按内容自适应：标题一行 + detail 最多三行（超长省略）。
//   · 未显示时 visible = false：不占鼠标、不参与父布局的 implicitHeight。
Rectangle {
    id: bar

    // ---- 外部接口 ----
    property string title: ""
    property string detail: ""
    property bool shown: false
    // "error" | "warning" | "info"
    property string tone: "error"

    // 点 ✕：外部应当把 shown 置 false。
    signal dismissed()
    // 点整条（✕ 以外）：外部可用来「去处理」。
    signal activated()

    // ---- 色调 ----
    readonly property string toneKey: (tone === "warning" || tone === "info") ? tone : "error"
    readonly property color accent: toneKey === "warning" ? Theme.caution
                                    : toneKey === "info" ? Theme.primary
                                                         : Theme.danger
    readonly property color containerColor: toneKey === "warning" ? Theme.cautionContainer
                                            : toneKey === "info" ? Theme.primaryContainer
                                                                 : Theme.dangerContainer
    readonly property color textColor: toneKey === "warning" ? Theme.caution
                                       : toneKey === "info" ? Theme.primaryContainerText
                                                            : Theme.dangerContainerText

    implicitHeight: body.implicitHeight + Theme.s3 * 2
    radius: Theme.rMd
    color: bar.containerColor
    border.width: 1
    border.color: bar.accent

    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0
    enabled: shown

    Behavior on opacity { NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic } }

    // 整条可点：放在最底层，内容与 ✕ 都在它上面。
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: bar.activated()
    }

    // 左侧 3px 竖条：只圆左边两角，跟着外框的圆角走。
    Rectangle {
        x: 1
        y: 1
        width: 3
        height: parent.height - 2
        topLeftRadius: bar.radius
        bottomLeftRadius: bar.radius
        color: bar.accent
    }

    RowLayout {
        id: body
        anchors.fill: parent
        anchors.leftMargin: Theme.s3 + Theme.s2
        anchors.rightMargin: Theme.s2
        anchors.topMargin: Theme.s3
        anchors.bottomMargin: Theme.s3
        spacing: Theme.s3

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s1

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: bar.title
                elide: Text.ElideRight
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Font.DemiBold
                color: bar.textColor
            }

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: bar.detail
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: bar.textColor
                opacity: 0.92
            }
        }

        // 右上角关闭。单独的 MouseArea 盖住整条那个，点它不会触发 activated()。
        Item {
            id: closeButton
            Layout.alignment: Qt.AlignTop | Qt.AlignRight
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22

            Rectangle {
                anchors.fill: parent
                radius: Theme.rFull
                color: bar.accent
                opacity: closeHover.hovered ? 0.16 : 0.0
                Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            }

            Text {
                anchors.centerIn: parent
                text: "\u2715"
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: bar.textColor
            }

            HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.dismissed()
            }
        }
    }
}
