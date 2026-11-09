import QtQuick
import QtQuick.Layouts
import Studious

// 「标签 + 控件」一行，标签列宽统一。
//
// hint 非空时，控件**下方**多一行小字（左对齐到控件列，缩进 labelWidth + s3）。
// hint 为空时 hint 那个 Text 不可见，Layout 不占位，布局跟原来一模一样。
ColumnLayout {
    id: field

    property string label: ""
    property int labelWidth: Theme.formLabelWidth
    property string hint: ""
    property color hintColor: Theme.inkVariant
    default property alias content: slot.data

    Layout.fillWidth: true
    // 行内提示与上面那一行之间的间距；只有一行时 spacing 不影响布局。
    spacing: Theme.s1

    RowLayout {
        id: row
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

    Text {
        visible: field.hint.length > 0
        text: field.hint
        Layout.leftMargin: field.labelWidth + Theme.s3
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        color: field.hintColor
    }
}
