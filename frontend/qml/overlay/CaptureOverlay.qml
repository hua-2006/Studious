// A 页：截屏翻译的遮罩与结果浮层（对应 docs/TempUI/flows.html 的 A1–A10）。
//
// 一个窗口承担两个阶段，和设计稿一致：
//   · Selecting  —— 全屏压暗、选框露出原图、八手柄、尺寸标签、工具栏（翻译 / 重新框选 / 取消）
//   · Recognizing / Translating —— 选区内 busy 框
//   · Showing / ShowingOriginal —— 译文块盖在原文行上（只盖文字那几行，不是整块压低）
//   · Error —— 选区内 err 框 + 重新翻译 / 关闭
//
// 为什么不做成两个窗口：设计稿里译文块就是贴在原位置上的，选区一消失就失去参照；
// 同一个窗口还能保证 Esc / 失焦的行为只有一套。
import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls
import Studious

Window {
    id: overlay

    // 截图工具（--ui-shot）靠这个找它。objectName 也是 QML 里唯一能被 C++ findChild 认的锚点。
    objectName: "captureOverlay"

    // 逻辑像素下整个虚拟桌面的矩形（由 Main.qml 传进来，避免这里再算一次多屏）
    property rect desktopRect: Qt.rect(0, 0, 1280, 800)
    property bool selecting: screenFlow.phase === "Selecting"
    property bool working: screenFlow.phase === "Recognizing" || screenFlow.phase === "Translating"
    property bool showing: screenFlow.phase === "Showing" || screenFlow.phase === "ShowingOriginal"
    property bool errored: screenFlow.phase === "Error"
    property bool engaged: selecting || working || showing || errored

    // 选区（逻辑像素，相对本窗口）。跟着 ScreenFlow 走，重新框选时清空。
    // ScreenFlow 的 selectionRect 要等 confirmSelection 才有值；确认之前用本地框，
    // 这样译文块、工具栏在「已选未译」阶段也有正确的参照。
    readonly property rect sel: screenFlow.selectionRect.width > 0
                                ? screenFlow.selectionRect
                                : localRect

    // 拖拽中的临时框（松手前只在本窗口内有效）
    property real dragX: 0
    property real dragY: 0
    property bool dragging: false
    property bool confirmed: false
    // 引擎下拉是否展开。用窗内面板而不是 Popup —— 见文件末尾 engineMenu 的注释。
    property bool menuOpen: false
    // 松手后的框（还没送去翻译）。ScreenFlow 的 selectionRect 要等 confirmSelection 才有值，
    // 所以「已选、等确认」这个阶段必须自己留一份。
    property rect localRect: Qt.rect(0, 0, 0, 0)

    // 放大镜跟随的光标位置（窗口逻辑坐标；-1 = 光标还没进窗口）。
    // 用鼠标自己报的坐标，别用 HoverHandler.point.position —— 浮层是 NOACTIVATE 窗口，
    // hover 事件在没激活的窗口上不一定来。
    property point cursorPos: Qt.point(-1, -1)

    readonly property real scale: screenFlow.pixelScale > 0 ? screenFlow.pixelScale : 1.0
    readonly property bool showOriginal: screenFlow.showOriginal

    // 工具栏上的引擎槽只显示短名：引擎的完整状态在配置页里看。
    function engineShortName() {
        switch (backend.engineId) {
        case "web":
            return "Web"
        case "llm-cloud":
            return qsTr("云端")
        case "llm-local":
            return qsTr("本地 LLM")
        case "local-mt":
            return qsTr("本地 MT")
        default:
            return qsTr("自动")
        }
    }

    // 引擎选项与配置页、翻译页用的是同一批 id（空串 = 自动，按优先级降级）。
    readonly property var engineChoices: [
        { code: "", label: qsTr("自动") },
        { code: "web", label: "Web" },
        { code: "llm-cloud", label: qsTr("云端 LLM") },
        { code: "llm-local", label: qsTr("本地 LLM") },
        { code: "local-mt", label: qsTr("本地 MT") }
    ]

    x: desktopRect.x
    y: desktopRect.y
    width: desktopRect.width
    height: desktopRect.height

    visible: engaged
    color: "transparent"
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.NoDropShadowWindowHint
    // 截屏结果窗口不能用 Qt.Popup（会自动关掉），也不需要标题栏。
    title: qsTr("截屏翻译")

    // 关窗口 = 取消这一次（等同于 Esc）。
    onClosing: function (close) {
        close.accepted = true;
        screenFlow.cancel();
    }

    // 选区在哪就用哪儿；拖拽中优先用临时框。
    readonly property rect liveRect: dragging
                                       ? Qt.rect(Math.min(dragX, mouseArea.mouseX),
                                                 Math.min(dragY, mouseArea.mouseY),
                                                 Math.abs(mouseArea.mouseX - dragX),
                                                 Math.abs(mouseArea.mouseY - dragY))
                                       : (localRect.width > 0 ? localRect : sel)

    // ---------------------------------------------------------------- 底图
    Image {
        id: shotImage
        anchors.fill: parent
        // C++ 每次截屏落一个临时 PNG（QML 的 Image 吃不了 QImage）。
        // 依赖 screenSize 是必须的：imageSourceUrl() 不是属性，光靠它不会重新求值。
        source: {
            screenFlow.screenSize;
            return screenFlow.imageSourceUrl();
        }
        fillMode: Image.PreserveAspectFit
        asynchronous: false
        cache: false
        smooth: true
    }

    // ---------------------------------------------------------------- 压暗（四块，中间镂空）
    // 不能用一个半透明大矩形 + 选区反色：那样选区里的原图会被压暗两次，
    // 而设计稿要求「选框露出原图」。
    readonly property color dimColor: Qt.rgba(0, 0, 0, 0.45)
    Rectangle { // 上
        x: 0; y: 0
        width: overlay.width
        height: Math.max(0, overlay.liveRect.y)
        color: overlay.dimColor
        visible: !overlay.showing
    }
    Rectangle { // 下
        x: 0
        y: overlay.liveRect.y + overlay.liveRect.height
        width: overlay.width
        height: Math.max(0, overlay.height - y)
        color: overlay.dimColor
        visible: !overlay.showing
    }
    Rectangle { // 左
        x: 0
        y: overlay.liveRect.y
        width: Math.max(0, overlay.liveRect.x)
        height: overlay.liveRect.height
        color: overlay.dimColor
        visible: !overlay.showing
    }
    Rectangle { // 右
        x: overlay.liveRect.x + overlay.liveRect.width
        y: overlay.liveRect.y
        width: Math.max(0, overlay.width - x)
        height: overlay.liveRect.height
        color: overlay.dimColor
        visible: !overlay.showing
    }

    // ---------------------------------------------------------------- 放大镜（微信截图那种）
    // 框选时贴在光标右下方：上面是放大视图 + 十字线，下面是坐标、色值和 RGB。
    // 放大是把整屏底图按倍率缩放后用 clip 露出光标附近一块（Qt 6 的 Image 没有 sourceRect）。
    // 倍率来自设置 backend.overlayZoom（2~8 倍），不写死在这里。
    Rectangle {
        id: magnifier
        objectName: "magnifier"

        readonly property real zoomSize: 104          // 放大区边长（逻辑像素）
        readonly property real zoom: Math.max(2, backend.overlayZoom)
        readonly property int px: Math.round(overlay.cursorPos.x * overlay.scale)
        readonly property int py: Math.round(overlay.cursorPos.y * overlay.scale)
        readonly property var info: screenFlow.pixelInfoAt(px, py)
        readonly property color swatch: info.ok ? info.hex : Theme.surfaceHighest

        // 只在「正在拖框」时出现：松开手、选区定了就收起来，别一直挡着画面。
        visible: overlay.dragging && overlay.cursorPos.x >= 0 && overlay.cursorPos.y >= 0
        // z 压到工具栏（5）之下：放大镜跟着光标跑，难免压到工具栏上，
        // 让「翻译」这类主按钮永远压在它上面，别把操作挡了。
        z: 4
        // 宽度要装得下 "R249 G159 B255" 这种一行 RGB，太窄会截断。
        width: Math.max(zoomSize, 132)
        height: zoomSize + infoColumn.implicitHeight + Theme.s2 * 3
        radius: Theme.rSm
        color: Qt.rgba(Theme.surfaceHigh.r, Theme.surfaceHigh.g, Theme.surfaceHigh.b, 0.97)
        border.width: 1
        border.color: Theme.outlineVariant

        // 贴着光标右下角；右边 / 下边放不下就翻到另一侧，始终留在屏内。
        x: {
            const right = overlay.cursorPos.x + Theme.s3;
            return right + width + Theme.s2 <= overlay.width
                    ? right
                    : Math.max(Theme.s2, overlay.cursorPos.x - width - Theme.s3);
        }
        y: {
            const down = overlay.cursorPos.y + Theme.s3;
            return down + height + Theme.s2 <= overlay.height
                    ? down
                    : Math.max(Theme.s2, overlay.cursorPos.y - height - Theme.s3);
        }

        // ---- 放大区 ----
        Item {
            id: zoomBox
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.s1
            height: magnifier.zoomSize - Theme.s1 * 2
            clip: true

            // Qt 6 的 Image 没有 sourceRect，所以不能「只画一小块源图」。
            // 换成：把整屏底图按 zoom 倍放大，再用 clip 只露出光标附近那一小块。
            // 逻辑坐标 × zoom 就是放大图里的坐标，把它平移到视口中心即可。
            Image {
                width: overlay.width * magnifier.zoom
                height: overlay.height * magnifier.zoom
                x: -(overlay.cursorPos.x * magnifier.zoom) + zoomBox.width / 2
                y: -(overlay.cursorPos.y * magnifier.zoom) + zoomBox.height / 2
                source: shotImage.source
                fillMode: Image.Stretch
                // 平滑插值：要看的是「这块区域写了什么」，不是逐像素色块。
                smooth: true
                cache: false
            }

            // 十字线：正中那一行 / 一列
            Rectangle {
                width: 1
                height: parent.height
                x: Math.round(parent.width / 2)
                color: Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.85)
            }
            Rectangle {
                width: parent.width
                height: 1
                y: Math.round(parent.height / 2)
                color: Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.85)
            }

            // 中心小圆点：标出取色的那一格，比 1 像素描边框更不扎眼
            Rectangle {
                readonly property real unit: Math.max(2, magnifier.zoom / overlay.scale)
                width: unit
                height: unit
                x: Math.round(parent.width / 2 - width / 2)
                y: Math.round(parent.height / 2 - height / 2)
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Theme.primary
            }
        }

        // ---- 坐标 + 色值 ----
        Column {
            id: infoColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: zoomBox.bottom
            anchors.topMargin: Theme.s1
            anchors.leftMargin: Theme.s2
            anchors.rightMargin: Theme.s2
            spacing: Theme.s1

            // 物理像素坐标，跟别的截图工具（含微信）对得上。
            Text {
                width: parent.width
                text: magnifier.info.ok
                      ? ("X " + magnifier.px + "   Y " + magnifier.py)
                      : qsTr("光标不在画面内")
                color: Theme.ink
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeTiny
                elide: Text.ElideRight
            }

            Row {
                spacing: Theme.s2

                Rectangle {
                    width: 13
                    height: 13
                    radius: Theme.rXs
                    color: magnifier.swatch
                    border.width: 1
                    border.color: Theme.outline
                }

                Text {
                    text: magnifier.info.ok ? magnifier.info.hex : "—"
                    color: Theme.inkVariant
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeTiny
                }
            }

            Text {
                visible: magnifier.info.ok
                text: magnifier.info.ok
                      ? ("R" + magnifier.info.r + " G" + magnifier.info.g + " B" + magnifier.info.b)
                      : ""
                color: Theme.inkVariant
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeTiny
            }
        }
    }

    // ---------------------------------------------------------------- 选区框 + 手柄
    Rectangle {
        id: selectionBox
        x: overlay.liveRect.x
        y: overlay.liveRect.y
        width: overlay.liveRect.width
        height: overlay.liveRect.height
        color: "transparent"
        border.width: 2
        border.color: Theme.primary
        visible: !overlay.showing && overlay.liveRect.width > 1 && overlay.liveRect.height > 1

        // 尺寸标签贴在框外上方；贴上边时翻到框内，免得跑出屏幕
        Rectangle {
            id: sizeTag
            readonly property bool above: selectionBox.y > 28
            x: 0
            y: above ? -height - Theme.s2 : Theme.s2
            width: sizeLabel.implicitWidth + Theme.s3 * 2
            height: sizeLabel.implicitHeight + Theme.s1 * 2
            radius: Theme.rXs
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
            z: 3
            Text {
                id: sizeLabel
                anchors.centerIn: parent
                text: Math.round(selectionBox.width) + " × " + Math.round(selectionBox.height)
                color: Theme.ink
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        // 长按框内任意位置拖动整块选区（不必重新框选）。
        // z 压在八向手柄（4）之下，所以贴边那圈还是改尺寸，中间才是搬家。
        MouseArea {
            id: moveArea
            anchors.fill: parent
            anchors.margins: 4
            enabled: overlay.resizable
            acceptedButtons: Qt.LeftButton
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.SizeAllCursor
            onPressed: function (mouse) {
                overlay.moveRect = overlay.liveRect;
                overlay.moveStart = mapToItem(overlay.contentItem, mouse.x, mouse.y);
                mouse.accepted = true;
            }
            onPositionChanged: function (mouse) {
                if (!pressed || overlay.moveRect.width <= 0)
                    return;
                const p = mapToItem(overlay.contentItem, mouse.x, mouse.y);
                overlay.applyMove(p.x - overlay.moveStart.x, p.y - overlay.moveStart.y);
            }
            onReleased: {
                overlay.moveRect = Qt.rect(0, 0, 0, 0);
                overlay.moveStart = Qt.point(0, 0);
            }
        }
        // 八个手柄：框定之后仍然可用 —— 拖它做二次微调，不必重新框选。
        // 用 index + 页面级 handleSpecs（JS 对象数组当 model 时，delegate 里的
        // required property var modelData 在这版 Qt 上取不到值，手柄会静默不显示）。
        Repeater {
            model: 8
            delegate: Rectangle {
                required property int index
                readonly property var spec: overlay.handleSpecs[index]

                // 命中区比看得见的方块大一圈，8px 的方块太难点中
                width: 14
                height: 14
                color: "transparent"
                x: selectionBox.width * spec.h - width / 2
                y: selectionBox.height * spec.v - height / 2
                visible: overlay.resizable
                z: 4

                Rectangle {
                    anchors.centerIn: parent
                    width: 8
                    height: 8
                    radius: 3
                    color: Theme.primary
                    border.width: 1
                    border.color: Theme.surface
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: parent.spec.cursor
                    onPressed: function (mouse) {
                        overlay.resizeEdge = parent.spec.key;
                        overlay.resizeRect = overlay.liveRect;
                        overlay.resizeStart = mapToItem(overlay.contentItem, mouse.x, mouse.y);
                        mouse.accepted = true;
                    }
                    onPositionChanged: function (mouse) {
                        if (!pressed || overlay.resizeEdge === "")
                            return;
                        // 手柄自己会跟着框一起动，所以用「映射回浮层坐标」的绝对位置算位移，
                        // 不能用手柄内的相对坐标（会自我循环）。
                        const p = mapToItem(overlay.contentItem, mouse.x, mouse.y);
                        overlay.applyResize(p.x - overlay.resizeStart.x, p.y - overlay.resizeStart.y);
                    }
                    onReleased: overlay.resizeEdge = ""
                }
            }
        }
    }

    // ---------------------------------------------------------------- 选区内：busy / error
    Rectangle {
        id: busyBox
        x: overlay.liveRect.x
        y: overlay.liveRect.y
        width: overlay.liveRect.width
        height: overlay.liveRect.height
        color: Qt.rgba(Theme.surfaceHigh.r, Theme.surfaceHigh.g, Theme.surfaceHigh.b, 0.96)
        visible: overlay.working
        radius: Theme.rXs
        border.width: 1
        border.color: Theme.outlineVariant
        Column {
            anchors.centerIn: parent
            spacing: Theme.s2
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: screenFlow.phase === "Recognizing" ? qsTr("识别中…") : qsTr("翻译中…")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: screenFlow.statusText
                color: Theme.inkVariant
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }

    Rectangle {
        id: errBox
        x: overlay.liveRect.x
        y: overlay.liveRect.y
        width: overlay.liveRect.width
        height: overlay.liveRect.height
        color: Theme.dangerContainer
        border.width: 1
        border.color: Theme.danger
        visible: overlay.errored
        radius: Theme.rXs
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - Theme.s4 * 2, 420)
            spacing: Theme.s2
            Text {
                width: parent.width
                text: qsTr("没有翻译成功")
                // dangerText 是给浅色 danger 底用的（深色主题下是深红），
                // 这里压在 dangerContainer 上，要用容器配的前景色。
                color: Theme.dangerContainerText
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                width: parent.width
                text: screenFlow.errorText
                color: Theme.dangerContainerText
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // ---------------------------------------------------------------- 译文块
    // 每块只盖住 OCR 出来的那一行，坐标是「相对选区左上角」的物理像素，
    // 所以除一次 pixelScale 就回到逻辑像素。
    Item {
        id: plateLayer
        anchors.fill: parent
        visible: overlay.showing
        Repeater {
            model: screenFlow.blocks
            delegate: Rectangle {
                required property var modelData
                readonly property real px: overlay.sel.x + modelData.x / overlay.scale
                readonly property real py: overlay.sel.y + modelData.y / overlay.scale
                readonly property real pw: Math.max(8, modelData.w / overlay.scale)
                readonly property real ph: Math.max(8, modelData.h / overlay.scale)
                x: px - 2
                y: py - 1
                width: pw + 4
                height: ph + 2
                radius: Theme.rXs
                // 底色用 surfaceLow（设计稿的 --plate = --sc-low），
                // 它比 surface 亮一档，压在原文上才看得出是一块「贴纸」。
                color: Qt.rgba(Theme.surfaceLow.r, Theme.surfaceLow.g, Theme.surfaceLow.b,
                               backend.overlayOpacity / 100)
                border.width: overlay.showOriginal ? 1 : 0
                border.color: Theme.outlineVariant

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    verticalAlignment: Text.AlignVCenter
                    text: overlay.showOriginal
                              ? modelData.original
                              : (modelData.untranslated ? modelData.original : modelData.text)
                    color: modelData.untranslated && !overlay.showOriginal ? Theme.inkVariant : Theme.ink
                    font.family: Theme.fontContent
                    // 设置里的字号是「原文行高 13px 时的大小」，这里按行高再收一次，
                    // 免得小行被大字号撑破。
                    font.pixelSize: Math.max(9, Math.min(backend.overlayFontSize, ph * 0.92))
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                // 「保留原文标注」：译文条上方压一条薄原文条，底部一道浅线分隔。
                Rectangle {
                    id: originalTag
                    visible: backend.overlayKeepOriginal && !overlay.showOriginal && !modelData.untranslated
                    x: 0
                    y: -height - 2
                    width: parent.width
                    height: Math.max(11, ph * 0.7)
                    radius: Theme.rXs
                    color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, 0.82)

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 2
                        anchors.rightMargin: 2
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.original
                        color: Theme.inkVariant
                        font.family: Theme.fontContent
                        font.pixelSize: Math.max(8, backend.overlayFontSize * 0.8)
                        elide: Text.ElideRight
                        maximumLineCount: 1
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.outline
                        opacity: 0.55
                    }
                }

                // 「显示原文」状态下加一条虚线下划线 + 角标，与设计稿 A5 一致
                Rectangle {
                    visible: overlay.showOriginal
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.outline
                }
            }
        }
    }

    // ---------------------------------------------------------------- 工具栏
    // 位置：选区下方；下方放不下就翻到上方（设计稿 A9 的贴边翻转）。
    Rectangle {
        id: ovbar
        readonly property bool below: overlay.liveRect.y + overlay.liveRect.height + height + Theme.s4 < overlay.height
        x: Math.max(Theme.s2,
                    Math.min(overlay.liveRect.x + (overlay.liveRect.width - width) / 2,
                             overlay.width - width - Theme.s2))
        y: below ? overlay.liveRect.y + overlay.liveRect.height + Theme.s3
                 : overlay.liveRect.y - height - Theme.s3
        width: barRow.implicitWidth + Theme.s3 * 2
        height: 44
        radius: Theme.rMd
        color: Qt.rgba(Theme.surfaceHigh.r, Theme.surfaceHigh.g, Theme.surfaceHigh.b, 0.97)
        border.width: 1
        border.color: Theme.outlineVariant
        visible: overlay.engaged && overlay.liveRect.width > 1 && overlay.liveRect.height > 1
                 && !overlay.dragging
        z: 5

        Row {
            id: barRow
            anchors.centerIn: parent
            spacing: Theme.s2

            // 阶段标签：让用户知道现在是「等你确认」还是「已出结果」
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: overlay.selecting ? qsTr("已选 %1 × %2")
                                            .arg(Math.round(overlay.liveRect.width))
                                            .arg(Math.round(overlay.liveRect.height))
                                        : (overlay.errored ? qsTr("失败")
                                                           : (overlay.working ? qsTr("处理中…") : qsTr("译文")))
                color: Theme.inkVariant
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeTiny
            }

            Rectangle {
                width: 1
                height: 18
                color: Theme.outlineVariant
            }

            // 引擎槽（设计稿 A2 的「引擎」位置）：三个页面共用同一个 engineId，
            // 所以这里改的就是翻译页/配置页那个值。
            OverlayButton {
                id: engineSlot
                objectName: "engineSlot"
                text: overlay.engineShortName() + " ▾"
                primary: false
                onClicked: overlay.menuOpen = !overlay.menuOpen
            }

            Rectangle {
                width: 1
                height: 18
                color: Theme.outlineVariant
            }


            OverlayButton {
                text: qsTr("重新框选")
                visible: true
                onClicked: {
                    overlay.confirmed = false;
                    overlay.localRect = Qt.rect(0, 0, 0, 0);
                    screenFlow.cancel();
                    screenFlow.begin();
                }
            }
            OverlayButton {
                text: overlay.showOriginal ? qsTr("显示译文") : qsTr("显示原文")
                visible: overlay.showing
                onClicked: screenFlow.toggleOriginal()
            }
            OverlayButton {
                text: qsTr("复制")
                visible: overlay.showing
                onClicked: screenFlow.copyResult()
            }
            OverlayButton {
                text: qsTr("关闭 Esc")
                onClicked: screenFlow.cancel()
            }

            // 主操作放最右端（设计稿的「保存」位置）：从左到右看是「次要 → 主操作」的收尾。
            OverlayButton {
                text: overlay.errored ? qsTr("重新翻译") : qsTr("翻译")
                primary: true
                visible: overlay.selecting || overlay.errored
                onClicked: {
                    if (overlay.errored && screenFlow.phase === "Error")
                        screenFlow.retranslate();
                    else
                        screenFlow.confirmSelection(overlay.liveRect);
                }
            }
        }
    }

    // 空选区提示（设计稿 A10）：拖出来的框宽或高为 0 时明确说一句
    Rectangle {
        id: emptyHint
        anchors.horizontalCenter: parent.horizontalCenter
        y: overlay.liveRect.y + overlay.liveRect.height + Theme.s3 + 48
        width: emptyHintLabel.implicitWidth + Theme.s4 * 2
        height: emptyHintLabel.implicitHeight + Theme.s2 * 2
        radius: Theme.rSm
        color: Theme.cautionContainer
        border.width: 1
        border.color: Theme.caution
        visible: overlay.engaged && !overlay.dragging
                 && overlay.liveRect.width > 0 && overlay.liveRect.height > 0
                 && (overlay.liveRect.width < 3 || overlay.liveRect.height < 3)
        Text {
            id: emptyHintLabel
            anchors.centerIn: parent
            text: qsTr("选区宽或高为 0，请重新框选")
            color: Theme.caution
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    // 八个方向手柄的几何：h/v 是相对选区的 0~1 比例，cursor 是鼠标指针形状。
    readonly property var handleSpecs: [
        { key: "nw", h: 0.0, v: 0.0, cursor: Qt.SizeFDiagCursor },
        { key: "n", h: 0.5, v: 0.0, cursor: Qt.SizeVerCursor },
        { key: "ne", h: 1.0, v: 0.0, cursor: Qt.SizeBDiagCursor },
        { key: "w", h: 0.0, v: 0.5, cursor: Qt.SizeHorCursor },
        { key: "e", h: 1.0, v: 0.5, cursor: Qt.SizeHorCursor },
        { key: "sw", h: 0.0, v: 1.0, cursor: Qt.SizeBDiagCursor },
        { key: "s", h: 0.5, v: 1.0, cursor: Qt.SizeVerCursor },
        { key: "se", h: 1.0, v: 1.0, cursor: Qt.SizeFDiagCursor }
    ]

    // 松手（或调试钩子）之后要做的：留框 → 进二次确认 → 可选自动翻译。
    // 二次调整：拖八个手柄改框，不必重新框选。
    property string resizeEdge: ""
    property rect resizeRect: Qt.rect(0, 0, 0, 0)
    property point resizeStart: Qt.point(0, 0)

    // 手柄什么时候可用：框选阶段（含已框定等确认）、框有实际大小、不在翻译/展示中。
    readonly property bool resizable: overlay.selecting && !overlay.working
                                       && overlay.liveRect.width > 1 && overlay.liveRect.height > 1

    // 整块选区搬家：moveRect 是按下那一刻的框，moveStart 是按下的位置。
    property rect moveRect: Qt.rect(0, 0, 0, 0)
    property point moveStart: Qt.point(0, 0)

    function applyMove(dx, dy) {
        const base = overlay.moveRect;
        if (base.width <= 0 || base.height <= 0)
            return;
        const x = Math.max(0, Math.min(base.x + dx, overlay.width - base.width));
        const y = Math.max(0, Math.min(base.y + dy, overlay.height - base.height));
        overlay.localRect = Qt.rect(Math.round(x), Math.round(y), Math.round(base.width), Math.round(base.height));
    }
    function applyResize(dx, dy) {
        const base = overlay.resizeRect;
        const edge = overlay.resizeEdge;
        if (edge === "" || base.width <= 0)
            return;

        let x = base.x;
        let y = base.y;
        let w = base.width;
        let h = base.height;
        if (edge.indexOf("w") >= 0) {
            x += dx;
            w -= dx;
        }
        if (edge.indexOf("e") >= 0)
            w += dx;
        if (edge.indexOf("n") >= 0) {
            y += dy;
            h -= dy;
        }
        if (edge.indexOf("s") >= 0)
            h += dy;

        // 最小 12px；翻过对面时把那一侧钉住，而不是让框翻个面。
        if (w < 12) {
            if (edge.indexOf("w") >= 0)
                x = base.x + base.width - 12;
            w = 12;
        }
        if (h < 12) {
            if (edge.indexOf("n") >= 0)
                y = base.y + base.height - 12;
            h = 12;
        }

        // 夹在屏幕内
        x = Math.max(0, Math.min(x, overlay.width - 12));
        y = Math.max(0, Math.min(y, overlay.height - 12));
        w = Math.min(w, overlay.width - x);
        h = Math.min(h, overlay.height - y);

        overlay.localRect = Qt.rect(Math.round(x), Math.round(y), Math.round(w), Math.round(h));
    }

    function commitSelection(r) {
        if (r.width < 3 || r.height < 3) {
            // 单击（没拖）就当没选，保持 Selecting 等用户重来
            return;
        }
        overlay.localRect = r;
        overlay.confirmed = true;
        // 框定了就收起放大镜（它的可见条件是 dragging，这里顺手把坐标也清掉，
        // 免得下次热键开局先闪一下上一轮的位置）。
        overlay.cursorPos = Qt.point(-1, -1);
        if (backend.captureAutoRun)
            screenFlow.confirmSelection(r);
    }

    // ---------------------------------------------------------------- 选择交互
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: overlay.selecting && !overlay.confirmed
        cursorShape: Qt.CrossCursor
        acceptedButtons: Qt.LeftButton
        // 放大镜要在没按键时也跟着动，所以必须开 hover。
        hoverEnabled: true
        onPressed: function (mouse) {
            overlay.menuOpen = false;
            overlay.cursorPos = Qt.point(mouse.x, mouse.y);
            overlay.dragX = mouse.x;
            overlay.dragY = mouse.y;
            overlay.dragging = true;
            overlay.confirmed = false;
            mouse.accepted = true;
        }
        onPositionChanged: function (mouse) {
            // 拖拽中靠 property 变化驱动 liveRect 重算（QML 里没有 command 模式）。
            // 放大镜另需一份光标位置，这里顺手更新。
            overlay.cursorPos = Qt.point(mouse.x, mouse.y);
        }

        onReleased: function (mouse) {
            // 拖拽状态先关掉，再拿最终的框 —— 否则 liveRect 还在按 mouseX 算，
            // 松手瞬间鼠标位置已经不可靠了。
            const r = overlay.dragging
                          ? Qt.rect(Math.min(overlay.dragX, mouse.x), Math.min(overlay.dragY, mouse.y),
                                    Math.abs(mouse.x - overlay.dragX), Math.abs(mouse.y - overlay.dragY))
                          : overlay.liveRect;
            overlay.dragging = false;
            overlay.commitSelection(r);
        }
    }

    // Esc 关闭、Enter 确认（设计稿：方向键微调暂不做，先保证有键盘出口）
    Item {
        anchors.fill: parent
        focus: overlay.engaged
        Keys.onEscapePressed: overlay.menuOpen ? (overlay.menuOpen = false) : screenFlow.cancel()
        Keys.onReturnPressed: if (overlay.selecting && !overlay.confirmed) screenFlow.confirmSelection(overlay.liveRect)
    }

    // Window 上没有 forceActiveFocus；顶置窗口要自己抢一次激活才能收键盘。
    Component.onCompleted: overlay.requestActivate()

    // 阶段回到 Idle 时清掉本地状态（下次热键是一个干净的开始）
    Connections {
        target: screenFlow
        function onPhaseChanged() {
            if (screenFlow.phase === "Idle") {
                overlay.dragging = false;
                overlay.confirmed = false;
                overlay.menuOpen = false;
                overlay.localRect = Qt.rect(0, 0, 0, 0);
                // 放大镜也要归零，否则下次热键会先闪一下上一轮的坐标。
                overlay.cursorPos = Qt.point(-1, -1);
            }
        }
    }

    // 点空白处收起引擎下拉。z 低于工具栏（5）高于内容，所以工具栏按钮照样能点，
    // 菜单本身 z=20 在它上面。
    MouseArea {
        anchors.fill: parent
        z: 4
        visible: overlay.menuOpen
        onClicked: overlay.menuOpen = false
    }

    // 工具栏引擎槽的下拉。
    //
    // 这里**不能**用 Popup：Popup 是独立顶层窗口，x/y 只要算偏一点（mapToItem 在
    // 布局完成前求值、或者 parent 与可视父级不一致）就会贴到屏幕左上角 ——
    // 而浮层本身是 WS_EX_NOACTIVATE 的全屏窗口，弹到左上角以后还压不到它上面。
    // 改成窗内 Rectangle 之后，它天然跟着工具栏走，也不用手工管 z 序和关窗。
    Rectangle {
        id: engineMenu
        objectName: "engineMenu"
        z: 20
        visible: overlay.menuOpen
        width: 176
        height: engineMenuColumn.implicitHeight + Theme.s1 * 2
        // 引擎槽在窗口里的位置：跟着工具栏（工具栏跟着选区）。
        // 注意不能用 mapToItem —— 它在绑定里是 C++ 调用，内部读的 x/y 不会产生依赖，
        // 只会在布局还没跑完时求值一次，然后永远停在 (0,0)（就是"弹到左上角"的根因）。
        // 逐级相加的每个坐标都是属性读取，依赖成立、随工具栏移动。
        readonly property point anchorPoint: Qt.point(ovbar.x + barRow.x + engineSlot.x,
                                                      ovbar.y + barRow.y + engineSlot.y)
        x: Math.max(Theme.s2, Math.min(anchorPoint.x, overlay.width - width - Theme.s2))
        y: {
            const below = anchorPoint.y + engineSlot.height + height + Theme.s2 < overlay.height
            return below ? anchorPoint.y + engineSlot.height + Theme.s2
                         : Math.max(Theme.s2, anchorPoint.y - height - Theme.s2)
        }
        color: Theme.surfaceHigh
        radius: Theme.rSm
        border.width: 1
        border.color: Theme.outline

        Column {
            id: engineMenuColumn
            x: Theme.s1
            y: Theme.s1
            width: parent.width - Theme.s1 * 2
            spacing: 2
            Repeater {
                model: overlay.engineChoices
                delegate: Rectangle {
                    required property var modelData
                    width: engineMenuColumn.width
                    height: 32
                    radius: Theme.rXs
                    color: (backend.engineId || "") === modelData.code
                               ? Theme.primaryContainer
                               : (itemHover.hovered ? Theme.surfaceHighest : "transparent")
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: (backend.engineId || "") === modelData.code
                                   ? Theme.primaryContainerText
                                   : Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    HoverHandler { id: itemHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            backend.engineId = modelData.code
                            overlay.menuOpen = false
                        }
                    }
                }
            }
        }
    }

    // 内联按钮：跟主界面同款 —— 主按钮用强调色实心，次按钮透明底 + 悬浮高亮。
    component OverlayButton: Rectangle {
        id: ob
        property string text: ""
        property bool primary: false
        signal clicked()
        readonly property bool hovered: obHover.hovered
        width: label.implicitWidth + Theme.s4 * 2
        height: 30
        radius: Theme.rSm
        color: ob.primary ? (ob.hovered ? Qt.lighter(Theme.primary, 1.08) : Theme.primary)
                          : (ob.hovered ? Theme.surfaceHighest : "transparent")
        border.width: ob.primary ? 0 : 1
        border.color: ob.hovered ? Theme.outline : Theme.outlineVariant
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        opacity: enabled ? 1.0 : 0.45
        Text {
            id: label
            anchors.centerIn: parent
            text: ob.text
            color: ob.primary ? Theme.primaryText : Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: ob.primary ? Font.DemiBold : Font.Normal
        }
        HoverHandler { id: obHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: ob.clicked() }
    }
}
