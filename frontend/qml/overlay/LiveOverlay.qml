// C 页：实时翻译的译文浮层（设计稿 C7 的「只盖文字」形态）。
//
// 这个窗口的三个性质是一套的，少一个就废：
//   · 全透明   —— 没被翻的那几行要原样透出来，浮层不能有底色；
//   · 点击穿透 —— 它压在用户正在读的窗口上，误点一下就打断阅读；
//   · 不被自己拍到 —— 实时翻译是「隔几秒截一次屏再识别」，浮层要是进了自己的截图，
//     下一轮 OCR 就会把上一轮的译文当原文再翻一遍，译文越翻越离谱且永远停不下来。
//
// 透明是 QML 的事；穿透（WS_EX_TRANSPARENT）和「排除出截图」（WDA_EXCLUDEFROMCAPTURE）
// QML 表达不了，由 C++ 的 attachPlateWindow() 打上。所以这里没有任何输入相关的代码：
// 不放 MouseArea、不 requestActivate、不抢焦点 —— 点了也点不到，管它等于白管。
import QtQuick
import QtQuick.Window
import Studious

Window {
    id: overlay

    // 截图工具（--ui-shot）和 C++ 的 findChild 都靠这个名字认窗口。
    objectName: "liveOverlay"

    // 显隐跟着会话走：开局可见、暂停保持最后一帧、出错留在屏上等用户重开，
    // 只有 Idle（还没开 / 已经停）才收起来。位置由 C++ 的 placeOverlay() 摆。
    visible: liveSession.phase !== "Idle"

    // 无边框 + 置顶 + 不要系统阴影。其余窗口样式（穿透、排除截图、不进任务栏）
    // 统一由 attachPlateWindow() 用 Win32 扩展样式打；QML 这层再加一次会和
    // C++ 那边的 SetWindowLong 位运算互相覆盖，谁后写谁赢，最后行为看时序。
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.NoDropShadowWindowHint
    color: "transparent"

    // OCR 的框是**物理像素**，控件坐标是逻辑像素。DPI 缩放下这两个不是一回事，
    // 全文件只有这一处做换算，别在别处再乘除一次。
    readonly property real scale: liveSession.pixelScale > 0 ? liveSession.pixelScale : 1.0

    // 选区背景底色的强度（0~1）。0 时不铺底，只在有译文的地方画贴纸。
    readonly property real shade: Math.max(0, Math.min(100, backend.overlayBackdrop)) / 100

    // QML 的 flags/visible 和 C++ 的 SetWindowLongPtr 写的是同一份窗口数据，
    // Qt 在显隐或重绘时会重写一次，把 WS_EX_TRANSPARENT（穿透）抹掉 —— 解锁后
    // 手柄就点不动了。所以这些时机过后再让 C++ 把「穿透 + 排除截图」盖回去。
    Timer {
        id: reapplyTimer
        interval: 0
        repeat: false
        onTriggered: liveSession.reapplyPlateFlags()
    }
    // 上面四个信号经常在同一个事件循环里一起响（开始一单 = phase + region 一起变，
    // 解锁 = regionLocked 变）。每个都 restart() 一个 0ms 单发 Timer，会把这次重打
    // 一直往后推，最后连着打好几轮 SetWindowLong。已经在队列里就合并掉。
    function scheduleReapply() {
        if (reapplyTimer.running)
            return
        reapplyTimer.restart()
    }
    onVisibleChanged: scheduleReapply()
    Connections {
        target: liveSession
        function onRegionLockedChanged() { scheduleReapply() }
        function onRegionChanged() { scheduleReapply() }
        function onPhaseChanged() { scheduleReapply() }
    }

    Item {
        anchors.fill: parent

        // ---- 选区背景底色 ----
        // 实时贴纸是半透明的，底下原文会透上来跟译文叠在一起；整块选区先铺一层
        // 底色（Theme.surface），字就压在干净的底上了。强度走
        // settings/overlay/backdrop（0 = 不铺，默认 70%）。
        // 窗口本身就正好等于选区，所以这里铺满即可，不需要额外算内缩。
        Rectangle {
            anchors.fill: parent
            visible: overlay.shade > 0
            color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, overlay.shade)
        }
        // model 绑 liveSession.blocks：每轮增量识别后 C++ 换一整份列表，
        // Repeater 跟着重建。块数一般在几十的量级，重建比逐块 diff 便宜也少出错。
        Repeater {
            model: liveSession.blocks

            delegate: Rectangle {
                id: plate
                required property var modelData

                // h 缺失或为 0 时兜一个 1：否则下面全是 NaN，整块静默消失，很难查。
                readonly property real lineH: Math.max(1, (modelData.h || 0) / overlay.scale)

                // 字号先用**行高**推，再用它去算块高。
                // 反过来（先定块高、再拿块高算字号）会形成 height ↔ font.pixelSize 的
                // 绑定环 —— Qt 只会留一条告警，然后永远取上一帧的值。
                readonly property real fontSize:
                    Math.max(9, Math.min(backend.overlayFontSize, lineH * 0.9))

                // untranslated：这一行本来就不用翻（数字、代码、专名），或者翻译失败
                // 保留了原文。设计稿 C7 的做法是**整块不画**，让用户直接读屏幕上
                // 原来那行字，而不是压一层几乎一样的复制品。
                visible: modelData.untranslated !== true

                x: (modelData.x || 0) / overlay.scale
                y: (modelData.y || 0) / overlay.scale
                width: Math.max(8, (modelData.w || 0) / overlay.scale)
                // 文字自适应，但不许超过原行高的 1.35 倍：
                // 再高就会压到上一行/下一行的原文，而实时场景里那几行也是别人的内容。
                height: Math.min(Math.max(10, lineH * 1.35),
                                 Math.max(lineH, fontSize * 1.45))
                radius: Theme.rXs
                // 底色 surfaceLow 比 surface 亮一档，压在原文上才看得出是一块「贴纸」。
                color: Qt.rgba(Theme.surfaceLow.r, Theme.surfaceLow.g, Theme.surfaceLow.b,
                               backend.overlayOpacity / 100)

                Text {
                    id: plateText
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s1
                    anchors.rightMargin: Theme.s1
                    verticalAlignment: Text.AlignVCenter
                    // 显示原文时读 original；个别块 C++ 只给了译文（没原文可给），
                    // 那就退回译文 —— 宁可显示一块一样的字，也别给一片空白。
                    text: liveSession.showOriginal
                          ? (plate.modelData.original || plate.modelData.text)
                          : plate.modelData.text
                    color: liveSession.showOriginal ? Theme.inkVariant : Theme.ink
                    font.family: Theme.fontContent
                    font.pixelSize: plate.fontSize
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                    // 高度上面已经算好，行数锁死 1：长句不锁的话会折成两行溢出块外，
                    // 压到下一行的原文上 —— 这比截断难看得多。
                    maximumLineCount: 1
                }

                // 「显示原文」（Ctrl 之类切换）：左侧一条 2px 竖条当锚点。
                // 选竖条而不是整圈描边：描边会把上下相邻的两行割开，而这几个块
                // 在设计稿里是连成一段的；2px 也几乎不占字位。
                Rectangle {
                    visible: liveSession.showOriginal
                    x: 1
                    y: 0
                    width: 2
                    height: parent.height
                    radius: 1
                    color: Theme.outlineVariant
                    opacity: 0.5
                }

            }
        }

        // ---- 解锁后的选区调整（和截图翻译的框选一个手感）----
        // 锁定（默认）时这些东西全部不画、也不接鼠标：窗口在 platform 层是穿透的，
        // 画了也点不到。控制条上的「锁定」按钮切 liveSession.regionLocked。
        Rectangle {
            id: frame
            anchors.fill: parent
            visible: !liveSession.regionLocked
            color: "transparent"
            border.width: 1.5
            border.color: Theme.primary
        }

        // 整块拖动：按在选区里（不在手柄上）就能挪位置。
        //
        // 坐标必须落在**屏幕**坐标上：窗口一动，窗口内的 mouse.x 就跟着反着变，
        // 再拿它去算绝对位置等于把反馈环接回自己身上 —— 拖起来只有半速，还会漂。
        // QML 里没有 mapToGlobal()，但浮层是无边框窗口（窗口原点就是内容原点），
        // 所以「window.x + mouse.x」就是指针的屏幕坐标。
        MouseArea {
            id: moveArea
            anchors.fill: parent
            z: 1
            enabled: !liveSession.regionLocked && liveSession.phase !== "Idle"
            cursorShape: Qt.SizeAllCursor

            property point pressPoint   // 按下时的屏幕坐标
            property rect startGeom     // 按下时的窗口几何

            onPressed: function (mouse) {
                pressPoint = Qt.point(overlay.x + mouse.x, overlay.y + mouse.y)
                startGeom = Qt.rect(overlay.x, overlay.y, overlay.width, overlay.height)
                liveSession.setRegionDragging(true)
            }
            onPositionChanged: function (mouse) {
                if (!pressed)
                    return
                // 右边的 overlay.x 读的是「这次移动之前」的窗口位置（赋值在求值之后），
                // 所以换算出来的始终是指针此刻的屏幕坐标，不会读写同源。
                const sx = overlay.x + mouse.x
                const sy = overlay.y + mouse.y
                overlay.x = Math.round(startGeom.x + sx - pressPoint.x)
                overlay.y = Math.round(startGeom.y + sy - pressPoint.y)
            }
            onReleased: {
                liveSession.setRegionDragging(false)
                overlay.commitGeometry()
            }
        }

        // 八个手柄：四个角 + 四边中点。只改被按住的那一侧，另一侧钉死不动。
        Repeater {
            model: [
                { "h": "left",   "v": "top",    "cur": Qt.SizeFDiagCursor },
                { "h": "right",  "v": "top",    "cur": Qt.SizeBDiagCursor },
                { "h": "left",   "v": "bottom", "cur": Qt.SizeBDiagCursor },
                { "h": "right",  "v": "bottom", "cur": Qt.SizeFDiagCursor },
                { "h": "center", "v": "top",    "cur": Qt.SizeVerCursor },
                { "h": "center", "v": "bottom", "cur": Qt.SizeVerCursor },
                { "h": "left",   "v": "middle", "cur": Qt.SizeHorCursor },
                { "h": "right",  "v": "middle", "cur": Qt.SizeHorCursor }
            ]

            delegate: Rectangle {
                id: grip
                required property var modelData

                readonly property int side: 10

                visible: !liveSession.regionLocked && liveSession.phase !== "Idle"
                z: 2
                width: side
                height: side
                radius: side / 2
                // 手柄要压在译文上也看得见：实心强调色 + 一圈底色描边，
                // 比「白底白边」在浅色主题下清楚得多（白手柄在白底上等于没有）。
                color: Theme.primary
                border.width: 2
                border.color: Theme.surface

                // 贴着边框**内侧**摆，不要跨出去：窗口外的部分会被裁掉，
                // 四个角的手柄只剩半个圆，看着像渲染坏了。
                x: modelData.h === "left" ? 2
                   : modelData.h === "right" ? overlay.width - side - 2
                   : (overlay.width - side) / 2
                y: modelData.v === "top" ? 2
                   : modelData.v === "bottom" ? overlay.height - side - 2
                   : (overlay.height - side) / 2

                MouseArea {
                    anchors.fill: parent
                    cursorShape: grip.modelData.cur

                    property point pressPoint   // 按下时的屏幕坐标
                    property rect startGeom     // 按下时的窗口几何

                    onPressed: function (mouse) {
                        // 手柄自己的坐标 → 窗口坐标（手柄可能越出窗口半格）→ 屏幕坐标。
                        const p = grip.mapToItem(null, mouse.x, mouse.y)
                        pressPoint = Qt.point(overlay.x + p.x, overlay.y + p.y)
                        startGeom = Qt.rect(overlay.x, overlay.y, overlay.width, overlay.height)
                    }
                    onPositionChanged: function (mouse) {
                        if (!pressed)
                            return
                        // 手柄跟着窗口一起动，所以也只能用屏幕坐标算位移：
                        // 缩左边/上边时窗口自己就在移，窗口内坐标是半速的。
                        const p = grip.mapToItem(null, mouse.x, mouse.y)
                        const dx = overlay.x + p.x - pressPoint.x
                        const dy = overlay.y + p.y - pressPoint.y
                        let gx = startGeom.x
                        let gy = startGeom.y
                        let gw = startGeom.width
                        let gh = startGeom.height
                        if (grip.modelData.h === "left") { gx += dx; gw -= dx }
                        if (grip.modelData.h === "right") { gw += dx }
                        if (grip.modelData.v === "top") { gy += dy; gh -= dy }
                        if (grip.modelData.v === "bottom") { gh += dy }
                        // 反着拖到负数会让窗口翻转，直接卡在 40px 下限。
                        // 卡住的那一边要钉在原边界上：gx 还跟着指针走的话，
                        // 窗口会一边保持 40 宽、一边继续往右/下漂。
                        if (gw < 40) {
                            gw = 40
                            if (grip.modelData.h === "left")
                                gx = startGeom.x + startGeom.width - 40
                        }
                        if (gh < 40) {
                            gh = 40
                            if (grip.modelData.v === "top")
                                gy = startGeom.y + startGeom.height - 40
                        }
                        overlay.x = Math.round(gx)
                        overlay.y = Math.round(gy)
                        overlay.width = Math.round(gw)
                        overlay.height = Math.round(gh)
                    }
                    onReleased: overlay.commitGeometry()
                }
            }
        }
    }

    // 拖动结束才把新几何交回 C++（liveSession.setRegion 会重算物理像素并重抓一帧）。
    // 拖动过程中窗口自己挪，画面不跟着重抓 —— 否则 600ms 一拍 + 拖动 = 一堆废请求。
    function commitGeometry() {
        liveSession.setRegion(Qt.rect(overlay.x, overlay.y, overlay.width, overlay.height))
    }

    Component.onCompleted: {
        // 只交一次窗口句柄：C++ 记下它，加点击穿透 + 排除截图，并按 regionPhysical
        // 把窗口摆到选区的物理像素位置。之后每一单复用同一个窗口 ——
        // 每轮都新建 Window 会在开始实时翻译时闪一下，还会把上一份扩展样式丢掉。
        liveSession.attachPlateWindow(overlay)
    }
}
