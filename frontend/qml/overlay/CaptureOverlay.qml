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
    // 点「设置」弹出的浮层设置小窗是否展开。它是本窗口的子窗口（见文件末尾的实例）。
    property bool settingsOpen: false
    // 语言菜单展开的是哪一个："" = 收起，"src" = 源语言，"dst" = 目标语言。
    // 两者共用同一块窗内面板（同一时刻只开一个）。
    property string langMenuFor: ""
    // 小窗里点了「更多设置」：交给 Main 收浮层、回主界面的「浮层」页。
    signal moreSettingsRequested()
    // 这一次框选是给谁用的："capture"（默认，框完去翻译）还是 "live"（框完开实时翻译）。
    // 实时模式下工具栏只留「开始实时翻译 / 取消」——设计稿 C1 就两个动作，
    // 而且实时是持续刷新，没有"译完再复制/显示原文"这些一次性动作。
    property string purpose: "capture"
    // 实时模式点了「开始实时翻译」：把选区（逻辑像素）交给 Main，由它启动 LiveSession。
    signal liveStartRequested(rect region)
    // 松手后的框（还没送去翻译）。ScreenFlow 的 selectionRect 要等 confirmSelection 才有值，
    // 所以「已选、等确认」这个阶段必须自己留一份。
    property rect localRect: Qt.rect(0, 0, 0, 0)

    // 放大镜跟随的光标位置（窗口逻辑坐标；-1 = 光标还没进窗口）。
    // 用鼠标自己报的坐标，别用 HoverHandler.point.position —— 浮层是 NOACTIVATE 窗口，
    // hover 事件在没激活的窗口上不一定来。
    property point cursorPos: Qt.point(-1, -1)

    readonly property real scale: screenFlow.pixelScale > 0 ? screenFlow.pixelScale : 1.0
    readonly property bool showOriginal: screenFlow.showOriginal

    // 确认选区。截图模式交给 ScreenFlow 去 OCR + 翻译；实时模式只把框交出去，
    // 浮层立刻退出——实时有自己的贴纸窗口和控制条，两者不该同时待在屏幕上。
    function confirmCurrent(region) {
        if (region.width < 3 || region.height < 3)
            return;
        if (purpose === "live") {
            liveStartRequested(region);
            screenFlow.cancel();
            return;
        }
        screenFlow.confirmSelection(region);
    }

    // 语言对和主界面是同一份值：backend.srcLang / backend.dstLangs。
    // 截图翻译一次只出一个目标语言，所以这里写的是单元素列表。
    function langLabel(code) {
        const list = backend.languages || []
        const key = code || "auto"
        for (let i = 0; i < list.length; ++i) {
            if (list[i].code === key)
                return list[i].label
        }
        return key
    }

    function langChoices(includeAuto) {
        const list = backend.languages || []
        const out = []
        for (let i = 0; i < list.length; ++i) {
            if (!includeAuto && list[i].code === "auto")
                continue
            out.push(list[i])
        }
        return out
    }

    function chooseLang(code) {
        if (langMenuFor === "src") {
            backend.srcLang = code
        } else if (langMenuFor === "dst") {
            backend.dstLangs = [code]
        }
        langMenuFor = ""
    }

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
    // 名字统一由后端给（EngineIds.cpp 一份，界面语言切了也跟着变）。
    readonly property var engineChoices: [
        { code: "", label: backend.engineName("") },
        { code: "web", label: backend.engineName("web") },
        { code: "llm-cloud", label: backend.engineName("llm-cloud") },
        { code: "llm-local", label: backend.engineName("llm-local") },
        { code: "local-mt", label: backend.engineName("local-mt") }
    ]

    // 后端报的引擎状态（id → "ready" / "not-ready" / "missing-assets" / "missing-config"）。
    // 做成绑定而不是一次性拷贝：菜单开着的时候下次刷新、或者切了引擎，圆点自己会变。
    readonly property var engineStatusMap: {
        const map = ({})
        const rows = backend.engineStatuses || []
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!row || !row.id)
                continue
            // 状态口径统一在 Theme.engineStateOf（装好了但当前语言对走不通 → 黄灯）。
            map[Theme.canonicalEngineId(row.id)] = Theme.engineStateOf(row)
        }
        return map
    }

    // 空串（自动）看全体：只要还有一个能用的引擎，自动就是绿的；全废才红。
    function engineStateOf(code) {
        const map = overlay.engineStatusMap
        if (code === "") {
            let best = "missing-assets"
            for (const key in map) {
                if (key === "")
                    continue
                if (map[key] === "ready")
                    return "ready"
                if (map[key] === "not-ready")
                    best = "not-ready"
            }
            return best
        }
        return map[code] || "not-ready"
    }

    // 缺资产（要下载）、缺配置（要填地址/模型）的都不能选；
    // 「未就绪」（黄）可以选 —— 本地服务会按需拉起。
    function engineUsable(code) {
        const state = overlay.engineStateOf(code)
        return state !== "missing-assets" && state !== "missing-config"
    }

    function engineDotColor(code) {
        return Theme.engineStateColor(overlay.engineStateOf(code))
    }

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
    // 压暗浓度来自设置 backend.overlayDim（20~80，百分比）：0.45 那种写死的值
    // 在浅色截图和暗色截图上观感差太远，交给用户自己调。
    readonly property color dimColor: Qt.rgba(0, 0, 0, backend.overlayDim / 100)
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
    // 框选时贴在光标旁边（哪个角由设置决定，默认右下方）：
    // 上面是放大视图 + 十字线，下面是坐标、色值和 RGB。
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

        // 贴在光标的哪个角，来自设置 backend.magnifierCorner（默认右下）。
        // 拆成「左右」「上下」两个布尔，x/y 才能各自独立翻折 ——
        // 比如靠右但下面还有地方时，只翻水平方向，不该整个跑到左上方去。
        readonly property bool placeRight: backend.magnifierCorner === "bottomRight"
                                           || backend.magnifierCorner === "topRight"
        readonly property bool placeBelow: backend.magnifierCorner === "bottomRight"
                                           || backend.magnifierCorner === "bottomLeft"

        // 只在「正在拖框」时出现：松开手、选区定了就收起来，别一直挡着画面。
        // backend.overlayMagnifier 关掉就整块不出现（十字光标还在，只是不放大）。
        visible: overlay.dragging && backend.overlayMagnifier
                 && overlay.cursorPos.x >= 0 && overlay.cursorPos.y >= 0
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

        // 先按设置的方向贴；那一侧放不下（贴屏幕边）就翻到对面，始终留在屏内。
        // 水平、垂直分开判：右下设置碰到底边时只往上翻，仍然在光标右侧。
        x: {
            const gap = Theme.s3;
            if (magnifier.placeRight) {
                const right = overlay.cursorPos.x + gap;
                return right + width + Theme.s2 <= overlay.width
                        ? right
                        : Math.max(Theme.s2, overlay.cursorPos.x - width - gap);
            }
            const left = overlay.cursorPos.x - width - gap;
            return left >= Theme.s2
                    ? left
                    : Math.max(Theme.s2,
                               Math.min(overlay.width - width - Theme.s2, overlay.cursorPos.x + gap));
        }
        y: {
            const gap = Theme.s3;
            if (magnifier.placeBelow) {
                const down = overlay.cursorPos.y + gap;
                return down + height + Theme.s2 <= overlay.height
                        ? down
                        : Math.max(Theme.s2, overlay.cursorPos.y - height - gap);
            }
            const up = overlay.cursorPos.y - height - gap;
            return up >= Theme.s2
                    ? up
                    : Math.max(Theme.s2,
                               Math.min(overlay.height - height - Theme.s2, overlay.cursorPos.y + gap));
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
            // 取消：只打断这一单，选区保留，可以换个引擎马上再翻（Esc 也是这个动作）。
            OverlayButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("取消 Esc")
                onClicked: screenFlow.cancelTranslation()
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
    // 上下都放不下（选区几乎占满整屏）时塞进选区贴底 —— 否则 y 会算成负数，
    // 整条被窗口裁掉，用户看到的就是"工具栏跑屏幕外面去了"。
    Rectangle {
        id: ovbar
        readonly property bool below: overlay.liveRect.y + overlay.liveRect.height + height + Theme.s4 < overlay.height
        readonly property bool roomAbove: overlay.liveRect.y - height - Theme.s3 >= Theme.s2
        readonly property bool inside: !below && !roomAbove
        x: Math.max(Theme.s2,
                    Math.min(overlay.liveRect.x + (overlay.liveRect.width - width) / 2,
                             overlay.width - width - Theme.s2))
        y: below ? overlay.liveRect.y + overlay.liveRect.height + Theme.s3
                 : (inside
                    ? Math.max(Theme.s2, overlay.liveRect.y + overlay.liveRect.height - height - Theme.s3)
                    : overlay.liveRect.y - height - Theme.s3)
        width: barRow.implicitWidth + Theme.s3 * 2
        height: 44
        radius: Theme.rMd
        color: Qt.rgba(Theme.surfaceHigh.r, Theme.surfaceHigh.g, Theme.surfaceHigh.b, 0.97)
        border.width: 1
        border.color: Theme.outlineVariant
        visible: overlay.engaged && overlay.liveRect.width > 1 && overlay.liveRect.height > 1
                 && !overlay.dragging
        z: 12   // 压在选框（z=3/4）之上：贴进选区时不能被框线、手柄或尺寸标签盖住
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

            // 只有出结果那一屏，「信息」和后面那组控件之间才切一条线；
            // 二次确认那一屏，信息跟引擎/语言是同一组的，不切。
            Rectangle {
                width: 1
                height: 18
                // Row 只摆 x、不管 y：不手动垂直居中就会贴在行顶，看着像"偏上"。
                anchors.verticalCenter: parent.verticalCenter
                visible: overlay.showing
                color: Theme.outlineVariant
            }

            // 引擎槽：三个入口共用同一个 engineId，所以这里改的就是翻译页/配置页那个值。
            OverlayButton {
                id: engineSlot
                objectName: "engineSlot"
                text: overlay.engineShortName() + " ▾"
                primary: false
                // 实时模式也要能选引擎：框选阶段定的引擎就是这一单开局用的
                // （四个入口共用 backend.engineId，见「引擎选择统一到一处」）。
                onClicked: {
                    // 打开前刷一次状态：菜单右边那几个圆点要反映"现在"能不能用
                    // （刚下完资产、刚起停本地服务，都可能在两单之间变）。
                    backend.refreshEngineStatuses()
                    overlay.menuOpen = !overlay.menuOpen
                }
            }

            // 语言对：和主界面、实时、小窗共用同一份值（backend.srcLang / dstLangs）。
            // 样式保持极简的一行「[源] → [目标]」，点哪格开哪个菜单。
            // 截图翻译一次只出一个目标语言，所以目标那格按单选写。
            OverlayButton {
                id: srcSlot
                objectName: "srcSlot"
                text: overlay.langLabel(backend.srcLang) + " ▾"
                primary: false
                onClicked: {
                    overlay.menuOpen = false
                    overlay.settingsOpen = false
                    overlay.langMenuFor = overlay.langMenuFor === "src" ? "" : "src"
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "→"
                color: Theme.inkVariant
                font.pixelSize: Theme.fontSizeTiny
            }

            OverlayButton {
                id: dstSlot
                objectName: "dstSlot"
                text: overlay.langLabel((backend.dstLangs && backend.dstLangs.length > 0)
                                        ? backend.dstLangs[0] : "zh") + " ▾"
                primary: false
                onClicked: {
                    overlay.menuOpen = false
                    overlay.settingsOpen = false
                    overlay.langMenuFor = overlay.langMenuFor === "dst" ? "" : "dst"
                }
            }

            // 设置：打开「浮层设置」子窗口（截图翻译这一单怎么翻）。再点一下收起。
            // 它必须是浮层的子窗口：跟着浮层一起出现、一起关，位置也恒在浮层之上。
            OverlayButton {
                id: gearSlot
                objectName: "gearSlot"
                text: qsTr("设置")
                primary: false
                onClicked: {
                    overlay.menuOpen = false;
                    overlay.settingsOpen = !overlay.settingsOpen;
                    if (overlay.settingsOpen) {
                        settingsPanel.openAt(overlay.x + ovbar.x + barRow.x + gearSlot.x,
                                             overlay.y + ovbar.y + barRow.y + gearSlot.y,
                                             gearSlot.width,
                                             gearSlot.height);
                    }
                }
            }

            // 出结果之后才有：换原文/译文、整段复制。用户没给它们排位置，
            // 放在「设置」这一组里（都属"看结果时顺手用"），不打断后半段的操作顺序。
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

            Rectangle {
                width: 1
                height: 18
                // Row 只摆 x、不管 y：不手动垂直居中就会贴在行顶，看着像"偏上"。
                anchors.verticalCenter: parent.verticalCenter
                visible: overlay.selecting || overlay.showing || overlay.errored
                color: Theme.outlineVariant
            }

            OverlayButton {
                text: qsTr("重新框选")
                // 实时模式只留「开始 / 取消」两件事（设计稿 C1），框歪了按 Esc 重来。
                visible: overlay.purpose !== "live"
                onClicked: {
                    overlay.confirmed = false;
                    overlay.localRect = Qt.rect(0, 0, 0, 0);
                    screenFlow.cancel();
                    screenFlow.begin();
                }
            }

            // 翻译之后重跑一遍（比如刚换了目标语言）。
            // 出错时最右边那颗主按钮本身就是「重新翻译」，这里不再重复一颗。
            OverlayButton {
                text: qsTr("重新翻译")
                visible: overlay.showing
                onClicked: screenFlow.retranslate()
            }

            // 主操作：二次确认这一屏的收尾。
            OverlayButton {
                text: overlay.purpose === "live" ? qsTr("开始实时翻译")
                                                 : (overlay.errored ? qsTr("重新翻译") : qsTr("翻译"))
                primary: true
                visible: overlay.selecting || overlay.errored
                onClicked: {
                    if (overlay.errored && screenFlow.phase === "Error")
                        screenFlow.retranslate();
                    else
                        overlay.confirmCurrent(overlay.liveRect);
                }
            }

            // 任何一屏的最右边都是「| ✕」，位置固定，手不用找。
            Rectangle {
                width: 1
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                visible: overlay.selecting || overlay.showing || overlay.errored
                color: Theme.outlineVariant
            }

            CloseButton {
                visible: overlay.selecting || overlay.showing || overlay.errored
                onClicked: screenFlow.cancel()
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
    // 方向键微调：整块选区平移，尺寸不变（改大小还是拖手柄）。
    // 只有「框已定、还没翻译」这段才接 —— 结果已经出来时挪框，用户没法判断
    // 是重跑还是保留，索性不给动。返回 true 表示这手确实吃掉了。
    function nudgeSelection(dx, dy) {
        if (!overlay.selecting || overlay.dragging)
            return false;
        const base = overlay.liveRect;
        if (base.width < 3 || base.height < 3)
            return false;   // 还没框出来，方向键没意义
        const x = Math.max(0, Math.min(base.x + dx, overlay.width - base.width));
        const y = Math.max(0, Math.min(base.y + dy, overlay.height - base.height));
        overlay.localRect = Qt.rect(Math.round(x), Math.round(y),
                                    Math.round(base.width), Math.round(base.height));
        return true;
    }

    // 方向键的键位判定 + 修饰键（Shift = 10px，否则 1px）。
    //
    // 为什么不在那个 Item 上写 `Keys.enabled: backend.overlayArrowNudge`：
    // Keys.enabled 是整份 Keys 的开关，会把同一个 Item 上的 Esc / Enter 一起关掉 ——
    // 用户一关「方向键微调」，浮层就只剩鼠标能退出。所以门控放在函数第一行。
    function handleArrowKey(event) {
        if (!backend.overlayArrowNudge)
            return;
        let dx = 0;
        let dy = 0;
        if (event.key === Qt.Key_Left)
            dx = -1;
        else if (event.key === Qt.Key_Right)
            dx = 1;
        else if (event.key === Qt.Key_Up)
            dy = -1;
        else if (event.key === Qt.Key_Down)
            dy = 1;
        else
            return;
        const step = (event.modifiers & Qt.ShiftModifier) ? 10 : 1;
        if (overlay.nudgeSelection(dx * step, dy * step))
            event.accepted = true;
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
            overlay.confirmCurrent(r);
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
            const dragged = overlay.dragging;
            overlay.dragging = false;
            // 拖到窗口边界外（邻屏、负坐标）松手时 mouse.x/y 会越界：抓屏底图只有本窗口
            // 这么大，越界坐标交给 C++ 就是整块取黑边。所以先把**两个角**分别夹回
            // 窗口内再拼框（夹完再拼，不是拼完再夹 —— 后者会把框往外撑）。
            const w0 = overlay.width;
            const h0 = overlay.height;
            let r = overlay.liveRect;
            if (dragged) {
                const ax = Math.max(0, Math.min(overlay.dragX, w0));
                const ay = Math.max(0, Math.min(overlay.dragY, h0));
                const bx = Math.max(0, Math.min(mouse.x, w0));
                const by = Math.max(0, Math.min(mouse.y, h0));
                // 真拖过就保底 3px：commitSelection 对更小的框是静默丢弃 = 点了没反应。
                const rx = Math.max(0, Math.min(Math.min(ax, bx), w0 - 3));
                const ry = Math.max(0, Math.min(Math.min(ay, by), h0 - 3));
                r = Qt.rect(Math.round(rx), Math.round(ry),
                            Math.max(3, Math.round(Math.abs(bx - ax))),
                            Math.max(3, Math.round(Math.abs(by - ay))));
            }
            overlay.commitSelection(r);
        }
    }

    // Esc 关闭、Enter 确认、方向键微调选区
    Item {
        anchors.fill: parent
        focus: overlay.engaged
        // Esc 分五档：收语言菜单 → 收设置小窗 → 收引擎菜单 → 取消当前这一单（选区留着）→ 整体退出。
        // 直接整体退出会让"翻到一半发现选错引擎"的人重框一遍，很烦。
        Keys.onEscapePressed: overlay.langMenuFor !== "" ? (overlay.langMenuFor = "")
                             : (overlay.settingsOpen ? (overlay.settingsOpen = false)
                                : (overlay.menuOpen ? (overlay.menuOpen = false)
                                   : (overlay.working ? screenFlow.cancelTranslation() : screenFlow.cancel())))
        Keys.onReturnPressed: if (overlay.selecting && !overlay.confirmed) overlay.confirmCurrent(overlay.liveRect)
        // 只认方向键，别的键原样放行；开关与修饰键在 handleArrowKey 里判。
        Keys.onPressed: function (event) { overlay.handleArrowKey(event) }
    }

    // Window 上没有 forceActiveFocus；顶置窗口要自己抢一次激活才能收键盘。
    Component.onCompleted: overlay.requestActivate()

    // 阶段回到 Idle 时清掉本地状态（下次热键是一个干净的开始）
    Connections {
        target: screenFlow
        function onPhaseChanged() {
            // 每次重新进入框选都要再抢一次焦点：窗口是 WS_EX_NOACTIVATE 的，
            // 光靠启动时那一次 requestActivate，热键第二次就不一定有键盘了（方向键会失效）。
            if (screenFlow.phase === "Selecting")
                overlay.requestActivate();


            if (screenFlow.phase === "Idle") {
                overlay.dragging = false;
                overlay.confirmed = false;
                overlay.menuOpen = false;
                overlay.settingsOpen = false;
                overlay.localRect = Qt.rect(0, 0, 0, 0);
                // 放大镜也要归零，否则下次热键会先闪一下上一轮的坐标。
                overlay.cursorPos = Qt.point(-1, -1);
            }
        }
    }

    // 点空白处收起引擎下拉 / 语言菜单 / 设置小窗。z 低于工具栏（5）高于内容，所以工具栏按钮照样能点。
    MouseArea {
        anchors.fill: parent
        z: 4
        visible: overlay.menuOpen || overlay.settingsOpen || overlay.langMenuFor !== ""
        onClicked: {
            overlay.menuOpen = false;
            overlay.settingsOpen = false;
            overlay.langMenuFor = "";
        }
    }

    // 语言菜单：源语言与目标语言共用这一块，跟着被点的那一格走。
    // 和引擎菜单同理，不用 Popup（独立顶层窗口在这里会抢焦点、也压不到浮层上）。
    Rectangle {
        id: langMenu
        objectName: "langMenu"
        z: 21
        visible: overlay.langMenuFor !== ""
        width: 180
        height: Math.min(overlay.height - Theme.s4 * 2, langMenuColumn.implicitHeight + Theme.s1 * 2)

        readonly property bool forSource: overlay.langMenuFor === "src"
        readonly property Item anchorItem: forSource ? srcSlot : dstSlot
        readonly property point anchorPoint: Qt.point(ovbar.x + barRow.x + anchorItem.x,
                                                      ovbar.y + barRow.y + anchorItem.y)
        x: Math.max(Theme.s2, Math.min(anchorPoint.x, overlay.width - width - Theme.s2))
        y: {
            const below = anchorPoint.y + anchorItem.height + height + Theme.s2 < overlay.height
            return below ? anchorPoint.y + anchorItem.height + Theme.s2
                         : Math.max(Theme.s2, anchorPoint.y - height - Theme.s2)
        }
        color: Theme.surfaceHigh
        radius: Theme.rSm
        border.width: 1
        border.color: Theme.outline

        Flickable {
            id: langMenuFlick
            anchors.fill: parent
            anchors.margins: Theme.s1
            contentHeight: langMenuColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: langMenuColumn
                width: langMenuFlick.width
                spacing: 2

                Repeater {
                    model: overlay.langChoices(langMenu.forSource)
                    delegate: Rectangle {
                        id: langRow
                        required property var modelData
                        readonly property bool picked: (langMenu.forSource
                                                       ? (backend.srcLang || "auto")
                                                       : ((backend.dstLangs && backend.dstLangs.length > 0)
                                                          ? backend.dstLangs[0] : "zh")) === modelData.code
                        width: parent.width
                        height: 28
                        radius: Theme.rXs
                        color: picked ? Theme.primaryContainer
                                      : (rowHover.hovered ? Theme.surfaceHighest : "transparent")

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.s2
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            text: langRow.modelData.label
                            elide: Text.ElideRight
                            color: langRow.picked ? Theme.primaryContainerText : Theme.ink
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                        }

                        HoverHandler {
                            id: rowHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: overlay.chooseLang(langRow.modelData.code)
                        }
                    }
                }
            }
        }
    }

    // 工具栏引擎槽的下拉。
    //
    // 这里**不能**用 Popup：Popup 是独立顶层窗口，x/y 只要算偏一点（mapToItem 在
    // 布局完成前求值、或者 parent 与可视父级不一致）就会贴到屏幕左上角 ——
    // 而浮层本身是 WS_EX_NOACTIVATE 的全屏窗口，弹到左上角以后还压不到它上面。
    // 改成窗内 Rectangle 之后，它天然跟着工具栏走，也不用手工管 z 序和关窗。
    MenuPanel {
        id: engineMenu
        objectName: "engineMenu"
        z: 20
        visible: overlay.menuOpen
        width: 196
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

        Repeater {
            model: overlay.engineChoices
            delegate: MenuRow {
                required property var modelData
                text: modelData.label
                selected: (backend.engineId || "") === modelData.code
                showDot: true
                dotColor: overlay.engineDotColor(modelData.code)
                // 缺资产 / 缺配置不给选（选了也翻不了）；黄灯可以选，本地服务会按需拉起。
                enabled: overlay.engineUsable(modelData.code)
                onActivated: {
                    backend.engineId = modelData.code
                    overlay.menuOpen = false
                }
            }
        }
    }



    // 内联按钮：跟主界面同款 —— 主按钮用强调色实心，次按钮透明底 + 悬浮高亮。
    // 「设置」小窗：本窗口的子窗口（owner = 浮层），可见性由 settingsOpen 一处决定 ——
    // 浮层一收它跟着收，不用再和各处清理逻辑对着改。
    CaptureSettingsPanel {
        id: settingsPanel
        objectName: "captureSettingsPanel"
        // 实时入口点「设置」时给 live 那一组（刷新间隔、区域底色…），
        // 截图入口给 capture 那一组。两边是同一份 backend 值，只是露脸的项不同。
        mode: overlay.purpose === "live" ? "live" : "capture"
        visible: overlay.settingsOpen && overlay.engaged
        onClosed: overlay.settingsOpen = false
        onMoreSettingsRequested: {
            overlay.settingsOpen = false;
            overlay.moreSettingsRequested();
        }
    }

    // ✕ 已抽成 components/CloseButton.qml（截图 / 小窗 / 设置面板 / 实时控制条共用）。

    component OverlayButton: Rectangle {
        id: ob
        property string text: ""
        property bool primary: false
        signal clicked()
        readonly property bool hovered: obHover.hovered
        width: label.implicitWidth + Theme.s4 * 2
        height: 26
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
