import QtQuick
import QtQuick.Layouts
import Studious

// 右上角的资产任务卡。
//
// 为什么要有它：下载资产以前只在设置页那一行变百分比，翻到别的页就看不到了，
// 装完也只是那一行突然变成"已安装"，用户不知道要等到什么时候才能用。
// 这里把一件任务从"下载 → 校验 → 解压 → 安装 → 可用"整条链摆出来，
// 折在最上面，用完自己消失。
//
// 数据源是后端的两张表：assetProgress（id → 百分比，-1 = 未知总量）和
// assetPhases（id → 阶段）。阶段由后端推进，解压/安装没有字节进度，只能靠文案。
Rectangle {
    id: panel

    readonly property var progressMap: backend.assetProgress
    readonly property var phaseMap: backend.assetPhases
    readonly property int taskCount: taskList.length
    readonly property int rowHeight: 46
    readonly property int collapsedRows: 3
    // 超过三件才需要"+N"和滚动；三件以内直接全摆出来。
    readonly property bool scrollable: taskCount > collapsedRows

    property bool expanded: false
    // [{ id, percent, phase }]，重建而不是增量改：任务来去都不频繁，
    // 少一套同步逻辑就少一处状态对不上。
    property var taskList: []

    function rebuild() {
        const progress = panel.progressMap
        const phases = panel.phaseMap
        const rows = []
        for (const id in progress) {
            rows.push({
                "id": id,
                "percent": Number(progress[id]),
                "phase": phases[id] !== undefined ? String(phases[id]) : "download"
            })
        }
        panel.taskList = rows
        if (rows.length <= panel.collapsedRows)
            panel.expanded = false
    }

    function phaseLabel(phase, percent) {
        switch (phase) {
        case "verify":
            return qsTr("校验中…")
        case "extract":
            return qsTr("解压中…")
        case "install":
            return qsTr("安装中…")
        case "import":
            return qsTr("导入中…")
        case "done":
            return qsTr("可用")
        case "failed":
            return qsTr("失败")
        default:
            return percent < 0 ? qsTr("下载中…") : qsTr("下载中 %1%").arg(percent)
        }
    }

    visible: taskCount > 0
    width: 344
    radius: Theme.rMd
    color: Theme.surfaceHigh
    border.width: 1
    border.color: Theme.outlineVariant

    implicitHeight: content.implicitHeight + Theme.s3 * 2

    Component.onCompleted: rebuild()

    Connections {
        target: backend
        function onAssetProgressChanged() { panel.rebuild() }
        function onAssetPhasesChanged() { panel.rebuild() }
    }

    ColumnLayout {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.s3
        spacing: Theme.s2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s2

            Text {
                Layout.fillWidth: true
                text: qsTr("资产任务 · %1").arg(panel.taskCount)
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                font.letterSpacing: 0.4
                color: Theme.inkVariant
                elide: Text.ElideRight
            }

            // "+N" 只在折叠状态出现：展开后按钮变成"收起"，
            // 否则一个按钮干两件事，用户得记着当前是哪种。
            AppButton {
                visible: panel.scrollable
                text: panel.expanded ? qsTr("收起")
                                     : qsTr("+%1").arg(panel.taskCount - panel.collapsedRows)
                variant: "text"
                fontPixelSize: Theme.fontSizeTiny
                implicitHeight: 22
                onClicked: panel.expanded = !panel.expanded
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.outlineVariant
        }

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.topMargin: Theme.s1
            // 用 preferredHeight 而不是 height：ColumnLayout 按子项隐式高度排版，
            // 直接写 height 会被压成 0（列表整块看不见）。
            Layout.preferredHeight: Math.min(panel.taskCount, panel.collapsedRows) * panel.rowHeight
            // 折叠时不给滚：多出来的只有"+N"那一个入口，
            // 不然用户在卡片上乱滚会以为列表坏了。
            interactive: panel.expanded && panel.scrollable
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            model: panel.taskList
            spacing: 0

            delegate: Item {
                id: row

                required property var modelData
                required property int index

                readonly property string phase: modelData.phase
                readonly property int percent: modelData.percent
                readonly property bool done: phase === "done"
                readonly property bool failed: phase === "failed"
                readonly property color accent: failed ? Theme.danger
                                              : done ? Theme.good
                                                     : Theme.primary

                width: list.width
                height: panel.rowHeight

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.s1

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s2

                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.id
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.ink
                            elide: Text.ElideMiddle
                        }

                        Text {
                            text: panel.phaseLabel(row.phase, row.percent)
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: row.failed ? Theme.danger
                                              : row.done ? Theme.good
                                                         : Theme.inkVariant
                        }
                    }

                    Rectangle {
                        id: track

                        Layout.fillWidth: true
                        height: 3
                        radius: 2
                        color: Theme.outlineVariant

                        Rectangle {
                            width: {
                                if (row.phase === "download" && row.percent >= 0)
                                    return track.width * Math.max(0, Math.min(1, row.percent / 100))
                                // 校验/解压/安装没有字节进度：条子铺满，
                                // 靠右边那行字说进行到哪一步。
                                return track.width
                            }
                            height: parent.height
                            radius: parent.radius
                            color: row.accent

                            Behavior on width {
                                NumberAnimation { duration: Theme.durFast }
                            }
                        }
                    }
                }
            }
        }
    }
}
