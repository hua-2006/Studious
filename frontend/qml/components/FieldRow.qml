import QtQuick
import QtQuick.Layouts
import Studious

// 「标签 + 控件」一行，标签列宽统一。
RowLayout {
    id: field

    property string label: ""
    property int labelWidth: 132
    default property alias content: slot.data

    Layout.fillWidth: true
    spacing: Theme.s3

    Text {
        text: field.label
        Layout.preferredWidth: field.labelWidth
        Layout.alignment: Qt.AlignVCenter
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        color: Theme.ink
        elide: Text.ElideRight
    }

    RowLayout {
        id: slot
        Layout.fillWidth: true
        spacing: Theme.s2
    }
}
