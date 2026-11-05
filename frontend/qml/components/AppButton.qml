import QtQuick
import Studious

// 通用按钮（模板 .btn / .btn.tonal / .btn.text）。
Rectangle {
    id: button

    property string text: ""
    // outline | tonal | primary | text | danger
    property string variant: "outline"
    property bool buttonEnabled: true
    signal clicked()

    readonly property bool isPrimary: variant === "primary"
    readonly property bool isTonal: variant === "tonal"
    readonly property bool isText: variant === "text"
    readonly property bool isDanger: variant === "danger"

    implicitWidth: label.implicitWidth + (isText ? Theme.s2 : Theme.s4) * 2
    implicitHeight: 34
    radius: Theme.rSm
    color: !buttonEnabled ? "transparent"
                          : isPrimary ? (hover.hovered ? Theme.primaryContainer : Theme.primary)
                          : isTonal ? Theme.surfaceHighest
                          : isText ? (hover.hovered ? Theme.surfaceHigh : "transparent")
                          : isDanger ? (hover.hovered ? Theme.dangerContainer : "transparent")
                          : (hover.hovered ? Theme.surfaceHigh : Theme.surface)
    border.width: (isPrimary || isText) ? 0 : 1
    border.color: isDanger ? Theme.danger : Theme.outlineVariant
    opacity: buttonEnabled ? 1.0 : 0.45

    Behavior on color { ColorAnimation { duration: Theme.durFast } }

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        font.weight: button.isPrimary ? Font.DemiBold : Font.Normal
        color: button.isPrimary ? Theme.primaryText
               : button.isDanger ? Theme.danger
                                 : Theme.ink
    }

    HoverHandler { id: hover; enabled: button.buttonEnabled }

    MouseArea {
        anchors.fill: parent
        enabled: button.buttonEnabled
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
