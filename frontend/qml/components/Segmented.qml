import QtQuick
import QtQuick.Layouts
import Studious

// 互斥分段控件（模板 .seg）。
Rectangle {
    id: seg

    property var items: []
    property string current: ""
    // code -> 颜色（Theme.good / Theme.caution / Theme.danger 之类）。
    // 给了就在标签左边画一个 6px 圆点；不给就完全不画，其它页面用法不变。
    property var badges: ({})
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

                // 当前这项有没有状态点。值可能是 undefined / null / 空串，都算「没有」。
                readonly property var badgeEntry: seg.badges ? seg.badges[segBtn.modelData.code] : undefined
                readonly property bool badgeShown: segBtn.badgeEntry !== undefined
                                                   && segBtn.badgeEntry !== null
                                                   && segBtn.badgeEntry !== ""

                // 圆点 + 间距也算进宽度，否则圆点会把标签挤偏。
                Layout.fillHeight: true
                Layout.preferredWidth: (segBtn.badgeShown ? 6 + Theme.s2 : 0)
                                       + segLabel.implicitWidth + Theme.s3 * 2
                radius: Theme.rFull
                color: seg.current === modelData.code ? Theme.primaryContainer : "transparent"

                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                RowLayout {
                    id: segContent
                    anchors.centerIn: parent
                    spacing: Theme.s2

                    // 状态点：只在 badges 里给了颜色时才出现（宽度也只在可见时算）。
                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        visible: segBtn.badgeShown
                        implicitWidth: 6
                        implicitHeight: 6
                        radius: 3
                        color: segBtn.badgeShown ? segBtn.badgeEntry : "transparent"
                    }

                    Text {
                        id: segLabel
                        Layout.alignment: Qt.AlignVCenter
                        text: segBtn.modelData.label
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: seg.current === segBtn.modelData.code ? Font.DemiBold : Font.Normal
                        color: seg.current === segBtn.modelData.code
                               ? Theme.primaryContainerText : Theme.inkVariant
                    }
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
