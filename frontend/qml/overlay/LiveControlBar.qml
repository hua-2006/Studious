// C 页：实时翻译的控制条（设计稿 C2 运行中 / C4 已暂停 / C6 中断）。
//
// 为什么是独立窗口、而不是浮层里的一条工具栏：
// 译文浮层必须点击穿透（WS_EX_TRANSPARENT），穿透窗口里的按钮永远点不到；
// 而控制条要能点、能拖、能收 Esc，只能另开一个窗口。
//
// 它同样要「不进自己的截图」，否则下一轮 OCR 会把按钮上的中文当成原文再翻一遍；
// 但它**不能**穿透 —— 所以 attachControlWindow() 只排除截图，不加 TRANSPARENT。
//
// 显隐跟着 phase 走：会话一结束（Idle）它自己消失，调用方不用记得「顺便把那个
// 小窗口也收掉」。C++ 只切 phase 就够了。
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import Studious

Window {
    id: bar

    objectName: "liveControlBar"

    // Tool 位：不进任务栏、不占 Alt-Tab；置顶是必须的 —— 浮层在最上层，
    // 控制条得压在它上面才点得到。无边框 + 透明底，圆角和描边由 card 自己画。
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.NoDropShadowWindowHint
    color: "transparent"

    readonly property string phase: liveSession.phase
    readonly property bool running: phase === "Running"
    readonly property bool paused: phase === "Paused"
    readonly property bool errored: phase === "Error"
    // 「一打开就显示运行中」会让人以为已经翻好了。首帧回来之前叫「准备中」，
    // 有了第一次更新才是「运行中」——状态词只描述已经发生的事。
    readonly property bool warming: running && liveSession.updateCount <= 0
    // 「设置」小窗的开关。和截图浮层一样：再点一次收起来。
    property bool settingsOpen: false
    // 引擎下拉的开关。菜单画在同一个窗口里（窗口下方留出空间），
    // 不再用 Popup —— Popup 是独立顶层窗口，在置顶浮层旁边会被抢焦点、也拍不到。
    property bool engineMenuOpen: false
    // 语言菜单的开关："" = 收起，"src" = 源语言，"dst" = 目标语言。
    // 和引擎菜单一样画在窗口里，所以窗口高度也要跟着长。
    property string langMenuFor: ""
    // 第二行那句小字：非空才占位（Column 里 visible=false 的项不参与布局，不占高）。
    readonly property bool hasHint: liveSession.slowHint !== ""

    // 只在这三个状态出现；Idle 和任何还没初始化出来的 phase 一律不显示。
    // 这里**必须直接比较 phase 字符串**，不能写成 running || paused || errored：
    // 那三个是从 phase 派生出来的 bool，phase 一变 Qt 会先通知 visible 重算一次，
    // 此刻 running 已经 false、paused 还没 true → 窗口先隐后显。对用户来说是
    // 「点暂停，工具栏闪一下、并且被重新摆回初始位置」（隐一次会走 onVisibleChanged
    // 的复位逻辑，把用户拖过的位置和"本局已摆位"标记一起清掉）。
    visible: phase === "Running" || phase === "Paused" || phase === "Error"

    // 尺寸由内容撑开：三态控件数量不同，写死宽度会留一大片空白或把字截掉。
    // 菜单展开时窗口往下长出一块 —— 菜单是画在窗口里的（见 engineMenuOpen 的注释），
    // 窗口不长高就会被裁掉一半。
    width: card.width
    height: card.height + (engineMenuOpen ? engineMenu.height + Theme.s1 : 0)
            + (langMenuFor !== "" ? langMenu.height + Theme.s1 : 0)

    // Esc 要能收到键盘。Tool 窗口出现时不保证有焦点，所以每次显出来抢一次；
    // 这不影响浮层（那边是永远不抢焦点的）。
    // 收起来时顺手把「设置」小窗和引擎菜单一起关掉，免得留下孤儿窗口。
    onVisibleChanged: {
        if (visible) {
            requestActivate();
            // 控制条抬到译文浮层上面：浮层的原文底色是一整块窗口，控制条被摆进
            // 选区里时会被它压住（见 LiveSession::raiseControlWindow 的注释）。
            raise();
        } else {
            settingsOpen = false;
            engineMenuOpen = false;
            langMenuFor = "";
        }
    }

    // phase 变化时浮层会跟着显隐，可能又盖到控制条上；等这一轮布局跑完再抬一次。
    Connections {
        target: liveSession
        function onPhaseChanged() {
            if (bar.visible)
                Qt.callLater(bar.raise);
        }
    }

    // Alt-F4 关掉控制条而会话还在跑 = 浮层自己在那儿翻、谁都停不掉。
    // 关窗等同「停止」，语义和 Esc 一致。
    onClosing: liveSession.stop()

    Rectangle {
        id: card

        width: content.implicitWidth + Theme.s3 * 2
        height: content.implicitHeight + Theme.s3 * 2
        radius: Theme.rMd
        color: Theme.surfaceHigh
        border.width: 1
        border.color: Theme.outlineVariant

        // 拖动手柄：无边框窗口没有系统标题栏，整张卡片的空白处都能拖。
        // 按钮是后面声明的兄弟项，压在它上面，所以点按钮不会走到这里。
        MouseArea {
            id: dragArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeAllCursor

            property bool usingSystemMove: false
            property real pressScreenX: 0
            property real pressScreenY: 0
            property rect startGeom: Qt.rect(0, 0, 0, 0)

            onPressed: function (mouse) {
                // 按在卡片空白处也算「点到外面」：把引擎菜单收掉
                // （控制条是独立窗口，没有 Popup 那套 CloseOnPressOutside）。
                bar.engineMenuOpen = false;
                // 用户自己动过手了：这一单不再按选区自动摆位，拖到哪就停在哪。
                bar.userMoved = true;
                // 记屏幕坐标：窗口一动，窗口内的 mouse.x 就反着变，拿它算绝对位置
                // 只有半速还会漂。无边框窗口原点是内容原点，window.x + mouse.x 就是屏坐标。
                pressScreenX = bar.x + mouse.x;
                pressScreenY = bar.y + mouse.y;
                startGeom = Qt.rect(bar.x, bar.y, bar.width, bar.height);
                // 有系统拖拽就走系统的（不抖、能跨屏、贴边有系统反馈）；
                // 拿不到（个别平台/组合）就退回手工跟随，至少不会纹丝不动。
                usingSystemMove = bar.startSystemMove();
            }

            onPositionChanged: function (mouse) {
                if (!pressed || usingSystemMove)
                    return;
                bar.x = Math.round(startGeom.x + (bar.x + mouse.x) - pressScreenX);
                bar.y = Math.round(startGeom.y + (bar.y + mouse.y) - pressScreenY);
            }

            // 拖完再结算一次「有没有压到选区」：拖进选区要排除出抓屏，
            // 拖出来要还原，不能只在自动摆位那一下算。
            onReleased: bar.syncOverlap()
            onCanceled: bar.syncOverlap()
        }

        Column {
            id: content
            x: Theme.s3
            y: Theme.s3
            spacing: Theme.s2

            Row {
                id: mainRow
                // 行高统一：Row 不给子项做垂直对齐，各自等高才能中线对齐。
                height: 26
                spacing: Theme.s2

                // ---- 状态胶囊 ----
                // 底色用 good/caution 的容器色（深色主题下是深底亮字），不自己
                // rgba 调透明度 —— 同一屏在不同主题下浓淡才一致。
                Rectangle {
                    id: statusPill
                    visible: !bar.errored
                    height: mainRow.height
                    width: pillDot.width + Theme.s1 + pillLabel.implicitWidth + Theme.s2 * 2
                    radius: Theme.rFull
                    color: bar.paused ? Theme.cautionContainer
                                      : (bar.warming ? Theme.cautionContainer : Theme.goodContainer)

                    Rectangle {
                        id: pillDot
                        width: 6
                        height: 6
                        radius: 3
                        x: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        color: bar.paused ? Theme.caution
                                          : (bar.warming ? Theme.caution : Theme.good)
                    }

                    Text {
                        id: pillLabel
                        anchors.left: pillDot.right
                        anchors.leftMargin: Theme.s1
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s2
                        anchors.verticalCenter: parent.verticalCenter
                        text: bar.paused ? qsTr("已暂停")
                                         : (bar.warming ? qsTr("准备中") : qsTr("运行中"))
                        color: bar.paused ? Theme.caution
                                          : (bar.warming ? Theme.caution : Theme.good)
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                    }
                }

                // ---- 进度 ----
                // 暂停时也留着：用户最想知道的就是「停在哪了」，数字比分秒更实在。
                Text {
                    visible: !bar.errored
                    height: mainRow.height
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("已更新 %1 次").arg(liveSession.updateCount)
                    color: Theme.inkVariant
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeTiny
                }

                // ---- 中断：一行错因 ----
                // 错误正文可能是引擎吐的一整段（含 URL、退出码），所以限宽 + 省略，
                // 控制条不该被一句话撑成一整条横贯屏幕。
                Text {
                    visible: bar.errored
                    height: mainRow.height
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("已中断：识别失败")
                    color: Theme.danger
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                }

                Text {
                    visible: bar.errored && liveSession.errorText !== ""
                    height: mainRow.height
                    verticalAlignment: Text.AlignVCenter
                    width: Math.min(implicitWidth, 260)
                    text: liveSession.errorText
                    color: Theme.danger
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    elide: Text.ElideRight
                }

                VSep {}

                // ---- 当前引擎（可换）----
                // 和截图翻译工具栏上的引擎槽同一个意思：换了下一次请求就用新引擎，
                // 不重开这一单（LiveSession.setEngineId 会 retranslate 一次）。
                BarButton {
                    id: engineButton
                    objectName: "liveEngineButton"
                    // 显式引一次 backend.engineId：JS 函数体里的属性读取不会建依赖，
                    // 不这么写的话主页换引擎、这里还显示旧名字。
                    text: {
                        const mainEngine = backend.engineId
                        return bar.engineShortName() + (mainEngine === undefined ? "" : "")
                    }
                    onClicked: {
                        bar.langMenuFor = ""
                        bar.engineMenuOpen = !bar.engineMenuOpen
                    }
                }

                // ---- 语言对：[源] → [目标] ----
                // 和主页共用一份值；实时只贴一版译文，所以目标语言按单选处理。
                BarButton {
                    id: srcLangButton
                    objectName: "liveSrcLangButton"
                    text: bar.langLabel(backend.srcLang) + " ▾"
                    onClicked: {
                        bar.engineMenuOpen = false
                        bar.langMenuFor = bar.langMenuFor === "src" ? "" : "src"
                    }
                }

                Text {
                    height: mainRow.height
                    verticalAlignment: Text.AlignVCenter
                    text: "→"
                    color: Theme.inkVariant
                    font.pixelSize: Theme.fontSizeTiny
                }

                BarButton {
                    id: dstLangButton
                    objectName: "liveDstLangButton"
                    text: bar.langLabel((backend.dstLangs && backend.dstLangs.length > 0)
                                        ? backend.dstLangs[0] : "zh") + " ▾"
                    onClicked: {
                        bar.engineMenuOpen = false
                        bar.langMenuFor = bar.langMenuFor === "dst" ? "" : "dst"
                    }
                }

                // ---- 设置 ----
                // 和截图浮层的「设置」同一个意思：这次怎么翻的快捷开关（刷新间隔、
                // 区域边框强度…）。位置固定在 ✕ 左边，误点代价小。
                BarButton {
                    id: settingsButton
                    objectName: "liveSettingsButton"
                    text: qsTr("设置")
                    onClicked: bar.settingsOpen = !bar.settingsOpen
                }

                // ---- 分段：译文 / 原文 ----
                // 绑 liveSession.showOriginal，点击只调 toggleOriginal()：
                // 状态唯一来源在 C++，这里绝不自己改 flag，否则两边会打架。
                Rectangle {
                    id: segmented
                    // 暂停时也要留着：暂停只是「不再抓帧」，屏上那版译文还在，
                    // 用户这时最想对着原文核对——把开关藏掉等于逼他先继续。
                    visible: bar.running || bar.paused
                    height: mainRow.height
                    width: segRow.implicitWidth + 2
                    radius: Theme.rSm
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.outlineVariant

                    Row {
                        id: segRow
                        anchors.centerIn: parent
                        spacing: 0

                        Segment {
                            label: qsTr("译文")
                            active: !liveSession.showOriginal
                            onActivated: if (liveSession.showOriginal) liveSession.toggleOriginal()
                        }

                        Segment {
                            label: qsTr("原文")
                            active: liveSession.showOriginal
                            onActivated: if (!liveSession.showOriginal) liveSession.toggleOriginal()
                        }
                    }
                }

                VSep {}

                // ---- 锁定选区 ----
                // 解锁 = 浮层能接鼠标、八个手柄可拖；锁定 = 恢复点击穿透，
                // 手柄全部隐藏（LiveOverlay 那层直接不画）。
                BarButton {
                    id: lockButton
                    objectName: "liveLockButton"
                    text: liveSession.regionLocked ? qsTr("解锁选区") : qsTr("锁定选区")
                    onClicked: liveSession.regionLocked = !liveSession.regionLocked
                }

                // ---- 暂停 / 继续 ----
                // 暂停不动选区、也不清译文，只是不再抓帧；继续会立刻补一帧。
                BarButton {
                    visible: !bar.errored
                    text: bar.paused ? qsTr("继续") : qsTr("暂停")
                    onClicked: bar.paused ? liveSession.resume() : liveSession.pause()
                }

                // 中断了就没有「暂停」这回事，只给重来和关掉。
                BarButton {
                    visible: bar.errored
                    text: qsTr("重新开始")
                    primary: true
                    // 优先 start()：retranslate() 是给「同一单重跑」用的，
                    // 实时这一单识别挂了以后要按当前选区重新开局。
                    onClicked: liveSession.start(liveSession.regionLogical)
                }

                VSep {}

                // ---- 刷新：立刻采一帧 ----
                // 定时拍是 400~1500 ms 一拍，画面刚换完字（比如点了下一集）时
                // 用户不想干等下一拍。放在「暂停」右边的分隔符之后、✕ 之前：
                // 它和「暂停」一样是会话级的动作，不跟设置/显示形态混在一起。
                // 只在运行中给：暂停时没有"刷新"这回事（继续本来就会立刻补一帧）。
                BarButton {
                    id: refreshButton
                    objectName: "liveRefreshButton"
                    visible: bar.running
                    text: qsTr("刷新")
                    // 点下去必须有回执：主色实心闪 300 ms。
                    // 屏幕上文字没变时，这一次重翻的结果和原来逐字相同，浮层不会动；
                    // 只靠状态栏那行「正在翻译…」一闪，用户会以为点了没生效。
                    property bool flash: false
                    primary: flash
                    onClicked: {
                        refreshButton.flash = true;
                        refreshFlash.restart();
                        liveSession.refresh();
                    }
                    Timer {
                        id: refreshFlash
                        interval: 300
                        onTriggered: refreshButton.flash = false
                    }
                }

                // 右上角的 ✕：和「停止」同一个动作，位置固定在手最顺的角上。
                Rectangle {
                    id: closeButton
                    width: 22
                    height: mainRow.height
                    radius: Theme.rXs
                    color: closeHover.hovered ? Theme.surfaceHighest : "transparent"

                    Text {
                        anchors.centerIn: parent
                        text: "✕"
                        color: Theme.inkVariant
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                    }

                    HoverHandler {
                        id: closeHover
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: liveSession.stop()
                    }
                }
            }

            // ---- 第二行：慢速提示 ----
            // 宽度跟着第一行走、超了换行：卡片宽度由控件决定，不该被一句提示撑宽。
            Text {
                visible: bar.hasHint
                width: mainRow.implicitWidth
                text: liveSession.slowHint
                color: Theme.inkVariant
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                wrapMode: Text.Wrap
            }
        }
    }

    // 「设置」小窗：控制条的子窗口（owner 是本窗口），跟着它一起出现/收掉。
    // mode="live" 让它只显示实时翻译那几项，不掺截图翻译的开关。
    CaptureSettingsPanel {
        id: settingsPanel
        objectName: "liveSettingsPanel"
        mode: "live"
        visible: bar.settingsOpen && bar.visible
        onClosed: bar.settingsOpen = false
        onMoreSettingsRequested: {
            bar.settingsOpen = false;
            bar.moreSettingsRequested();
        }
    }

    // 页脚「更多设置」：停掉这一单，回主界面的「浮层」页。
    signal moreSettingsRequested()

    onSettingsOpenChanged: {
        if (settingsOpen && visible) {
            // 锚点要一路加到底：窗口 → 卡片 → content（它自己带 s3 的内边距）→ 行 → 按钮。
            // 漏掉 content.x/y 面板就会往左上偏一个内边距。
            settingsPanel.openAt(bar.x + card.x + content.x + mainRow.x + settingsButton.x,
                                 bar.y + card.y + content.y + mainRow.y + settingsButton.y,
                                 settingsButton.width,
                                 settingsButton.height);
        }
    }

    // ---- 语言对 ----
    // 和主界面、截图翻译、小窗共用一份值（backend.srcLang / backend.dstLangs）。
    // 这一条只负责显示成「[源] → [目标]」，改完下一帧就按新语言重翻。
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
            // 实时一次只贴一版译文，所以目标语言是单选；本地 MT 本来也只支持一个。
            backend.dstLangs = [code]
        }
        langMenuFor = ""
    }

    // ---- 引擎下拉 ----
    // 选项、状态点和「未就绪的行置灰」全部照抄截图翻译工具栏（CaptureOverlay 的
    // engineMenu），两处手感必须一样。
    // 值就是全局的 `settings/engine/id`（backend.engineId）—— 和主界面、截图翻译、
    // 小窗共用一个，这里**不保留**任何本会话覆盖。
    function engineOptions() {
        // id 列表和状态口径都在 Theme（四处共用一份，避免同一引擎多处异色）。
        const statuses = backend.engineStatuses
        const out = []
        for (let i = 0; i < Theme.engineIds.length; ++i) {
            const id = Theme.engineIds[i]
            let state = "ready"
            let detail = ""
            if (id !== "") {
                for (let j = 0; j < statuses.length; ++j) {
                    if (Theme.canonicalEngineId(statuses[j].id) === id) {
                        state = Theme.engineStateOf(statuses[j])
                        detail = statuses[j].detail || ""
                        break
                    }
                }
            }
            out.push({ "id": id, "label": backend.engineName(id), "state": state, "detail": detail })
        }
        return out
    }

    function engineShortName() {
        const list = engineOptions()
        for (let i = 0; i < list.length; ++i) {
            if (list[i].id === backend.engineId)
                return list[i].label
        }
        return qsTr("自动")
    }

    // 选引擎：先把这一单停住（在飞的请求也要掐掉，否则旧引擎的译文会晚一步回来
    // 覆盖新结果），换完再继续 —— 下一帧就用新引擎重翻。
    // 同时清掉 liveSession 的会话覆盖、写全局设置：四处（主页/截屏/实时/小窗）
    // 从此只有一个真值。LiveSession 那边检测到 settings 变了会强制重翻一帧。
    function selectEngine(uiEngineId) {
        const wasRunning = bar.running
        if (wasRunning)
            liveSession.pause()
        liveSession.engineId = ""
        backend.engineId = uiEngineId
        engineMenuOpen = false
        if (wasRunning)
            liveSession.resume()
    }

    // 菜单画在控制条窗口里，不用 Popup：Popup 是独立顶层窗口，在置顶浮层旁边
    // 会被抢焦点、也会被系统截图排除掉，而且 x/y 跟着 mapToItem 算容易飘。
    MenuPanel {
        id: engineMenu

        visible: bar.engineMenuOpen
        z: 5
        // 贴着「引擎」按钮的左边缘，落到卡片下面。x 要夹两头：只保证 ≥0 的话，
        // 按钮靠右时菜单右边会被窗口边界裁掉（菜单是画在窗口里的，不是 Popup）。
        x: Math.max(0, Math.min(Math.round(card.x + content.x + mainRow.x + engineButton.x),
                                bar.width - engineMenu.width))
        y: Math.round(card.y + card.height + Theme.s1)
        width: 168

        Repeater {
            model: bar.engineOptions()

            delegate: MenuRow {
                required property var modelData

                text: modelData.label
                selected: backend.engineId === modelData.id
                // 「自动」没有引擎状态，不画灯；其余每行都亮（绿/黄/红）。
                showDot: modelData.id !== ""
                dotColor: Theme.engineStateColor(modelData.state)
                // 缺资产 / 缺配置的行压暗且不响应。
                enabled: modelData.state !== "missing-assets"
                         && modelData.state !== "missing-config"
                onActivated: bar.selectEngine(modelData.id)
            }
        }
    }

    // 语言菜单：源/目标共用一块，跟着被点的那一格。同样画在窗口里（理由同上面）。
    Rectangle {
        id: langMenu

        visible: bar.langMenuFor !== ""
        z: 5

        readonly property bool forSource: bar.langMenuFor === "src"
        readonly property Item anchorItem: forSource ? srcLangButton : dstLangButton

        x: Math.max(0, Math.min(Math.round(card.x + content.x + mainRow.x + anchorItem.x),
                                bar.width - langMenu.width))
        y: Math.round(card.y + card.height + Theme.s1)
        width: 168
        height: langMenuColumn.implicitHeight + Theme.s1 * 2
        radius: Theme.rSm
        color: Theme.surfaceHigh
        border.width: 1
        border.color: Theme.outlineVariant

        Column {
            id: langMenuColumn
            anchors.fill: parent
            anchors.margins: Theme.s1
            spacing: 0

            Repeater {
                model: bar.langChoices(langMenu.forSource)

                delegate: Rectangle {
                    id: langOption
                    required property var modelData

                    readonly property bool current: (langMenu.forSource
                                                    ? (backend.srcLang || "auto")
                                                    : ((backend.dstLangs && backend.dstLangs.length > 0)
                                                       ? backend.dstLangs[0] : "zh")) === modelData.code

                    width: parent.width
                    height: 26
                    radius: Theme.rXs
                    color: langOptionHover.hovered ? Theme.surfaceHighest : "transparent"

                    HoverHandler { id: langOptionHover }

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.s2
                        anchors.rightMargin: Theme.s2
                        verticalAlignment: Text.AlignVCenter
                        text: langOption.modelData.label
                        elide: Text.ElideRight
                        color: langOption.current ? Theme.primary : Theme.ink
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: langOption.current ? Font.DemiBold : Font.Normal
                    }

                    TapHandler {
                        onTapped: bar.chooseLang(langOption.modelData.code)
                    }
                }
            }
        }
    }

    // 控制条自己收起时，设置小窗不能留成孤儿窗口。
    // 取证钩子用：STUDIOUS_UI_TARGET=liveControlBar + STUDIOUS_UI_CALL=toggleSettings
    function toggleSettings() {
        settingsOpen = !settingsOpen;
    }
    // 同上，用来验证「锁定/解锁选区」真的切了穿透位。
    function toggleLock() {
        liveSession.regionLocked = !liveSession.regionLocked;
    }
    // 取证钩子：STUDIOUS_UI_CALL=liveControlBar:toggleEngineMenu
    function toggleEngineMenu() {
        engineMenuOpen = !engineMenuOpen;
    }
    // Esc 收起整单。放在窗口根上而不是卡片里：卡片里的子项可能被重建，
    // 焦点项只留一个，行为才不会随状态切换漂移。
    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: {
            if (bar.langMenuFor !== "")
                bar.langMenuFor = "";
            else if (bar.engineMenuOpen)
                bar.engineMenuOpen = false;
            else if (bar.settingsOpen)
                bar.settingsOpen = false;
            else
                liveSession.stop();
        }
    }

    Component.onCompleted: {
        // 交句柄给 C++：它把本窗口登记成「排除截图」（WDA_EXCLUDEFROMCAPTURE），
        // 但不设点击穿透 —— 这里的按钮是要点的。
        liveSession.attachControlWindow(bar)
        // attach 不负责摆位。这一单真正的位置要等 region 定下来才算得出来，
        // 所以这里只是先算一次（多半还是空选区、直接返回），真正的摆位靠下面
        // 监听 region/phase 变化后的 schedulePlace()。
        bar.schedulePlace()
    }

    // ---- 摆位 ----
    // 之前只在 Component.onCompleted 调 placeNearRegion()：那会儿 regionLogical 还是
    // 空的，函数直接 return，之后再没人叫它，于是窗口一直停在 Qt 的默认位置
    // （屏幕正中）。现在改成「会话一开就摆，用户拖过就不再抢」。
    property bool placedForSession: false
    property bool userMoved: false

    Connections {
        target: liveSession
        function onRegionChanged() { bar.schedulePlace() }
        function onPhaseChanged() {
            // 只有真正结束（Idle）才复位：下一单重新按选区摆位，而不是沿用上一单
            // 被拖到的地方。以前这个复位挂在 onVisibleChanged(false) 上，而窗口在
            // Running↔Paused 之间会瞬隐一次，于是"点暂停 → 工具栏跳回初始位置"。
            if (bar.phase === "Idle") {
                bar.placedForSession = false;
                bar.userMoved = false;
            }
            bar.schedulePlace();
        }
    }

    // 这一帧可能还没布局完（宽度是 0），推迟到下一轮事件循环再算。
    function schedulePlace() {
        if (!bar.visible)
            return;
        if (bar.placedForSession || bar.userMoved)
            return;
        Qt.callLater(bar.placeNearRegion);
    }

    // 默认位置：贴在实时选区正上方、水平居中；贴到屏幕顶放不下就落到选区下方；
    // 上下都放不下（选区几乎占满整屏）就塞进选区贴底 —— 否则会被推到屏幕外。
    // 用户自己拖过就不再算（拖到哪就是哪，下次开局别抢回来）。
    function placeNearRegion() {
        if (bar.userMoved || bar.placedForSession || bar.width <= 0)
            return;
        const r = liveSession.regionLogical;
        if (!r || r.width <= 0 || r.height <= 0)
            return;
        const deskW = Screen.desktopAvailableWidth;
        const deskH = Screen.desktopAvailableHeight;
        const gap = Theme.s2;
        let px = Math.round(r.x + (r.width - bar.width) / 2);
        px = Math.max(gap, Math.min(px, deskW - bar.width - gap));
        let py = Math.round(r.y - bar.height - gap);
        if (py < gap)
            py = Math.round(r.y + r.height + gap);
        if (py + bar.height > deskH - gap) {
            // 全屏选区：上方是负坐标、下方又出界，只能放进来（贴底，尽量少挡译文）。
            py = Math.round(r.y + r.height - bar.height - Theme.s3);
            py = Math.max(gap, Math.min(py, deskH - bar.height - gap));
        }
        bar.x = px;
        bar.y = py;
        bar.placedForSession = true;
        bar.syncOverlap();
        bar.raise();
    }

    // 工具栏压在选区上时，得让后端把它排除出抓屏：控制条平时故意"能被拍到"
    // （用户要截图反馈），可一旦落在选区里，下一帧 OCR 就会把「暂停 / 刷新」
    // 这些按钮文字当成原文再翻一遍。
    function syncOverlap() {
        const r = liveSession.regionLogical;
        if (!r || r.width <= 0 || r.height <= 0)
            return;
        const overlaps = bar.x < r.x + r.width && bar.x + bar.width > r.x
                      && bar.y < r.y + r.height && bar.y + bar.height > r.y;
        liveSession.setControlBarOverlapping(overlaps);
    }

    // 内联按钮：跟 CaptureOverlay 的 OverlayButton 同一套观感 ——
    // 主操作强调色实心，次操作透明底 + 悬浮高亮；危险动作用 danger 描边，
    // 不填色（填一片红会把整条控制条的视觉重心带跑）。
    component BarButton: Rectangle {
        id: bb

        property string text: ""
        property bool primary: false
        property bool danger: false

        signal clicked()

        width: bbLabel.implicitWidth + Theme.s4 * 2
        // 26 = mainRow 的行高。内联组件是独立组件，取不到外层 id（写 mainRow.height
        // 编译期就报未定义），所以行高只能在这几个组件里各写一遍 —— 改行高要一起改。
        height: 26
        radius: Theme.rSm
        color: bb.primary
               ? (bbHover.hovered ? Qt.lighter(Theme.primary, 1.08) : Theme.primary)
               : (bbHover.hovered ? Theme.surfaceHighest : "transparent")
        border.width: bb.primary ? 0 : 1
        border.color: bb.danger
                      ? Theme.danger
                      : (bbHover.hovered ? Theme.outline : Theme.outlineVariant)
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        Text {
            id: bbLabel
            anchors.centerIn: parent
            text: bb.text
            color: bb.primary ? Theme.primaryText : (bb.danger ? Theme.danger : Theme.ink)
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: bb.primary ? Font.DemiBold : Font.Normal
        }

        HoverHandler {
            id: bbHover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: bb.clicked()
        }
    }

    // 分段控件的一格。选中态只铺 surfaceHighest + 加粗，不用强调色 ——
    // 这条控制条上「重新开始 / 暂停」才是主操作，这里再抢色就主次不分了。
    component Segment: Rectangle {
        id: seg

        property string label: ""
        property bool active: false

        signal activated()

        width: segLabel.implicitWidth + Theme.s3 * 2
        // 24 = 行高 26 减掉分段框上下各 1px 的描边（描边由外面的 segmented 画）。
        height: 24
        radius: Theme.rXs
        color: seg.active ? Theme.surfaceHighest
                          : (segHover.hovered
                             ? Qt.rgba(Theme.surfaceHighest.r, Theme.surfaceHighest.g,
                                       Theme.surfaceHighest.b, 0.45)
                             : "transparent")

        Text {
            id: segLabel
            anchors.fill: parent
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: seg.label
            color: seg.active ? Theme.ink : Theme.inkVariant
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: seg.active ? Font.DemiBold : Font.Normal
        }

        HoverHandler {
            id: segHover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: seg.activated()
        }
    }

    // 竖向分隔线。外面包一层等高的 Item：Row 不给子项做垂直居中，
    // 1×18 的线直接塞进去会贴在行的顶边上（看起来像"偏上"）。
    component VSep: Item {
        width: 1
        height: 26

        Rectangle {
            width: 1
            height: 18
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.outlineVariant
        }
    }
}
