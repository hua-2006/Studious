import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Studious

// 窗内下拉外壳（**不是** Popup）。
//
// 为什么不用 Popup：截屏/实时/小窗这些浮层窗口本身就很小，
// Popup 会开成独立窗口并被裁掉；所以下拉只能画在窗口里、由调用方决定位置。
//
// 用法（在页面里）：
//   MenuPanel {
//       id: engineMenu
//       visible: false
//       width: 220
//       x / y 由调用方按锚点算
//       MenuRow { text: ...; selected: ...; dotColor: ...; onActivated: ... }
//   }
Rectangle {
    id: panel

    // 超过这个高度就滚动，不再往下长。
    property int maxHeight: 360
    // 内容：往这里放 MenuRow（也接受任意 Item / 分隔线）。
    default property alias content: column.data
    // 内容实际需要的高度（不含内边距），调用方要自己算 y 时可以用。
    readonly property int contentHeight: column.implicitHeight

    implicitWidth: Math.max(column.implicitWidth + Theme.s1 * 2, 168)
    // 只有内容真超了才滚动：内容短的时候高度贴着内容走。
    implicitHeight: Math.min(column.implicitHeight, maxHeight) + Theme.s1 * 2

    radius: Theme.rMd
    color: Theme.surfaceHigh
    border.width: 1
    border.color: Theme.outlineVariant
    clip: true

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.s1
        contentWidth: width
        contentHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        // 内容装得下时不吃滚轮，免得滚动事件被这里吞掉。
        interactive: contentHeight > height

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            width: flick.width
            // 行与行之间留 2px：悬浮圆角贴在一起会显得黏。
            spacing: 2
        }
    }
}
