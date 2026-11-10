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
    // 三档以上、又放在整行宽卡片里时设 true：每项等分整条宽度，选中胶囊
    // 就是那一等分的整块（像标签栏），不会出现"文字挤在左、胶囊只有几个字宽"。
    // 默认 false —— FieldRow 里那些窄用法保持"宽度由文字决定"。
    property bool stretch: false
    // 这些 code 不给选：标签压暗、点了不发 selected，而是发 blocked(code)。
    // 让调用方决定"为什么不能选"怎么说（通常是缺资产，需要指路去资产页）。
    property var blockedCodes: []
    signal selected(string code)
    signal blocked(string code)

    implicitWidth: row.implicitWidth + Theme.s1 * 2
    implicitHeight: 32 + Theme.s1 * 2

    radius: Theme.rFull
    color: Theme.surfaceMid
    border.width: 1
    border.color: Theme.outlineVariant

    // 选中块是一颗会滑动的胶囊：位置跟着 seg.current 走，不在每个按钮上各自涂色。
    Rectangle {
        id: pill
        visible: false
        radius: seg.radius
        color: Theme.primaryContainer
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }

    // 按钮宽度由文字决定，不能靠"下标 × 等分宽"算位置，只能问布局要实际坐标。
    function syncPill() {
        var target = null
        for (var i = 0; i < row.children.length; ++i) {
            var c = row.children[i]
            if (c && c.modelData && c.modelData.code === seg.current) {
                target = c
                break
            }
        }
        if (!target) {
            pill.visible = false
            return
        }
        var p = target.mapToItem(seg, 0, 0)
        pill.x = p.x
        pill.y = p.y
        pill.width = target.width
        pill.height = target.height
        pill.visible = true
    }

    onCurrentChanged: Qt.callLater(syncPill)
    Component.onCompleted: Qt.callLater(syncPill)

    // 布局变化（父容器改宽、语言切换改文字）后按钮位置会变，需要重新对位。
    Connections {
        target: row

        function onWidthChanged() { Qt.callLater(seg.syncPill) }
    }

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
                // 缺资产这类"点了也没用"的项：压暗，但仍然可点（用来解释为什么不行）。
                readonly property bool isBlocked: seg.blockedCodes.indexOf(segBtn.modelData.code) >= 0

                // 圆点 + 间距也算进宽度，否则圆点会把标签挤偏。
                Layout.fillHeight: true
                Layout.fillWidth: seg.stretch
                Layout.preferredWidth: (segBtn.badgeShown ? 6 + Theme.s2 : 0)
                                       + segLabel.implicitWidth + Theme.s3 * 2
                radius: Theme.rFull
                // 选中态由上面那颗滑动胶囊表示，按钮自己保持透明。
                color: "transparent"

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
                        opacity: segBtn.isBlocked ? 0.42 : 1.0
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        // 不能选的项也接点击：调用方要用它弹一句"还差什么"。
                        if (segBtn.isBlocked) {
                            seg.blocked(segBtn.modelData.code)
                            return
                        }
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
