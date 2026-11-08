import QtQuick
import Studious

// 「未实现」标记。
//
// 用法：在未接后端的控件旁边放一个 `Stub { }`。
// 删除某个标记 = 删掉那一行 `Stub { }`。
// 想一次性隐藏全部标记（比如截图给外人看）= 把 Theme.showStubs 设成 false。
Rectangle {
    id: stub

    visible: Theme.showStubs
    implicitWidth: stubLabel.implicitWidth + Theme.s2 * 2
    implicitHeight: 18
    radius: Theme.rFull
    color: "transparent"
    border.width: 1
    border.color: Theme.caution

    Text {
        id: stubLabel
        anchors.centerIn: parent
        text: qsTr("即将支持")
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: Theme.caution
    }
}
