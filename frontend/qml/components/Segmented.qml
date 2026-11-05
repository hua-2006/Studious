import QtQuick
import QtQuick.Layouts
import Studious

// 互斥分段控件（模板 .seg）。
Rectangle {
    id: seg

    property var items: []
    property string current: ""
    signal selected(string code)

    implicitWidth: row.implicitWidth + Theme.s1 * 2
    implicitHeight: 32 + Theme.s1 * 2

    radius: Theme.rFull
    color: Theme.surfaceMid
    border.width: 1
    border.color: Theme.outlineVariant

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.s1
        spacing: 2

        Repeater {
            model: seg.items

            delegate: Rectangle {
                id: segBtn
                required property var modelData

                Layout.fillHeight: true
                Layout.preferredWidth: segLabel.implicitWidth + Theme.s3 * 2
                radius: Theme.rFull
                color: seg.current === modelData.code ? Theme.primaryContainer : "transparent"

                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                Text {
                    id: segLabel
                    anchors.centerIn: parent
                    text: segBtn.modelData.label
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: seg.current === segBtn.modelData.code ? Font.DemiBold : Font.Normal
                    color: seg.current === segBtn.modelData.code
                           ? Theme.primaryContainerText : Theme.inkVariant
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (seg.current !== segBtn.modelData.code) {
                            seg.current = segBtn.modelData.code
                            seg.selected(segBtn.modelData.code)
                        }
                    }
                }
            }
        }
    }
}
