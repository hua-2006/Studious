import QtQuick
import QtQuick.Layouts
import Studious

// 带标题的卡片容器（模板 .card）。
Rectangle {
    id: card

    property string title: ""
    property string hint: ""
    default property alias content: body.data
    property alias headerActions: actions.data

    implicitHeight: column.implicitHeight + Theme.s5 * 2
    radius: Theme.rLg
    color: Theme.surfaceLow
    border.width: 1
    border.color: Theme.outlineVariant

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.s5
        spacing: Theme.s3

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2

            Text {
                text: card.title
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
            }
            Item { Layout.fillWidth: true }
            Row { id: actions; spacing: Theme.s2 }
        }

        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: card.hint
            wrapMode: Text.WordWrap
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.inkVariant
        }

        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: Theme.s3
        }
    }
}
