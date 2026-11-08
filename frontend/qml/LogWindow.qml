// 日志窗口：独立顶层窗口，由主窗口左上角 brand logo 双击打开。
//
// 为什么不挂 Popup / 为什么不继承主窗口的 transient parent：
//   主窗口最小化（甚至收进托盘）时日志还得能看 —— 排障基本都发生在那一刻。
//   日志来源是 AppBackend 的应用级日志汇（LogSink：qInstallMessageHandler 收的全部
//   qDebug/qInfo/qWarning/qCritical + 自己落盘的 studious.log），不是某个引擎的 tail。

// 接线只有两行（下面的 logText / logFilePath），换日志实现不用动这个窗口。

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Studious

Window {
    id: logWin

    // --ui-shot / C++ findChild 的锚点，跟 captureOverlay 一个路数。
    objectName: "logWindow"

    title: qsTr("Studious 日志")
    width: 900
    height: 600
    minimumWidth: 560
    minimumHeight: 360
    color: Theme.surface

    // 只挂 FramelessWindowHint：跟主窗口一样自绘标题条（系统原生标题栏会和
    // 这条工具条重复一行标题）。拖动 / 缩放见下面的 DragHandler。
    flags: Qt.Window | Qt.FramelessWindowHint
    // 关键：别跟着主窗口一起最小化。见文件头注释。
    transientParent: null

    // ---------------- 日志来源（唯一接线点）----------------
    readonly property string logText: backend.logText
    readonly property string logFilePath: backend.logFilePath

    // 「打开文件」用：Windows 路径的反斜杠要换成 /，非 ASCII 走 encodeURI。
    readonly property string logFileUrl: logFilePath.length > 0
                                         ? "file:///" + encodeURI(logFilePath.replace(/\\/g, "/"))
                                         : ""

    // ---------------- 跟随尾部 ----------------
    // follow = 用户希望跟着最新一行；pinned = 视图此刻确实贴着底。
    // 两个都为真才滚：用户手动往上翻之后 pinned 变假，新日志进来不抢滚动条。
    property bool follow: true
    property bool pinned: true

    // 「清空」不删文件（后端没有清日志的接口），只把已显示的部分从视图里摘掉：
    // 记下清空那一刻的结尾，之后只显示比它新的那一段。
    property string clearedTail: ""

    readonly property string viewText: {
        if (clearedTail.length === 0)
            return logText
        if (logText === clearedTail)
            return ""
        if (logText.endsWith(clearedTail))
            return logText.substring(0, logText.length - clearedTail.length)
        // 尾部已经滚出窗口范围，基线对不上了：整段重来，别显示半截错位的内容。
        return logText
    }

    // 行数按「视觉行」算：日志尾部一般以 \n 收尾，直接 split 会多算一个空行。
    readonly property int lineCount: {
        if (viewText.length === 0)
            return 0
        const body = viewText.endsWith("\n") ? viewText.substring(0, viewText.length - 1) : viewText
        return body.split("\n").length
    }

    // 贴底阈值 24px：滚轮一格约 40px，留点余量，免得「差两行」被判成离开底部。
    readonly property int bottomSlack: 24

    function isAtBottom() {
        return logFlick.contentY + logFlick.height >= logFlick.contentHeight - bottomSlack
    }

    function scrollToEnd() {
        logFlick.contentY = Math.max(0, logFlick.contentHeight - logFlick.height)
    }

    // 文本换完、高度算完才滚，否则拿到的是旧的 contentHeight（同 ConfigPage 的日志块）。
    function maybeFollow() {
        if (!follow || !pinned)
            return
        scrollToEnd()
    }

    onFollowChanged: {
        if (!follow)
            return
        // 重新打开跟随 = 用户想看最新，直接贴底，别停在半路。
        pinned = true
        Qt.callLater(scrollToEnd)
    }

    // 每次显示都贴到底：窗口可能关着好几分钟了。
    onVisibleChanged: {
        if (visible) {
            pinned = true
            Qt.callLater(scrollToEnd)
        }
    }

    Component.onCompleted: Qt.callLater(maybeFollow)

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------------- 标题栏（工具条）----------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.surfaceHigh

            Rectangle {
                width: parent.width
                height: 1
                anchors.bottom: parent.bottom
                color: Theme.outlineVariant
            }

            // 无边框窗口的拖动：整条标题栏都是把手。双击标题栏最大化也顺手接上。
            DragHandler {
                target: null
                onActiveChanged: if (active) logWin.startSystemMove()
            }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: logWin.visibility === Window.Maximized
                                ? logWin.showNormal() : logWin.showMaximized()
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s2
                spacing: Theme.s1

                Text {
                    Layout.fillWidth: true
                    text: qsTr("日志")
                    elide: Text.ElideRight
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                // 关掉之后滚轮停在哪就停在哪，新行不再把视图顶走。
                AppChip {
                    text: qsTr("跟随最新")
                    checked: logWin.follow
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    onToggled: logWin.follow = !logWin.follow
                }

                AppButton {
                    text: qsTr("复制")
                    variant: "text"
                    fontPixelSize: Theme.fontSizeSmall
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    buttonEnabled: logWin.viewText.length > 0
                    onClicked: backend.copyLogs()
                }

                AppButton {
                    text: qsTr("清空")
                    variant: "text"
                    fontPixelSize: Theme.fontSizeSmall
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    buttonEnabled: logWin.viewText.length > 0
                    onClicked: { logWin.clearedTail = ""; backend.clearLogs() }
                }

                AppButton {
                    text: qsTr("打开所在目录")
                    variant: "text"
                    fontPixelSize: Theme.fontSizeSmall
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    buttonEnabled: logWin.logFilePath.length > 0
                    onClicked: backend.openLogDir()
                }

                AppButton {
                    text: qsTr("打开文件")
                    variant: "text"
                    fontPixelSize: Theme.fontSizeSmall
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    buttonEnabled: logWin.logFilePath.length > 0
                    onClicked: backend.openLogFile()
                }

                AppButton {
                    text: "\u2715"
                    variant: "text"
                    fontPixelSize: Theme.fontSizeSmall
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: logWin.close()
                }
            }
        }

        // ---------------- 日志正文 ----------------
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.s3
            radius: Theme.rMd
            color: Theme.surfaceLow
            border.width: 1
            border.color: Theme.outlineVariant
            clip: true

            Flickable {
                id: logFlick
                anchors.fill: parent
                anchors.margins: Theme.s2
                clip: true
                contentWidth: width
                contentHeight: logEdit.paintedHeight + Theme.s2 * 2
                boundsBehavior: Flickable.StopAtBounds

                // 只在 contentY 真的变了时才重算「贴底」：
                // 新日志让 contentHeight 变长时 contentY 不动，pinned 保持原值 ——
                // 贴底的人继续贴底，翻上去的人继续停在原地。
                onContentYChanged: logWin.pinned = logWin.isAtBottom()

                TextEdit {
                    id: logEdit
                    x: Theme.s2
                    y: Theme.s2
                    width: logFlick.width - Theme.s2 * 2
                    text: logWin.viewText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.ink
                    selectionColor: Theme.primaryContainer
                    selectedTextColor: Theme.primaryContainerText

                    onTextChanged: Qt.callLater(logWin.maybeFollow)
                }

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            }
        }

        // ---------------- 底部状态条 ----------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 28

            Text {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s3
                text: qsTr("%1 行 · %2").arg(logWin.lineCount)
                                            .arg(logWin.logFilePath.length > 0
                                                 ? logWin.logFilePath
                                                 : qsTr("（还没有日志文件）"))
                elide: Text.ElideRight
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.inkVariant
            }
        }
    }
}
