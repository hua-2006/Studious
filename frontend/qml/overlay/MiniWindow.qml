// 小窗口翻译（设计稿 B 页）：一个置顶小窗，输入/粘贴/划词取词 → 翻译 → 逐段结果。
//
// 分工：状态与结果全在 C++ 的 MiniSession（上下文属性 miniSession），这里只画界面。
// 为什么不用 Popup 做下拉：Popup 是独立顶层窗口，在这种常驻置顶的小窗旁边会被抢焦点，
// 系统截图也拍不到；下面两个菜单都是窗口内自己画的 Rectangle。
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

Window {
    id: win
    objectName: "miniWindow"

    // 设置面板里的「更多设置」：小窗不自己切页（它不知道主窗口有几页），
    // 交给 Main.qml 接过去显示主窗口并跳到「浮层」页。
    signal moreSettingsRequested()

    // 尺寸照设计稿：560 宽、558 高（B1~B8 共用同一壳）。
    width: 560
    height: 558
    color: "transparent"
    // 置顶开关的**本地快照**：只在构造时和用户切换时写，不直接绑 backend。
    // flags 里挂表达式绑定的话，用户一点「置顶」= 改 flags = Qt 销毁并重建原生窗口：
    // 焦点丢、闪一下白、输入法预编辑串全没。所以 flags 值定下来，切换走 applyPinned()，
    // 由它负责把位置、层级和键盘焦点还回去。
    property bool pinned: false
    // 置顶重建窗口的那一小段（见 applyPinned / onVisibleChanged）。
    property bool relayingPin: false

    // 无边框 + 置顶；置顶与否跟设置走（settings/mini/alwaysOnTop）。
    flags: Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
           | (win.pinned ? Qt.WindowStaysOnTopHint : Qt.WindowType(0))
    // 独立窗口：不能被主窗口「顺手收走」（主窗口最小化时它要留在屏幕上）。
    transientParent: null
    title: qsTr("小窗翻译")

    // 译文正文字号：只作用于译文/原文正文，控件字号另有 Theme.fontScale 统一缩放。
    readonly property int readingPx: Math.max(11, Math.round(backend.readingFontSize * Theme.fontScale))

    readonly property color ink: Theme.ink
    readonly property color inkVariant: Theme.inkVariant
    readonly property color line: Theme.outlineVariant

    // ---- 页签：翻译 / 记录
    property string tab: "translate"
    // ---- 两个下拉：0 = 都收着，1 = 源语言，2 = 目标语言
    property int openMenu: 0
    // ---- 引擎下拉（左下角）：和主页、截图翻译、实时翻译共用同一个设置
    property bool engineMenuOpen: false
    // ---- 记录页签里选中的那一条（-1 = 没选）
    property int recordIndex: -1
    property var records: []
    // 详情里那段正文。刻意用属性而不是从 delegate 里直接写 <TextEdit 的 id>：
    // 列表刷新（Repeater 重建 delegate）或点得极快时，跨 item 的 id 引用会拿到
    // 已销毁的对象，QML 会直接崩在 JS 栈里。
    property string recordDetailText: ""
    // 保存结果的一句回话（历史没开时告诉用户为什么没存进去）
    property string saveHint: ""

    // 目标语言的显示名（空串 = 未知，退回语言码）。
    function langName(code) {
        if (!code || code.length === 0)
            return qsTr("自动检测 (auto)")
        const list = backend.languages || []
        for (let i = 0; i < list.length; ++i) {
            if (list[i].code === code)
                return list[i].label
        }
        return code
    }

    function dstSummary() {
        const dsts = miniSession.dstLangs || []
        if (dsts.length === 0)
            return qsTr("选择目标语言")
        if (dsts.length === 1)
            return langName(dsts[0])
        return langName(dsts[0]) + " +" + (dsts.length - 1)
    }

    function toggleDst(code) {
        const current = (miniSession.dstLangs || []).slice()
        // 当前引擎（本地 MT）只能翻一个目标语言：单选式替换，不累加。
        if (!backend.dstMultiSupported) {
            miniSession.dstLangs = [code]
            openMenu = 0
            return
        }
        const at = current.indexOf(code)
        if (at >= 0) {
            if (current.length === 1)   // 至少留一个，不然翻译按钮就没意义了
                return
            current.splice(at, 1)
        } else {
            current.push(code)
        }
        miniSession.dstLangs = current
    }

    function refreshRecords() {
        records = backend.historyRecords(50)
    }

    // 取证钩子（--ui-shot 的 STUDIOUS_UI_CALL 会调它）：切到某个页签。
    // QML 函数的形参在元对象里是 QVariant，invokeMethod 塞 QString 会匹配不上，
    // 所以给两个零参版本给截图用。
    function showTab(name) {
        tab = name
        openMenu = 0
        if (name === "records")
            refreshRecords()
    }
    function showTranslateTab() { showTab("translate") }
    function showRecordsTab() { showTab("records") }

    // 点列表行：再点一次收起。正文在这里一次性取好放进属性，
    // 详情面板只做绑定，不在 delegate 里跨 item 抓引用。
    function selectRecord(index) {
        recordIndex = (index >= 0 && index === recordIndex) ? -1 : index
        var rec = recordIndex >= 0 && recordIndex < records.length ? records[recordIndex] : null
        recordDetailText = rec ? (rec.translatedText || rec.originalText || "") : ""
    }

    // 取证钩子：打开第一条记录的详情（--ui-shot 只能调零参方法）。
    function debugOpenFirstRecord() {
        refreshRecords()
        if (records.length > 0)
            selectRecord(0)
    }

    // 取证钩子（--ui-shot 只能调零参方法）：改一次语言，用来验证"小窗改了、
    // 主页/截屏/实时跟着变"这条同步链。
    function debugSrcJapanese() { miniSession.srcLang = "ja" }
    function debugDstJapanese() { toggleDst("ja") }

    function sourceLabel(kind) {
        if (kind === "mini")
            return qsTr("小窗翻译")
        if (kind === "live")
            return qsTr("实时翻译")
        return qsTr("截屏翻译")
    }

    function recordTime(iso) {
        if (!iso || iso.length < 16)
            return iso || ""
        return iso.substring(5, 16).replace("T", " ")
    }

    function recordPreview(item) {
        const text = (item.translatedText || "").trim()
        if (text.length > 0)
            return text.split("\n")[0]
        const src = (item.originalText || "").trim()
        return src.length > 0 ? src.split("\n")[0] : qsTr("（空）")
    }

    // 把「置顶」落到 flags 上。Windows 上改 flags 会让 Qt 重建原生窗口，
    // 所以先记下位置，重建后在下一轮事件循环把位置、层级、焦点都还回去。
    // relayingPin 期间 visible 可能短暂掉一下，onVisibleChanged 不能当成
    // 「窗口被重新打开」来处理（否则会话会被 prepare 一遍，设置面板也会被收掉）。
    function applyPinned(on, restoreFocus) {
        if (win.pinned === on)
            return
        const px = win.x
        const py = win.y
        const wasVisible = win.visible
        win.relayingPin = true
        win.pinned = on
        Qt.callLater(function () {
            win.x = px
            win.y = py
            if (wasVisible && win.visible) {
                win.raise()
                win.requestActivate()
                if (restoreFocus)
                    Qt.callLater(function () { input.forceActiveFocus() })
            }
            win.relayingPin = false
        })
    }

    // 顶栏「置顶」按钮：C++ 是唯一状态源，先写它，再按新值重设 flags。
    function togglePin() {
        backend.miniAlwaysOnTop = !backend.miniAlwaysOnTop
        applyPinned(backend.miniAlwaysOnTop, true)
    }

    // 构造时先把置顶对齐：QML 的 Window 是懒创建原生窗口的，此刻改 flags 不花代价，
    // 也就不用等第一次 show 才去纠正层级（那一下会闪）。
    Component.onCompleted: win.pinned = backend.miniAlwaysOnTop

    onVisibleChanged: {
        if (visible) {
            if (win.relayingPin) {
                // 只是置顶重建把 visible 抖了一下：别重跑 prepare()，把焦点还给输入框就行。
                Qt.callLater(function () { input.forceActiveFocus() })
                return
            }
            miniSession.prepare()
            refreshRecords()
            // 先把置顶/层级对齐（此时窗口刚显出来，改 flags 不留残影），
            // 再抬原生窗口，最后才动 QML 的 activeFocus。
            // 反过来写的话 requestActivate() 会把刚设好的输入焦点抢走：
            // 窗口看着在最前面，光标却不在输入框里。
            applyPinned(backend.miniAlwaysOnTop, false)
            requestActivate()
            Qt.callLater(function () { input.forceActiveFocus() })
        } else {
            if (win.relayingPin)
                return
            openMenu = 0
            miniSettings.visible = false
        }
    }

    // ---------------------------------------------------------- 引擎（左下角）
    // 和主页、截图翻译、实时翻译共用一个值：`settings/engine/id`（backend.engineId）。
    // 这里不做任何"本窗覆盖" —— 在这切了，主页和控制条下一帧就跟着变。
    function engineName() {
        // 名字统一由后端给（同一个 id 在四个入口里必须长得一样）。
        return backend.engineName(backend.engineId)
    }

    function engineChoices() {
        const ids = ["", "web", "llm-cloud", "llm-local", "local-mt"]
        const labels = [backend.engineName(""), backend.engineName("web"), backend.engineName("llm-cloud"),
                        backend.engineName("llm-local"), backend.engineName("local-mt")]
        const statuses = backend.engineStatuses
        const out = []
        for (let i = 0; i < ids.length; ++i) {
            let state = "ready"
            for (let j = 0; j < statuses.length; ++j) {
                if (statuses[j].id === ids[i]) {
                    state = statuses[j].state
                    break
                }
            }
            out.push({ "id": ids[i], "label": labels[i], "state": state })
        }
        return out
    }

    function engineDotColor(state) {
        if (state === "ready")
            return Theme.good
        if (state === "missing-assets")
            return Theme.danger
        return Theme.caution
    }

    function selectEngine(uiEngineId) {
        engineMenuOpen = false
        openMenu = 0
        backend.engineId = uiEngineId
    }

    // 截图取证用：--ui-shot 的 STUDIOUS_UI_CALL 只认零参方法
    function toggleEngineMenu() {
        openMenu = 0
        engineMenuOpen = !engineMenuOpen
    }

    Connections {
        target: miniSession
        function onSaveRecordRequested(originals, blocks, engineLabel) {
            const ok = backend.saveMiniRecord(originals, blocks, engineLabel)
            win.saveHint = ok ? qsTr("已保存到记录。")
                              : qsTr("没存进去：历史对话是关着的，去「设置 · 缓存」里打开。")
            if (ok)
                win.refreshRecords()
        }
        function onPhaseChanged() {
            if (miniSession.phase !== "Translating")
                win.openMenu = 0
            if (miniSession.phase === "Translating")
                win.saveHint = ""
        }
        // C++ → 输入框单向灌入（划词获取、清空、换源文）。输入框自己那边只往 C++
        // 写，两边都不去改对方的绑定，所以不会有「首次键入后绑定失效」那一出。
        function onSourceTextChanged() {
            if (input.text === miniSession.sourceText)
                return
            const atEnd = input.cursorPosition >= input.text.length
            input.text = miniSession.sourceText
            if (atEnd)
                input.cursorPosition = input.text.length
        }
    }

    Connections {
        target: backend
        // 置顶也可能从「设置」里改。没显示时直接对齐（那时改不会闪），
        // 显示着就走同一条「记位置 → 重设 → 还原」的路。
        function onPrefsChanged() {
            win.applyPinned(backend.miniAlwaysOnTop, false)
        }
    }

    // ---------------------------------------------------------------- 外壳
    Rectangle {
        id: card
        anchors.fill: parent
        radius: 20
        color: Theme.surface
        border.width: 1
        border.color: line

        // Esc 挂在卡片上，不挂在 Window 上：Window 不是 Item，Qt 不会把 Keys
        // 附上去（日志里那句 "Could not attach Keys property" 就是它），
        // 键盘事件从有焦点的子项往上冒，到这张卡片就能接住。
        Keys.onEscapePressed: function (event) {
            if (win.openMenu !== 0 || win.engineMenuOpen) {
                win.openMenu = 0
                win.engineMenuOpen = false
                event.accepted = true
            } else if (miniSettings.visible) {
                miniSettings.visible = false
                event.accepted = true
            } else if (miniSession.phase === "Translating") {
                miniSession.cancel()
                event.accepted = true
            } else {
                win.close()
                event.accepted = true
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 0
            spacing: 0

            // ---------------------------------------------------- 顶栏
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 46

                // 整条顶栏可拖动（按钮浮在上面，先吃掉自己的点击）。
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeAllCursor
                    onPressed: win.startSystemMove()
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s4
                    anchors.rightMargin: Theme.s2
                    spacing: Theme.s1

                    Repeater {
                        model: [{ code: "translate", label: qsTr("翻译") },
                                { code: "records", label: qsTr("记录") }]
                        delegate: Item {
                            required property var modelData
                            Layout.preferredWidth: tabText.implicitWidth + Theme.s3 * 2
                            Layout.fillHeight: true

                            Text {
                                id: tabText
                                anchors.centerIn: parent
                                text: modelData.label
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: win.tab === modelData.code ? Font.DemiBold : Font.Normal
                                color: win.tab === modelData.code ? Theme.primary : inkVariant
                            }
                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: tabText.implicitWidth
                                height: 2
                                radius: 1
                                color: Theme.primary
                                visible: win.tab === modelData.code
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    win.tab = modelData.code
                                    win.openMenu = 0
                                    if (modelData.code === "records")
                                        win.refreshRecords()
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }

                    // 置顶 / 设置：同一套文字按钮（26 高、rSm、左右 s3、悬浮 surfaceHigh、
                    // fontSizeSmall）。之前置顶是 28×28 还把字号压到 10，「置顶」两个字
                    // 被挤在方框里，跟旁边的「设置」完全不是一个观感。
                    Rectangle {
                        objectName: "miniPinButton"
                        Layout.preferredWidth: pinLabel.implicitWidth + Theme.s3 * 2
                        Layout.preferredHeight: 26
                        radius: Theme.rSm
                        color: pinHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: pinLabel
                            anchors.centerIn: parent
                            text: qsTr("置顶")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: backend.miniAlwaysOnTop ? Theme.primary : inkVariant
                        }
                        HoverHandler { id: pinHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: win.togglePin()
                        }
                    }

                    // 设置（跟截图翻译共用那块小窗设置）
                    Rectangle {
                        id: gearButton
                        objectName: "miniSettingsButton"
                        // 和工具栏上那颗「设置」一样用文字，不用齿轮字形：
                        // ⚙ 是彩色 emoji 块，跟这套单色图标不是一个画风。
                        Layout.preferredWidth: settingsLabel.implicitWidth + Theme.s3 * 2
                        Layout.preferredHeight: 26
                        radius: Theme.rSm
                        color: gearHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: settingsLabel
                            anchors.centerIn: parent
                            text: qsTr("设置")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: inkVariant
                        }
                        HoverHandler { id: gearHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                win.openMenu = 0
                                // 面板位置每次打开都按**当前**窗口坐标现算（见下面
                                // CaptureSettingsPanel 里的注释）：算一次存着，用户
                                // 把小窗拖走后面板就弹到屏幕角落去了。
                                if (miniSettings.visible) {
                                    miniSettings.visible = false
                                } else {
                                    // 齿轮按钮此刻在屏幕上的位置：它自己的坐标 → 窗口
                                    // 坐标 → 屏幕坐标（无边框窗口原点是内容原点）。
                                    const p = gearButton.mapToItem(null, 0, 0)
                                    miniSettings.openAt(win.x + p.x, win.y + p.y,
                                                        gearButton.width, gearButton.height)
                                    miniSettings.visible = true
                                }
                            }
                        }
                    }

                    // 关闭（和另外两处 ✕ 同一套尺寸：22 宽 / 26 高 / rSm / 悬浮 surfaceHigh）
                    Rectangle {
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 26
                        radius: Theme.rSm
                        color: closeHover.hovered ? Theme.surfaceHighest : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }
                        HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: win.close() }
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: line
                }
            }

            // ---------------------------------------------------- 翻译页
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.s4
                spacing: Theme.s3
                visible: win.tab === "translate"

                // 语言行
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    // 源语言
                    Rectangle {
                        id: srcField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        radius: Theme.rSm
                        color: win.openMenu === 1 ? Theme.surfaceHigh : Theme.surfaceLow
                        border.width: 1
                        border.color: win.openMenu === 1 ? Theme.primary : line

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.s3
                            anchors.right: parent.right
                            anchors.rightMargin: 26
                            text: win.langName(miniSession.srcLang)
                            elide: Text.ElideRight
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            color: ink
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.s2
                            text: "▾"
                            font.pixelSize: 10
                            color: inkVariant
                        }
                        TapHandler {
                            onTapped: win.openMenu = (win.openMenu === 1 ? 0 : 1)
                        }
                    }

                    // 目标语言（多选）
                    Rectangle {
                        id: dstField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        radius: Theme.rSm
                        color: win.openMenu === 2 ? Theme.surfaceHigh : Theme.surfaceLow
                        border.width: 1
                        border.color: win.openMenu === 2 ? Theme.primary : line

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s3
                            anchors.rightMargin: Theme.s2
                            spacing: Theme.s1

                            Text {
                                Layout.fillWidth: true
                                text: win.dstSummary()
                                elide: Text.ElideRight
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                color: ink
                            }
                            Rectangle {
                                Layout.preferredWidth: multiLabel.implicitWidth + 10
                                Layout.preferredHeight: 18
                                radius: 4
                                color: Theme.surfaceHighest
                                Text {
                                    id: multiLabel
                                    anchors.centerIn: parent
                                    // 本地 MT 只能翻一个目标语言，别再显示「多选」误导用户。
                                    text: backend.dstMultiSupported ? qsTr("多选") : qsTr("单选")
                                    font.family: Theme.fontUi
                                    font.pixelSize: 9
                                    color: inkVariant
                                }
                            }
                            Text {
                                text: "▾"
                                font.pixelSize: 10
                                color: inkVariant
                            }
                        }
                        TapHandler {
                            onTapped: win.openMenu = (win.openMenu === 2 ? 0 : 2)
                        }
                    }
                }

                // 输入区
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: !resultArea.visible
                    Layout.preferredHeight: resultArea.visible ? 96 : -1
                    Layout.minimumHeight: 96
                    radius: Theme.rSm
                    color: Theme.surfaceLow
                    border.width: 1
                    border.color: input.activeFocus ? Theme.primary : line
                    clip: true

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: Theme.s2
                        width: tagText.implicitWidth + 10
                        height: 16
                        radius: 4
                        color: Theme.surfaceHighest
                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text: miniSession.sourceTag === "clip" ? qsTr("划词获取") : qsTr("手动输入")
                            font.family: Theme.fontUi
                            font.pixelSize: 9
                            color: inkVariant
                        }
                    }

                    ScrollView {
                        anchors.fill: parent
                        anchors.topMargin: 26
                        anchors.margins: Theme.s2
                        clip: true

                        TextEdit {
                            id: input
                            objectName: "miniInput"
                            // 这里故意不写 text: miniSession.sourceText。那样一来
                            // onTextChanged 里回写 miniSession.sourceText 就把这条绑定
                            // 打断了 —— 首次键入之后绑定永久失效，「划词获取」和清空
                            // 再也灌不进这个框。C++ → 输入框改成走
                            // Connections.onSourceTextChanged() 单向灌入。
                            wrapMode: TextEdit.Wrap
                            selectByMouse: true
                            color: ink
                            selectionColor: Theme.primary
                            selectedTextColor: Theme.primaryText
                            font.family: Theme.fontContent
                            font.pixelSize: Theme.fontSizeBody
                            // 首帧的初值：窗口是懒创建的，构造时 C++ 里可能已经有
                            // 划词来的文本了。
                            Component.onCompleted: text = miniSession.sourceText
                            onTextChanged: {
                                if (miniSession.sourceText !== text)
                                    miniSession.sourceText = text
                            }
                            Keys.onPressed: function (event) {
                                if (event.key === Qt.Key_Return && (event.modifiers & Qt.ControlModifier)) {
                                    miniSession.translate()
                                    event.accepted = true
                                }
                            }
                        }
                    }

                    Text {
                        anchors.fill: parent
                        anchors.topMargin: 26 + Theme.s2
                        anchors.leftMargin: Theme.s3
                        anchors.rightMargin: Theme.s2
                        visible: miniSession.sourceText.length === 0
                        text: qsTr("输入或粘贴要翻译的文本")
                        wrapMode: Text.Wrap
                        font.family: Theme.fontContent
                        font.pixelSize: Theme.fontSizeBody
                        color: inkVariant
                    }
                }

                // 字数 · 段数
                Text {
                    Layout.fillWidth: true
                    visible: miniSession.paragraphCount > 0
                    text: qsTr("%1 字 · %2 段").arg(miniSession.charCount).arg(miniSession.paragraphCount)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: inkVariant
                }

                // 划词热键的说明（图片 / 空 / 太长只填不翻）；平时不占位。
                Text {
                    Layout.fillWidth: true
                    visible: miniSession.noticeText.length > 0
                    text: miniSession.noticeText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.caution
                }

                // 结果区
                Rectangle {
                    id: resultArea
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: miniSession.phase === "Done" || miniSession.phase === "Failed"
                             || miniSession.phase === "Translating"
                    radius: Theme.rSm
                    color: Theme.surfaceLow
                    border.width: 1
                    border.color: miniSession.phase === "Failed" ? Theme.danger : line
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.s3
                        spacing: Theme.s2

                        // 结果头：目标语言标签页 + 引擎 + 复制
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s2

                            Repeater {
                                model: miniSession.dstLangs
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property bool current: miniSession.activeDst === modelData
                                    Layout.preferredHeight: 22
                                    Layout.preferredWidth: langTab.implicitWidth + 16
                                    radius: Theme.rXs
                                    color: current ? Theme.primaryContainer : "transparent"
                                    Text {
                                        id: langTab
                                        anchors.centerIn: parent
                                        text: win.langName(modelData)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: current ? Theme.primaryContainerText : inkVariant
                                    }
                                    TapHandler {
                                        onTapped: miniSession.activeDst = modelData
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Text {
                                visible: miniSession.engineLabel.length > 0
                                text: miniSession.engineLabel
                                elide: Text.ElideRight
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: inkVariant
                            }
                        }

                        // 翻译中
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: Theme.s3
                            visible: miniSession.phase === "Translating"
                            Item { Layout.fillWidth: true; Layout.fillHeight: true }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: qsTr("正在翻译…")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                color: inkVariant
                            }
                            Item { Layout.fillWidth: true; Layout.fillHeight: true }
                        }

                        // 失败
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: Theme.s2
                            visible: miniSession.phase === "Failed"
                            Item { Layout.fillHeight: true }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("没有翻译成功")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                                color: Theme.dangerContainerText
                            }
                            Text {
                                Layout.fillWidth: true
                                text: miniSession.errorText
                                wrapMode: Text.Wrap
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.dangerContainerText
                            }
                            Item { Layout.fillHeight: true }
                        }

                        // 成功：逐段
                        ListView {
                            id: blockList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            visible: miniSession.phase === "Done"
                            clip: true
                            spacing: Theme.s2
                            model: miniSession.blocks

                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                            delegate: Item {
                                required property var modelData
                                width: blockList.width
                                height: blockText.implicitHeight + Theme.s2 * 2
                                        + (sourceLine.visible ? sourceLine.implicitHeight + 2 : 0)

                                Rectangle {
                                    id: blockBg
                                    anchors.fill: parent
                                    radius: Theme.rXs
                                    color: blockHover.hovered ? Theme.surfaceHigh : "transparent"
                                }
                                HoverHandler { id: blockHover }

                                Text {
                                    id: blockIndex
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.topMargin: Theme.s2
                                    anchors.leftMargin: Theme.s2
                                    width: 20
                                    text: ("0" + modelData.index).slice(-2)
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: Theme.primary
                                }

                                // 「译文带原文」打开时，每段译文上方多一行原文（小字、灰一点）。
                                Text {
                                    id: sourceLine
                                    visible: backend.showSource
                                    anchors.left: blockIndex.right
                                    anchors.right: parent.right
                                    anchors.rightMargin: 26
                                    anchors.top: parent.top
                                    anchors.topMargin: Theme.s2
                                    text: modelData.source
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    font.family: Theme.fontContent
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: inkVariant
                                    opacity: 0.8
                                }

                                Text {
                                    id: blockText
                                    anchors.left: blockIndex.right
                                    anchors.right: parent.right
                                    anchors.rightMargin: 26
                                    anchors.top: sourceLine.visible ? sourceLine.bottom : parent.top
                                    anchors.topMargin: sourceLine.visible ? 3 : Theme.s2
                                    text: modelData.text.length > 0 ? modelData.text : modelData.source
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    font.family: Theme.fontContent
                                    font.pixelSize: win.readingPx
                                    color: modelData.empty || modelData.passthrough ? inkVariant : ink
                                }

                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.s1
                                    width: 22
                                    height: 22
                                    radius: Theme.rXs
                                    color: copyHover.hovered ? Theme.surfaceHighest : "transparent"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "⧉"
                                        font.pixelSize: 11
                                        color: inkVariant
                                    }
                                    HoverHandler { id: copyHover }
                                    TapHandler {
                                        onTapped: miniSession.copyBlock(modelData.index - 1)
                                    }
                                }
                            }
                        }
                    }
                }

                // 操作行
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s2

                    // 引擎槽：放在左下角，写的是全局 settings/engine/id
                    Rectangle {
                        id: engineButton
                        objectName: "miniEngineButton"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: engineButtonText.implicitWidth + 22
                        radius: Theme.rXs
                        color: (engineHover.hovered || win.engineMenuOpen) ? Theme.surfaceHigh
                                                                          : "transparent"
                        Text {
                            id: engineButtonText
                            anchors.centerIn: parent
                            // 显式读一次 backend.engineId：JS 函数体里的属性读取
                            // 不建依赖，否则主页换引擎这里还显示旧名字。
                            text: {
                                const mainEngine = backend.engineId
                                return win.engineName() + (mainEngine === undefined ? "" : "")
                            }
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.primary
                        }
                        HoverHandler { id: engineHover }
                        TapHandler {
                            onTapped: {
                                win.openMenu = 0
                                win.engineMenuOpen = !win.engineMenuOpen
                            }
                        }
                    }

                    Text {
                        visible: win.saveHint.length > 0
                        text: win.saveHint
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }
                    Text {
                        visible: win.saveHint.length === 0 && miniSession.phase === "Done"
                                 && miniSession.elapsedMs > 0
                        text: qsTr("%1 ms").arg(miniSession.elapsedMs)
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }
                    Text {
                        visible: miniSession.phase === "Done"
                        text: qsTr("· %1 段").arg(miniSession.paragraphCount)
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }

                    Rectangle {
                        visible: miniSession.phase === "Done"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: allCopyText.implicitWidth + 16
                        radius: Theme.rXs
                        color: allCopyHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: allCopyText
                            anchors.centerIn: parent
                            text: qsTr("全部复制")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.primary
                        }
                        HoverHandler { id: allCopyHover }
                        TapHandler { onTapped: miniSession.copyAll() }
                    }

                    Item { Layout.fillWidth: true }

                    // 划词获取 / 清空
                    Rectangle {
                        Layout.preferredHeight: 30
                        Layout.preferredWidth: grabText.implicitWidth + 20
                        radius: Theme.rXs
                        color: grabHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: grabText
                            anchors.centerIn: parent
                            text: qsTr("划词获取")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.primary
                        }
                        HoverHandler { id: grabHover }
                        TapHandler {
                            onTapped: {
                                if (!miniSession.grabClipboard())
                                    console.info("[mini] clipboard is empty")
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredHeight: 30
                        Layout.preferredWidth: clearText.implicitWidth + 20
                        radius: Theme.rXs
                        color: clearHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: clearText
                            anchors.centerIn: parent
                            text: qsTr("清空")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: inkVariant
                        }
                        HoverHandler { id: clearHover }
                        TapHandler {
                            onTapped: {
                                miniSession.clearAll()
                                input.forceActiveFocus()
                            }
                        }
                    }

                    // 翻译 / 取消 / 保存
                    Rectangle {
                        Layout.preferredHeight: 32
                        Layout.preferredWidth: mainText.implicitWidth + 24
                        radius: Theme.rSm
                        color: mainHover.hovered
                               ? (Theme.dark ? Qt.lighter(Theme.primary, 1.12)
                                             : Qt.darker(Theme.primary, 1.08))
                               : Theme.primary

                        Text {
                            id: mainText
                            anchors.centerIn: parent
                            text: miniSession.phase === "Translating" ? qsTr("取消")
                                  : (miniSession.phase === "Done" ? qsTr("保存到记录") : qsTr("翻译"))
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Font.DemiBold
                            color: Theme.primaryText
                        }
                        HoverHandler { id: mainHover }
                        TapHandler {
                            onTapped: {
                                if (miniSession.phase === "Translating")
                                    miniSession.cancel()
                                else if (miniSession.phase === "Done")
                                    miniSession.saveRecord()
                                else
                                    miniSession.translate()
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------- 记录页
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.s4
                spacing: Theme.s2
                visible: win.tab === "records"

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("最近 %1 条").arg(win.records.length)
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: inkVariant
                    }
                    Rectangle {
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: refreshText.implicitWidth + 16
                        radius: Theme.rXs
                        color: refreshHover.hovered ? Theme.surfaceHigh : "transparent"
                        Text {
                            id: refreshText
                            anchors.centerIn: parent
                            text: qsTr("刷新")
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.primary
                        }
                        HoverHandler { id: refreshHover }
                        TapHandler { onTapped: win.refreshRecords() }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.preferredWidth: 92
                        text: qsTr("时间")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }
                    Text {
                        Layout.preferredWidth: 72
                        text: qsTr("来源")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("摘要")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: inkVariant
                    }
                }

                ListView {
                    id: recordList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 0
                    model: win.records
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Item {
                        required property var modelData
                        required property int index
                        width: recordList.width
                        height: 34

                        Rectangle {
                            anchors.fill: parent
                            color: recordHover.hovered || win.recordIndex === index
                                   ? Theme.surfaceHigh : "transparent"
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 1
                                color: line
                                opacity: 0.5
                            }
                        }
                        HoverHandler { id: recordHover }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 92
                            text: win.recordTime(modelData.createdAt)
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeTiny
                            color: inkVariant
                            elide: Text.ElideRight
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 92
                            width: 72
                            text: win.sourceLabel(modelData.kind)
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: inkVariant
                            elide: Text.ElideRight
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 164
                            width: parent.width - 164
                            text: win.recordPreview(modelData)
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: ink
                            elide: Text.ElideRight
                        }
                        TapHandler {
                            onTapped: win.selectRecord(index)
                        }
                    }
                }

                // 详情
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 132 : 0
                    visible: win.recordIndex >= 0 && win.recordIndex < win.records.length
                    radius: Theme.rXs
                    color: Theme.surfaceLow
                    border.width: 1
                    border.color: line
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.s2
                        spacing: Theme.s1

                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("%1 · %2")
                                      .arg(win.sourceLabel(win.records[win.recordIndex]
                                                           ? win.records[win.recordIndex].kind : ""))
                                      .arg(win.recordTime(win.records[win.recordIndex]
                                                          ? win.records[win.recordIndex].createdAt : ""))
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: inkVariant
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: qsTr("关闭")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.primary
                                TapHandler { onTapped: win.selectRecord(-1) }
                            }
                        }
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            TextEdit {
                                id: recordDetail
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                // 单向绑定到窗口属性，谁来改都只有一处真值。
                                text: win.recordDetailText
                                font.family: Theme.fontContent
                                font.pixelSize: Theme.fontSizeSmall
                                color: ink
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true; visible: win.records.length === 0 }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    visible: win.records.length === 0
                    text: backend.historyEnabled
                          ? qsTr("还没有记录。翻译完成会自动存进 mini.db。")
                          : qsTr("历史对话是关着的。去「设置 · 缓存」里打开「保存翻译历史」。")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: inkVariant
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
    }

    // ------------------------------------------------------------ 下拉菜单（窗内）
    Rectangle {
        id: langMenu
        visible: win.openMenu !== 0
        z: 60
        x: win.openMenu === 1 ? srcField.x : dstField.x
        y: srcField.y + srcField.height + 4
             + (win.openMenu === 1 ? 0 : 0)
        width: 240
        height: Math.min(280, menuColumn.implicitHeight + Theme.s2 * 2)
        radius: Theme.rSm
        color: Theme.surfaceHigh
        border.width: 1
        border.color: line

        Flickable {
            anchors.fill: parent
            anchors.margins: Theme.s1
            clip: true
            contentHeight: menuColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: menuColumn
                width: parent.width

                Repeater {
                    model: backend.languages
                    delegate: Item {
                        required property var modelData
                        width: menuColumn.width
                        height: 28
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.rXs
                            color: itemHover.hovered ? Theme.surfaceHighest : "transparent"
                        }
                        HoverHandler { id: itemHover }

                        // 目标语言多选时左面带勾选方块
                        Rectangle {
                            visible: win.openMenu === 2
                            x: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14
                            height: 14
                            radius: 3
                            color: (miniSession.dstLangs || []).indexOf(modelData.code) >= 0
                                   ? Theme.primary : "transparent"
                            border.width: 1
                            border.color: (miniSession.dstLangs || []).indexOf(modelData.code) >= 0
                                          ? Theme.primary : line
                            Text {
                                anchors.centerIn: parent
                                visible: (miniSession.dstLangs || []).indexOf(modelData.code) >= 0
                                text: "✓"
                                font.pixelSize: 9
                                color: Theme.primaryText
                            }
                        }

                        Text {
                            x: win.openMenu === 2 ? Theme.s2 + 22 : Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: win.openMenu === 1 && miniSession.srcLang === modelData.code
                                   ? Theme.primary : ink
                        }

                        TapHandler {
                            onTapped: {
                                if (win.openMenu === 2) {
                                    win.toggleDst(modelData.code)
                                } else {
                                    miniSession.srcLang = modelData.code
                                    win.openMenu = 0
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ 引擎下拉（窗内）
    // 菜单往上弹：按钮在左下角，往下画会出窗口。
    Rectangle {
        id: engineMenu
        objectName: "miniEngineMenu"
        visible: win.engineMenuOpen
        z: 62
        // 不靠 mapToItem 定位：按钮在第一帧还贴在行首、card 也可能是 0x0，
        // 算出来的锚点会错位，且布局后来动了绑定也不会重算。
        // 引擎槽固定在底栏最左（左边距 = s4），所以直接用窗口高度倒推：
        // 底栏高 26 + 下边距 s4，菜单压在它上面 4px。
        x: Theme.s4
        y: Math.max(Theme.s2, card.height - height - 26 - Theme.s4 - 4)
        width: 168
        height: engineColumn.implicitHeight + Theme.s2 * 2
        radius: Theme.rSm
        color: Theme.surfaceHigh
        border.width: 1
        border.color: line

        Column {
            id: engineColumn
            anchors.fill: parent
            anchors.margins: Theme.s2

            Repeater {
                model: win.engineChoices()
                delegate: Item {
                    id: engineRow
                    required property var modelData
                    width: engineColumn.width
                    height: 26

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.rXs
                        color: engineRowHover.hovered ? Theme.surfaceHighest : "transparent"
                    }
                    HoverHandler { id: engineRowHover }

                    Rectangle {
                        width: 6
                        height: 6
                        radius: 3
                        x: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: win.engineDotColor(engineRow.modelData.state)
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        text: engineRow.modelData.label
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: backend.engineId === engineRow.modelData.id
                                     ? Font.DemiBold : Font.Normal
                        color: backend.engineId === engineRow.modelData.id
                               ? Theme.primary : Theme.ink
                    }
                    TapHandler {
                        onTapped: win.selectEngine(engineRow.modelData.id)
                    }
                }
            }
        }
    }

    // 点空白处收菜单
    TapHandler {
        enabled: win.openMenu !== 0 || win.engineMenuOpen
        onTapped: {
            win.openMenu = 0
            win.engineMenuOpen = false
        }
    }

    // 设置小窗：复用截图翻译那块面板（mode = "mini"）
    CaptureSettingsPanel {
        id: miniSettings
        objectName: "miniSettingsPanel"
        mode: "mini"
        visible: false
        onClosed: visible = false
        // 「更多设置」只收面板、不关小窗：用户是去看完整设置，回来还要接着翻，
        // 把小窗一起关掉等于把这一单的上下文扔了。真正的跳页由 Main.qml 接这个信号。
        onMoreSettingsRequested: {
            visible = false
            win.moreSettingsRequested()
        }
        // 位置不在这里算：Component.onCompleted 时窗口可能还没摆位（x/y 都是 0），
        // 算出来一次就永远钉在屏幕左上角的错地方。改成打开时按当前坐标现算，
        // 见顶栏齿轮的 onTapped。
    }
}
