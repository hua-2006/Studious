import QtQuick
import Studious

// 轻量提示条：从下方浮上来、停一会儿、再淡出去。
//
// 用来回答「点了不该点的东西」——比如引擎还没装齐就去点它。
// 刻意不做成 Popup：Popup 是独立窗口，会抢焦点，而这里只是解释一句话，
// 不应该打断用户手上的操作。
Rectangle {
    id: toast

    property string text: ""
    // 停留多久（毫秒）之后开始淡出。
    property int holdMs: 2600

    readonly property bool active: opacity > 0.001

    signal hidden()

    function showMessage(message) {
        if (message.length === 0)
            return
        toast.text = message
        enterAnim.restart()
        holdTimer.restart()
    }

    function dismiss() {
        holdTimer.stop()
        exitAnim.restart()
    }

    width: Math.min(implicitWidth, 520)
    implicitWidth: toastLabel.implicitWidth + Theme.s4 * 2
    height: toastLabel.implicitHeight + Theme.s3
    radius: Theme.rMd
    color: Theme.surfaceHighest
    border.width: 1
    border.color: Theme.outlineVariant
    opacity: 0
    visible: opacity > 0.001
    z: 900
    // 从下方 8px 处升上来，收起时再落回去 —— 只是提示，不要有"弹窗感"。
    transform: Translate { id: toastShift }

    Text {
        id: toastLabel
        anchors.centerIn: parent
        width: Math.min(implicitWidth, 520 - Theme.s4 * 2)
        text: toast.text
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.ink
    }

    ParallelAnimation {
        id: enterAnim

        NumberAnimation {
            target: toast
            property: "opacity"
            from: 0
            to: 1
            duration: 180
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: toastShift
            property: "y"
            from: 10
            to: 0
            duration: 200
            easing.type: Easing.OutCubic
        }
    }

    ParallelAnimation {
        id: exitAnim

        NumberAnimation {
            target: toast
            property: "opacity"
            to: 0
            duration: 260
            easing.type: Easing.InCubic
        }
        NumberAnimation {
            target: toastShift
            property: "y"
            to: 6
            duration: 260
            easing.type: Easing.InCubic
        }
        onFinished: toast.hidden()
    }

    Timer {
        id: holdTimer
        interval: toast.holdMs
        onTriggered: toast.dismiss()
    }
}
