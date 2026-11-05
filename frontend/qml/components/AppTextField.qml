import QtQuick
import QtQuick.Controls.Basic
import Studious

// 统一外观的单行输入框。
//
// 为什么要包一层：Qt Quick Controls 的 Basic 样式里，TextField 的背景和文字色
// 取自控件自身的 palette；应用级 QPalette 在本机没覆盖住（系统是深色），
// 结果就是「黑底白字」。这里显式自绘背景与文字色，绕开调色板。
TextField {
    id: field

    implicitHeight: 34
    leftPadding: Theme.s2 + 2
    rightPadding: Theme.s2 + 2
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
    color: Theme.ink
    placeholderTextColor: Theme.outline
    selectByMouse: true
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: Theme.rSm
        color: Theme.surface
        border.width: 1
        border.color: field.activeFocus ? Theme.primary : Theme.outlineVariant
    }
}
