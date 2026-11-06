import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Controls
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把输入控件做成模板里的 .input / .field-like。
import QtQuick.Controls.Basic
import Studious

// 配置页：按 docs/TempUI/index.html 的 config 段复刻。
//
// 结构（对应模板）：
//   页头            h1 + 副标题
//   引擎            模式切换（自动 / 强制）+ 一份四项列表：自动带序号且可拖排序，
//                   强制带单选圈（写 backend.engineId，与翻译页同一个属性）
//   Web 来源        单选的 provider（"" = 自动）
//   云端 LLM        backend.llmConfig 的全部键
//   本地 MT         语言对 → 实际路径
//   本地 LLM        llama-server 的模型文件 / GPU / 启停
//
// 为什么引擎状态不再单独占一张卡：状态与引擎是一一对应的，拆成两处用户得来回对照；
// 直接做成行内副标签，点行就能跳到下面那张卡的对应配置。
Item {
    id: page

    // ------------------------------------------------------------ 本地数据模型

    // 引擎的 canonical id。页面内部只认这四个，后端两套 id 的差异在边界上换算，
    // 免得 "llm" / "llm-cloud" 到处漏。mock 是开发占位，永远不进界面。
    readonly property var engineKeys: ["web", "llm", "llm-local", "local-mt"]

    // 自动引擎顺序：canonical id 数组，镜像 backend.enginePriority。
    // 拖动过程中只改这个数组（其余行实时让位），松手才写回后端 ——
    // 后端每写一次都要落 QSettings 并重探全部引擎，逐帧写会变成探测风暴。
    property var autoOrder: []

    // 拖动的临时状态：dragIndex 是抓起时的下标，dragTarget 是按手指位置算出的落点，
    // dragOffset 让被拖的那行跟手。
    property int dragIndex: -1
    property int dragTarget: -1
    property real dragOffset: 0

    // 行高固定：拖动换位全靠下标 × 行高算坐标，行高必须可预测，
    // 所以这里不交给布局自适应。
    readonly property int engineRowHeight: 56

    // 引擎卡的模式：false = 自动（按优先级降级），true = 强制某一个。
    // 只是界面状态，真正决定行为的是 backend.engineId / backend.enginePriority。
    property bool forceMode: false

    // 正在拖动（自动模式专用）：其余行要不要让位全靠它。
    readonly property bool autoModeDragging: dragIndex >= 0

    // 云端 LLM 的编辑副本。backend.llmConfig 是整张 map 覆盖写，
    // 改一个键也要先拷一份完整 map 再提交。
    property var llmDraft: ({})

    // 点行跳转后要打开的那张卡的主下拉（空 = 只展开不开下拉）。
    property string pendingFocusKey: ""

    // Web 来源：后端只吃单个 id，"" = 自动（google → mymemory）。
    readonly property var webProviderKeys: ["", "google", "mymemory", "azure"]

    // 云端模型：后端没有「列模型」接口，先给一份占位清单，
    // 当前配置里的模型名永远排在第一项，保证下拉显示的是真实生效值。
    readonly property var cloudModelFallback: ["deepseek-v4.1-flash", "qwen3-32b", "gpt-4.1-mini",
                                               "glm-4.6", "llama-3.3-70b"]
    property var cloudModelOptions: []

    // 思考等级：下拉显示中文，落盘存协议里的英文码。
    readonly property var effortCodes: ["none", "minimal", "low", "medium", "high", ""]

    // 本地 LLM 的模型下拉：没有「列可用模型」接口，只放「自动」+ 当前值两项，
    // 其余选项由 Stub 标记说明还没接。
    property var localModelOptions: []

    // 本地 MT 的可用路径。后端还没暴露资产清单，先留空 —— 走「需下载」空态。
    property var localMtRoutes: []

    // 本地服务端口 / PID：后端暂未暴露，先显示占位。
    readonly property string localPort: "—"
    readonly property string localPid: "—"

    // 「获取模型」「探测」暂时没有后端接口，点了先给一句实话。
    property string modelHint: qsTr("已获取的模型会出现在下拉里；当前为占位项。")
    property string probeHint: qsTr("尚未探测：会真的发一次翻译请求，用返回的 usage 判断思考是否被关掉。")

    // ------------------------------------------------------------ id 换算

    // 后端有三套写法：engineId 的自动是空串、云端是 "llm-cloud"；
    // enginePriority / engineStatuses 里云端是 "llm"。全部折成 canonical。
    function canonicalId(id) {
        if (id === undefined || id === null || id === "" || id === "auto")
            return "auto"
        if (id === "llm" || id === "llm-cloud")
            return "llm"
        return id
    }

    // canonical → backend.engineId 真正认的 code。从 backend.engines 里反查，
    // 后端哪天把 "llm-cloud" 改名，这里不用跟着改。
    function engineCode(key) {
        var list = backend.engines
        for (var i = 0; i < list.length; ++i) {
            if (canonicalId(list[i].code) === key)
                return list[i].code
        }
        return key
    }

    // 引擎 canonical id → 显示名。
    // 不用 backend.engines 的 label：那套文案在后端 i18n 里，跟界面语言的切换时机不同步。
    function engineDisplayName(key) {
        if (key === "web")
            return qsTr("Web")
        if (key === "llm")
            return qsTr("云端 LLM")
        if (key === "llm-local")
            return qsTr("本地 LLM")
        if (key === "local-mt")
            return qsTr("本地 MT")
        return key
    }

    // Web 来源 id → 显示名（"" = 自动）。
    function providerName(id) {
        if (id === "")
            return qsTr("自动（google → mymemory）")
        if (id === "google")
            return qsTr("Google 在线翻译")
        if (id === "mymemory")
            return qsTr("MyMemory 记忆库")
        if (id === "azure")
            return qsTr("Microsoft Azure 翻译")
        return id
    }

    // Web 来源 id → 一行说明。
    function providerNote(id) {
        if (id === "")
            return qsTr("先试 Google，失败再退到 MyMemory；两个都不用 key。")
        if (id === "google")
            return qsTr("无需 key · 免费 · 国内通常需要代理；高频会限流")
        if (id === "mymemory")
            return qsTr("无需 key · 源语言未知时按 en 处理 · 额度有限")
        if (id === "azure")
            return qsTr("需要 key · v1 仅保留接口，填了也不参与翻译")
        return ""
    }

    // 在 engineStatuses 里查某个引擎的一条状态；查不到返回 null。
    function engineStatus(key) {
        var list = backend.engineStatuses
        for (var i = 0; i < list.length; ++i) {
            if (canonicalId(list[i].id) === key)
                return list[i]
        }
        return null
    }

    // 取不到就当未就绪：宁可让用户多点一次「去配置」，也不要把没探测到的引擎说成能用。
    function engineState(key) {
        var row = engineStatus(key)
        return (row && row.state) ? row.state : "not-ready"
    }


    // 行图标：跟左侧导航用同一批 SVG，按主题重新上色。
    function engineIcon(key) {
        if (key === "web")
            return "qrc:/qt/qml/Studious/resources/icons/globe.svg"
        if (key === "llm")
            return "qrc:/qt/qml/Studious/resources/icons/translate.svg"
        if (key === "llm-local")
            return "qrc:/qt/qml/Studious/resources/icons/settings.svg"
        if (key === "local-mt")
            return "qrc:/qt/qml/Studious/resources/icons/database.svg"
        return ""
    }

    // 副标签：主文案用后端给的 detail（英文，原样显示、不翻译），
    // 前面按状态补一个中文前缀，让人一眼看出是「没填参数」还是「缺资产」。
    function engineSubtitle(key) {
        var row = page.engineStatus(key)
        var detail = (row && row.detail !== undefined && row.detail !== null)
                     ? String(row.detail) : ""
        var state = page.engineState(key)
        if (state === "ready")
            return detail.length > 0 ? detail : qsTr("就绪")
        var prefix = state === "missing-assets" ? qsTr("缺资产") : qsTr("未就绪")
        // 后端给 llm-local 的 detail 本身就是「未就绪」；拼起来会变成「未就绪 · 未就绪」。
        if (detail.length === 0 || detail === prefix)
            return prefix
        return prefix + " · " + detail
    }

    // 自动模式下当前真正会被用到的那个：autoOrder 里第一个就绪的。
    // 一个都没有就返回空 —— 不高亮任何行，别装作有引擎能用。
    readonly property string highlightEngineKey: {
        if (page.forceMode)
            return ""
        for (var i = 0; i < page.autoOrder.length; ++i) {
            if (page.engineState(page.autoOrder[i]) === "ready")
                return page.autoOrder[i]
        }
        return ""
    }

    // 切模式。切回自动要顺手把 engineId 归零（同一个属性，翻译页跟着回自动）；
    // 切到强制则保持现状，没选过任何引擎时四个单选都不亮，由右上角提示说明原因。
    function applyForceMode(force) {
        page.forceMode = force
        var code = force ? "force" : "auto"
        if (engineModeSeg.current !== code)
            engineModeSeg.current = code
    }

    function setEngineMode(force) {
        page.applyForceMode(force)
        if (!force)
            backend.engineId = page.engineCode("auto")
    }

    // 本地服务是否在跑：取 engineStatuses 里的 llm-local 一条。
    readonly property bool localLlmReady: page.engineState("llm-local") === "ready"

    // 手动引擎的选中判断。backend.engineId 可能是自动，这时四项都不选中。
    function engineSelected(key) {
        return canonicalId(backend.engineId) === key
    }

    // ------------------------------------------------------------ 顺序同步

    function syncAutoOrder() {
        var stored = backend.enginePriority || []
        var next = []
        for (var i = 0; i < stored.length; ++i) {
            var key = canonicalId(stored[i])
            if (page.engineKeys.indexOf(key) >= 0 && next.indexOf(key) < 0)
                next.push(key)
        }
        // 后端可能还没落过盘（空）或缺项；补齐，保证自动引擎永远是四项。
        for (var j = 0; j < page.engineKeys.length; ++j) {
            if (next.indexOf(page.engineKeys[j]) < 0)
                next.push(page.engineKeys[j])
        }
        page.autoOrder = next
    }

    function beginDrag(index) {
        page.dragIndex = index
        page.dragTarget = index
        page.dragOffset = 0
        // 拖动期间关掉 Flickable 的惯性拖动，否则竖着拖手柄会把整页一起滚。
        if (scroll.contentItem)
            scroll.contentItem.interactive = false
    }

    function updateDrag(translationY) {
        if (page.dragIndex < 0)
            return
        page.dragOffset = translationY
        var rowH = page.engineRowHeight
        var center = page.dragIndex * rowH + page.dragOffset + rowH / 2
        var slot = Math.floor(center / rowH)
        page.dragTarget = Math.max(0, Math.min(slot, page.autoOrder.length - 1))
    }

    function commitDrag() {
        var from = page.dragIndex
        var to = page.dragTarget
        page.dragIndex = -1
        page.dragTarget = -1
        page.dragOffset = 0
        if (scroll.contentItem)
            scroll.contentItem.interactive = true

        if (from < 0 || to < 0 || from === to)
            return

        var next = page.autoOrder.slice()
        var moved = next.splice(from, 1)[0]
        next.splice(to, 0, moved)

        // 延到下一拍再落地：这里正在 delegate 自己的信号处理里跑，
        // 立刻换 model 会把当前 delegate 连同 DragHandler 一起销毁。
        Qt.callLater(function () {
            page.autoOrder = next
            // 写回即落盘，后端会顺手重算「自动」的解析结果。
            backend.enginePriority = next
        })
    }

    // ------------------------------------------------------------ 云端 LLM 读写

    // 后端整张 map 覆盖写，所以每次都要提交完整对象，不能只给一个键。
    function patchLlm(key, value) {
        var next = Object.assign({}, page.llmDraft)
        next[key] = value
        page.llmDraft = next
        backend.llmConfig = next
    }

    function llmString(key) {
        var v = page.llmDraft ? page.llmDraft[key] : ""
        return (v === undefined || v === null) ? "" : String(v)
    }

    function llmNumber(key, fallback) {
        var n = Number(page.llmDraft ? page.llmDraft[key] : NaN)
        return isNaN(n) ? fallback : n
    }

    function llmBool(key) {
        return page.llmDraft && page.llmDraft[key] === true
    }

    function rebuildCloudModelOptions() {
        var opts = []
        var current = page.llmString("model")
        if (current.length > 0)
            opts.push(current)
        for (var i = 0; i < page.cloudModelFallback.length; ++i) {
            if (opts.indexOf(page.cloudModelFallback[i]) < 0)
                opts.push(page.cloudModelFallback[i])
        }
        page.cloudModelOptions = opts
    }

    function cloudModelIndex() {
        var idx = page.cloudModelOptions.indexOf(page.llmString("model"))
        return idx < 0 ? 0 : idx
    }

    function effortIndex(code) {
        var idx = page.effortCodes.indexOf(code)
        return idx < 0 ? 0 : idx
    }

    // 下拉的 currentIndex 一旦被用户点过就不再跟着绑定走，
    // 所以外部（后端）变了之后要手动对一次。
    function syncLlmControls() {
        var modelIdx = page.cloudModelIndex()
        if (cloudModelCombo.currentIndex !== modelIdx)
            cloudModelCombo.currentIndex = modelIdx

        var eIdx = page.effortIndex(page.llmString("reasoningEffort"))
        if (effortCombo.currentIndex !== eIdx)
            effortCombo.currentIndex = eIdx
    }

    // ------------------------------------------------------------ 本地 LLM 读写

    function refreshLocalModelOptions() {
        var opts = [qsTr("自动（第一个可用）")]
        if (backend.localModel.length > 0)
            opts.push(backend.localModel)
        page.localModelOptions = opts
    }

    function syncLocalModelCombo() {
        page.refreshLocalModelOptions()
        var idx = backend.localModel.length > 0 ? 1 : 0
        if (localModelCombo.currentIndex !== idx)
            localModelCombo.currentIndex = idx
    }

    // ------------------------------------------------------------ 跳转

    function cardForEngine(key) {
        if (key === "web")
            return webCard
        if (key === "llm")
            return cloudCard
        if (key === "local-mt")
            return mtCard
        if (key === "llm-local")
            return localCard
        return null
    }

    function scrollToCard(card, animated) {
        var flick = scroll.contentItem
        if (!flick || !card)
            return
        var maxY = Math.max(0, flick.contentHeight - flick.height)
        var target = Math.max(0, Math.min(card.y - Theme.s4, maxY))
        scrollAnim.stop()
        if (animated === false) {
            flick.contentY = target
            return
        }
        scrollAnim.from = flick.contentY
        scrollAnim.to = target
        scrollAnim.start()
    }

    // guard = true 表示「点行」：没配好的引擎先拦住，别把人丢进一张填了一半的表。
    // guard = false 表示「点状态徽标」或「去配置」：拦过一次了，直接跳。
    function openEngineConfig(key, guard) {
        var card = page.cardForEngine(key)
        if (!card)
            return
        if (guard && page.engineState(key) !== "ready") {
            notReadyDialog.openFor(key)
            return
        }
        card.expanded = true
        page.pendingFocusKey = key
        page.scrollToCard(card, true)
        // 卡片高度有 250ms 动画，内容高度会变；等布局落定再补正一次并开下拉。
        focusTimer.restart()
    }

    Component.onCompleted: {
        page.syncAutoOrder()
        // 上次落盘的是「强制某个引擎」就按强制打开，否则按自动。
        page.applyForceMode(page.canonicalId(backend.engineId) !== "auto")
        page.llmDraft = backend.llmConfig
        page.rebuildCloudModelOptions()
        page.refreshLocalModelOptions()
        backend.refreshEngineStatuses()
    }

    Connections {
        target: backend

        function onEnginePriorityChanged() {
            page.syncAutoOrder()
        }

        // 翻译页改了引擎选择，这里要跟着换模式，否则界面会撒谎。
        function onEngineIdChanged() {
            page.applyForceMode(page.canonicalId(backend.engineId) !== "auto")
        }

        function onLlmConfigChanged() {
            page.llmDraft = backend.llmConfig
            page.rebuildCloudModelOptions()
            page.syncLlmControls()
        }

        function onLocalModelChanged() {
            page.syncLocalModelCombo()
        }
    }

    // 跳转后的收尾：定位到最终位置，需要的话把主下拉拉下来。
    Timer {
        id: focusTimer
        interval: Theme.durBase + 80

        onTriggered: {
            var key = page.pendingFocusKey
            page.pendingFocusKey = ""
            if (key === "")
                return
            page.scrollToCard(page.cardForEngine(key), false)
            // ComboBox 自己没有 open()，得直接开自定义的 popup。
            if (key === "llm")
                cloudModelCombo.popup.open()
            else if (key === "llm-local")
                localModelCombo.popup.open()
        }
    }

    NumberAnimation {
        id: scrollAnim
        target: scroll.contentItem
        property: "contentY"
        duration: Theme.durBase
        easing.type: Easing.OutCubic
    }

    // -------------------------------------------------------- 内联样式组件

    // 输入框：模板里的 .input。
    component ThemedField: TextField {
        id: field

        Layout.fillWidth: true
        implicitHeight: 38

        color: Theme.ink
        placeholderTextColor: Theme.inkVariant
        selectionColor: Theme.primaryContainer
        selectedTextColor: Theme.primaryContainerText
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: Theme.s3

        background: Rectangle {
            radius: Theme.rMd
            color: field.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
            border.width: field.activeFocus ? 1 : 0
            border.color: Theme.primary

            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
    }

    // 下拉：模板里的 .field-like + .menu。
    component ThemedCombo: ComboBox {
        id: combo

        Layout.fillWidth: true
        implicitHeight: 38

        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: Theme.s3
        topPadding: 0
        bottomPadding: 0

        background: Rectangle {
            radius: Theme.rMd
            color: combo.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
            border.width: 0

            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        contentItem: Text {
            rightPadding: combo.indicator.width + Theme.s2
            text: combo.displayText
            font: combo.font
            color: Theme.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        indicator: Text {
            x: combo.width - width - combo.rightPadding
            y: combo.topPadding + (combo.availableHeight - height) / 2
            text: "\u25BE"
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            color: Theme.inkVariant
        }

        delegate: ItemDelegate {
            id: comboItem
            width: combo.width - Theme.s3
            height: 40

            required property int index
            required property var modelData

            text: comboItem.modelData !== undefined ? String(comboItem.modelData) : ""
            highlighted: combo.highlightedIndex === index

            contentItem: Text {
                text: comboItem.text
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.ink
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }

            background: Rectangle {
                radius: Theme.rSm
                color: comboItem.highlighted ? Theme.surfaceHighest : "transparent"
            }
        }

        popup: Popup {
            y: combo.height + Theme.s1
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + Theme.s2, 320)
            padding: Theme.s1

            background: Rectangle {
                radius: Theme.rMd
                color: Theme.surfaceHigh
                border.width: 1
                border.color: Theme.outlineVariant
            }

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                boundsBehavior: Flickable.StopAtBounds

                ScrollIndicator.vertical: ScrollIndicator { }
            }
        }
    }

    // 数字框：Basic 样式默认把 +/- 画成两块整高色块，这里换成右侧两个细按钮。
    component ThemedSpin: SpinBox {
        id: spin

        readonly property int stepWidth: 26

        Layout.fillWidth: true
        implicitHeight: 38
        editable: true

        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: spin.stepWidth * 2 + Theme.s1

        background: Rectangle {
            radius: Theme.rMd
            color: Theme.surfaceHighest
            border.width: spin.activeFocus ? 1 : 0
            border.color: Theme.primary
        }

        contentItem: TextInput {
            text: spin.displayText
            font: spin.font
            color: Theme.ink
            selectionColor: Theme.primaryContainer
            selectedTextColor: Theme.primaryContainerText
            horizontalAlignment: Qt.AlignLeft
            verticalAlignment: Text.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: spin.inputMethodHints
        }

        up.indicator: Rectangle {
            x: spin.width - width
            height: spin.height
            implicitWidth: spin.stepWidth
            radius: Theme.rSm
            color: spin.up.hovered ? Theme.surfaceHigh : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\uFF0B"
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: spin.up.pressed ? Theme.ink : Theme.inkVariant
            }
        }

        down.indicator: Rectangle {
            x: spin.width - spin.stepWidth * 2
            height: spin.height
            implicitWidth: spin.stepWidth
            radius: Theme.rSm
            color: spin.down.hovered ? Theme.surfaceHigh : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\uFF0D"
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: spin.down.pressed ? Theme.ink : Theme.inkVariant
            }
        }
    }

    // 页内折叠卡片：外壳样式与 SectionCard 完全一致，只是内容可收起。
    // 头部一行（标题 + hint + 可选右侧操作 + ▾/▴），整行可点；收起后只留头部高度。
    component CollapsibleCard: Rectangle {
        id: acc

        property string title: ""
        property string hint: ""
        property bool expanded: true
        default property alias content: body.data
        property alias headerActions: actions.data
        signal toggled(bool expanded)

        implicitHeight: column.implicitHeight + Theme.s5 * 2
        radius: Theme.rLg
        color: Theme.surfaceLow
        border.width: 1
        border.color: Theme.outlineVariant
        // 收起动画期间内容会先溢出，靠 clip 裁掉。
        clip: true

        Behavior on height {
            NumberAnimation { duration: Theme.durBase; easing.type: Easing.OutCubic }
        }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.s5
            spacing: Theme.s3

            // 头部整行：点哪里都切换折叠。MouseArea 在下层，
            // 头部里的按钮在上层，所以「刷新」这类操作不会被折叠抢走。
            Item {
                Layout.fillWidth: true
                implicitHeight: headRow.implicitHeight

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        acc.expanded = !acc.expanded
                        acc.toggled(acc.expanded)
                    }
                }

                RowLayout {
                    id: headRow
                    anchors.fill: parent
                    spacing: Theme.s2

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: acc.title
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSection
                        font.weight: Font.DemiBold
                        color: Theme.ink
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        visible: text.length > 0
                        text: acc.hint
                        elide: Text.ElideRight
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }

                    Row {
                        id: actions
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Theme.s2
                    }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: acc.expanded ? "\u25B4" : "\u25BE"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.inkVariant
                    }
                }
            }

            // 内容区：收起时整块隐藏，ColumnLayout 连带把它的间距一起让出来。
            ColumnLayout {
                id: body
                Layout.fillWidth: true
                visible: acc.expanded
                spacing: Theme.s3
            }
        }
    }

    // ---------------------------------------------------------------- 页面
    ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            width: parent.width
            spacing: Theme.s4

            // -------------------------------------------------------- 页头
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                Layout.topMargin: Theme.s5
                spacing: Theme.s2

                Text {
                    text: qsTr("配置")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Font.Bold
                    color: Theme.ink
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("翻译链路怎么接通：选引擎、填凭据、装模型。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.inkVariant
                }
            }

            // -------------------------------------------------------- 引擎（整行 · 默认展开）
            CollapsibleCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: true
                title: qsTr("引擎")
                hint: qsTr("自动按优先级降级，强制只用一个；点行跳到下面的对应配置。")

                // 模式 + 右上角说明。说明跟着模式走，别让人猜现在是哪一种。
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    Segmented {
                        id: engineModeSeg
                        items: [
                            { code: "auto", label: qsTr("自动选择") },
                            { code: "force", label: qsTr("强制引擎") }
                        ]
                        current: page.forceMode ? "force" : "auto"
                        onSelected: function (code) { page.setEngineMode(code === "force") }
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: page.forceMode
                              ? (page.canonicalId(backend.engineId) === "auto"
                                 ? qsTr("当前：自动")
                                 : qsTr("当前：%1").arg(page.engineDisplayName(
                                                           page.canonicalId(backend.engineId))))
                              : qsTr("自动 = 优先级，前一个不可用才轮下一个")
                        elide: Text.ElideRight
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                // 一份列表，形态跟着模式变：自动带序号且可拖，强制带单选圈且固定顺序。
                Item {
                    id: engineList
                    Layout.fillWidth: true
                    Layout.preferredHeight: engineRepeater.count * page.engineRowHeight

                    Repeater {
                        id: engineRepeater
                        model: page.forceMode ? page.engineKeys : page.autoOrder

                        delegate: Rectangle {
                            id: engineRow
                            required property int index
                            required property string modelData

                            readonly property bool autoMode: !page.forceMode
                            // 自动模式高亮「实际会被用到」的那个，强制模式高亮选中的那个。
                            readonly property bool highlighted: autoMode
                                                                ? modelData === page.highlightEngineKey
                                                                : page.engineSelected(modelData)
                            readonly property bool dragging: autoMode && page.dragIndex === index
                            // 被拖那行让出的空隙由其余行实时补位。
                            readonly property int shift: {
                                if (!page.autoModeDragging || dragging)
                                    return 0
                                if (page.dragIndex < page.dragTarget
                                        && index > page.dragIndex && index <= page.dragTarget)
                                    return -page.engineRowHeight
                                if (page.dragIndex > page.dragTarget
                                        && index >= page.dragTarget && index < page.dragIndex)
                                    return page.engineRowHeight
                                return 0
                            }

                            width: engineList.width
                            height: page.engineRowHeight - Theme.s2
                            y: index * page.engineRowHeight + (dragging ? page.dragOffset : shift)
                            z: dragging ? 2 : 0
                            radius: Theme.rMd
                            color: highlighted ? Theme.primaryContainer : Theme.surfaceLow
                            border.width: 1
                            border.color: highlighted || dragging ? Theme.primary
                                                                  : Theme.outlineVariant

                            Behavior on y {
                                enabled: !engineRow.dragging
                                NumberAnimation {
                                    duration: Theme.durFast
                                    easing.type: Easing.OutCubic
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                // 手柄那一段让给 DragHandler，不然一按手柄就跳走。
                                anchors.leftMargin: engineRow.autoMode ? 46 : 0
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    // 强制模式点行先选中；自动模式没有选中的概念。
                                    if (!engineRow.autoMode)
                                        backend.engineId = page.engineCode(engineRow.modelData)
                                    page.openEngineConfig(engineRow.modelData, true)
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.s2
                                anchors.rightMargin: Theme.s3
                                spacing: Theme.s2

                                // 拖动手柄：只有自动模式有，且只占左侧一小条。
                                Item {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: engineRow.autoMode
                                    implicitWidth: 20
                                    implicitHeight: 24

                                    Text {
                                        anchors.centerIn: parent
                                        text: "\u2807"
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        color: engineRow.dragging
                                               ? Theme.primary
                                               : (engineRow.highlighted ? Theme.primaryContainerText
                                                                        : Theme.inkVariant)
                                    }

                                    DragHandler {
                                        id: engineDrag
                                        target: null
                                        xAxis.enabled: false
                                        cursorShape: Qt.ClosedHandCursor

                                        onActiveChanged: {
                                            if (engineDrag.active)
                                                page.beginDrag(engineRow.index)
                                            else
                                                page.commitDrag()
                                        }

                                        onActiveTranslationChanged: {
                                            if (engineDrag.active)
                                                page.updateDrag(engineDrag.activeTranslation.y)
                                        }
                                    }
                                }

                                // 序号（自动）/ 单选圈（强制）二选一。
                                Text {
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.preferredWidth: 16
                                    visible: engineRow.autoMode
                                    text: engineRow.index + 1
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: Font.DemiBold
                                    color: engineRow.highlighted ? Theme.primaryContainerText
                                                                 : Theme.inkVariant
                                }

                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: !engineRow.autoMode
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    radius: Theme.rFull
                                    color: "transparent"
                                    border.width: 1
                                    border.color: engineRow.highlighted ? Theme.primary : Theme.outline

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 10
                                        height: 10
                                        radius: Theme.rFull
                                        visible: engineRow.highlighted
                                        color: Theme.primary
                                    }
                                }

                                Image {
                                    Layout.alignment: Qt.AlignVCenter
                                    source: page.engineIcon(engineRow.modelData)
                                    sourceSize.width: 18
                                    sourceSize.height: 18
                                    // SVG 描边是写死的颜色，跟左栏一样用 MultiEffect 按主题重上色。
                                    layer.enabled: true
                                    layer.effect: MultiEffect {
                                        colorizationColor: engineRow.highlighted
                                                           ? Theme.railIconActive
                                                           : Theme.railIconIdle
                                        colorization: 1.0
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    spacing: 0

                                    Text {
                                        Layout.fillWidth: true
                                        text: page.engineDisplayName(engineRow.modelData)
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        font.weight: Font.DemiBold
                                        color: engineRow.highlighted ? Theme.primaryContainerText
                                                                     : Theme.ink
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: page.engineSubtitle(engineRow.modelData)
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeTiny
                                        color: engineRow.highlighted ? Theme.primaryContainerText
                                                                     : Theme.inkVariant
                                        opacity: 0.85
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("换引擎 / 换模型 / 换 Base URL 后，缓存键（含引擎标签）互不命中，同一句原文译文可能完全不同。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }
            }

            // ---------------------------------------------------- Web 来源（整行 · 默认收起）
            CollapsibleCard {
                id: webCard
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: false
                title: qsTr("Web 来源")
                hint: qsTr("单选；「自动」按 google → mymemory 依次尝试，固定某个来源后失败就直接报错，不会偷偷换源。")

                Repeater {
                    model: page.webProviderKeys

                    delegate: ColumnLayout {
                        id: providerRow
                        required property int index
                        required property string modelData

                        readonly property bool selected: backend.webProviderId === modelData

                        Layout.fillWidth: true
                        spacing: Theme.s1

                        Item {
                            Layout.fillWidth: true
                            implicitHeight: 34

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.webProviderId = providerRow.modelData
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.s1
                                anchors.rightMargin: Theme.s1
                                spacing: Theme.s2

                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    radius: Theme.rFull
                                    color: "transparent"
                                    border.width: 1
                                    border.color: providerRow.selected ? Theme.primary : Theme.outline

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 10
                                        height: 10
                                        radius: Theme.rFull
                                        visible: providerRow.selected
                                        color: Theme.primary
                                    }
                                }

                                Text {
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.fillWidth: true
                                    text: page.providerName(providerRow.modelData)
                                    elide: Text.ElideRight
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                    color: Theme.ink
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 26
                            text: page.providerNote(providerRow.modelData)
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }

                        // Azure 凭据：只有选中 azure 才展开。
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 26
                            visible: providerRow.modelData === "azure" && providerRow.selected
                            spacing: Theme.s2

                            FieldRow {
                                label: "Key"
                                Stub { }
                                ThemedField {
                                    font.family: Theme.fontMono
                                    placeholderText: qsTr("填入 Key")
                                    echoMode: TextInput.Password
                                }
                            }

                            FieldRow {
                                label: "Region"
                                Stub { }
                                ThemedField {
                                    font.family: Theme.fontMono
                                    placeholderText: qsTr("例如 eastasia")
                                }
                            }
                        }
                    }
                }
            }

            // --------------------------------------------------- 云端 LLM（整行 · 默认展开）
            CollapsibleCard {
                id: cloudCard
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: true
                title: qsTr("云端 LLM")
                hint: qsTr("OpenAI 兼容接口；没填 API Key 时这条链路自动跳过。")

                FieldRow {
                    label: "Base URL"
                    ThemedField {
                        font.family: Theme.fontMono
                        placeholderText: "http://127.0.0.1:3065/v1"
                        text: page.llmString("baseUrl")
                        // 键控提交而不是逐字提交：每个键都会落一次 QSettings + 重探全部引擎。
                        onEditingFinished: page.patchLlm("baseUrl", text)
                    }
                }

                FieldRow {
                    label: "API Key"
                    ThemedField {
                        font.family: Theme.fontMono
                        placeholderText: "sk-…"
                        echoMode: TextInput.Password
                        text: page.llmString("apiKey")
                        onEditingFinished: page.patchLlm("apiKey", text)
                    }
                }

                FieldRow {
                    label: qsTr("模型")
                    ThemedCombo {
                        id: cloudModelCombo
                        model: page.cloudModelOptions
                        currentIndex: page.cloudModelIndex()
                        onActivated: function (index) {
                            if (index >= 0 && index < page.cloudModelOptions.length)
                                page.patchLlm("model", page.cloudModelOptions[index])
                        }
                    }
                    Stub { }
                    AppButton {
                        text: qsTr("获取模型")
                        onClicked: page.modelHint = qsTr("后端还没有「获取模型」接口，先按占位列表选。")
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 132 + Theme.s3
                    text: page.modelHint
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }

                FieldRow {
                    label: "temperature"
                    ThemedSpin {
                        from: 0
                        to: 150
                        stepSize: 5
                        value: Math.round(page.llmNumber("temperature", 0.2) * 100)
                        onValueModified: page.patchLlm("temperature", value / 100)

                        textFromValue: function (v) { return (v / 100).toFixed(2) }
                        valueFromText: function (t) {
                            var n = parseFloat(t)
                            return isNaN(n) ? 0 : Math.round(n * 100)
                        }
                    }
                }

                FieldRow {
                    label: "maxTokens"
                    ThemedSpin {
                        from: 1
                        to: 32768
                        stepSize: 1
                        value: page.llmNumber("maxTokens", 2048)
                        onValueModified: page.patchLlm("maxTokens", value)
                    }
                }

                FieldRow {
                    label: "timeoutMs"
                    // 上限跟 AppSettings 的钳位一致，否则存下去会被悄悄截断。
                    ThemedSpin {
                        from: 1000
                        to: 180000
                        stepSize: 1000
                        value: page.llmNumber("timeoutMs", 60000)
                        onValueModified: page.patchLlm("timeoutMs", value)
                    }
                }

                FieldRow {
                    label: "systemPrompt"
                    TextArea {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 72
                        placeholderText: qsTr("留空使用内置提示词")
                        wrapMode: TextArea.Wrap
                        selectByMouse: true
                        color: Theme.ink
                        placeholderTextColor: Theme.inkVariant
                        selectionColor: Theme.primaryContainer
                        selectedTextColor: Theme.primaryContainerText
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        leftPadding: Theme.s3
                        rightPadding: Theme.s3
                        topPadding: Theme.s2
                        bottomPadding: Theme.s2
                        text: page.llmString("systemPrompt")

                        // TextArea 没有 editingFinished，失焦时才提交（同样是为了别逐字落盘）。
                        onActiveFocusChanged: {
                            if (!activeFocus)
                                page.patchLlm("systemPrompt", text)
                        }

                        background: Rectangle {
                            radius: Theme.rMd
                            color: Theme.surfaceHighest
                            border.width: parent.activeFocus ? 1 : 0
                            border.color: Theme.primary
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 132 + Theme.s3
                    text: qsTr("留空使用内置提示词。")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }

                FieldRow {
                    label: qsTr("思考等级")
                    ThemedCombo {
                        id: effortCombo
                        model: [qsTr("关闭"), qsTr("极低"), qsTr("低"), qsTr("中"), qsTr("高"),
                                qsTr("跟随默认")]
                        currentIndex: page.effortIndex(page.llmString("reasoningEffort"))
                        onActivated: function (index) {
                            if (index >= 0 && index < page.effortCodes.length)
                                page.patchLlm("reasoningEffort", page.effortCodes[index])
                        }
                    }
                    Stub { }
                    AppButton {
                        text: qsTr("探测")
                        onClicked: page.probeHint = qsTr("后端还没有探测接口；接上后会真的发一次翻译请求。")
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 132 + Theme.s3
                    text: page.probeHint
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }

                FieldRow {
                    label: qsTr("批量合并段数")
                    ThemedSpin {
                        from: 1
                        to: 64
                        stepSize: 1
                        value: page.llmNumber("batchSize", 8)
                        onValueModified: page.patchLlm("batchSize", value)
                    }
                }

                FieldRow {
                    label: qsTr("重试预算")
                    ThemedSpin {
                        from: 1000
                        to: 600000
                        stepSize: 1000
                        value: page.llmNumber("retryBudgetMs", 30000)
                        onValueModified: page.patchLlm("retryBudgetMs", value)
                    }
                }

                FieldRow {
                    label: qsTr("清理模型注记")
                    AppToggle {
                        Layout.alignment: Qt.AlignVCenter
                        checked: page.llmBool("stripModelNotes")
                        onToggled: page.patchLlm("stripModelNotes", checked)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("清掉小模型加的「【中文】」注记，只删整行括注，正文不动。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }
            }

            // ----------------------------------------------------- 本地 MT（整行 · 默认收起）
            CollapsibleCard {
                id: mtCard
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: false
                title: qsTr("本地 MT")
                hint: qsTr("按顺序取第一个可用路径；英文中转要做两次推理，会明显变慢。")
                // 整张卡只有一个表格块，这一个标记同时代表卡片与表格。
                Stub { }

                // 表头
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    Text {
                        Layout.preferredWidth: 180
                        text: qsTr("语言对")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                        color: Theme.inkVariant
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("实际路径")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.DemiBold
                        color: Theme.inkVariant
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.outlineVariant
                }

                Repeater {
                    model: page.localMtRoutes

                    delegate: RowLayout {
                        id: mtRow
                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Theme.s3

                        Text {
                            Layout.preferredWidth: 180
                            text: mtRow.modelData.pair !== undefined ? mtRow.modelData.pair : ""
                            elide: Text.ElideRight
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.ink
                        }

                        Text {
                            Layout.fillWidth: true
                            text: mtRow.modelData.path !== undefined ? mtRow.modelData.path : ""
                            elide: Text.ElideRight
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: page.localMtRoutes.length === 0
                    text: qsTr("需下载")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                    opacity: 0.85
                }
            }

            // -------------------------------------- 本地 LLM（llama-server · 整行 · 默认收起）
            CollapsibleCard {
                id: localCard
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: false
                title: qsTr("本地 LLM（llama-server）")
                hint: qsTr("由主线程持有进程，启动会阻塞轮询 /v1/models；启动失败时进程一定已被回收。")

                FieldRow {
                    label: qsTr("模型文件")
                    Stub { }
                    ThemedCombo {
                        id: localModelCombo
                        model: page.localModelOptions
                        currentIndex: backend.localModel.length > 0 ? 1 : 0
                        onActivated: function (index) {
                            // 第一项是「自动」= 空串，后端拿第一个可用模型。
                            backend.localModel = index <= 0 ? "" : backend.localModel
                        }
                    }
                    Stub { }
                    AppButton {
                        text: qsTr("浏览…")
                        onClicked: page.modelHint = qsTr("后端还没有文件选择接口。")
                    }
                }

                FieldRow {
                    label: qsTr("上下文")
                    Stub { }
                    ThemedSpin {
                        from: 256
                        to: 262144
                        stepSize: 256
                        value: 4096
                    }
                }

                FieldRow {
                    label: qsTr("线程")
                    Stub { }
                    ThemedSpin {
                        from: 1
                        to: 256
                        stepSize: 1
                        value: 4
                    }
                }

                FieldRow {
                    label: qsTr("GPU 层数")
                    Stub { }
                    ThemedSpin {
                        id: gpuSpin
                        from: 0
                        to: 999
                        stepSize: 1
                        value: 0

                        ToolTip.visible: gpuHover.hovered
                        ToolTip.text: qsTr("Vulkan 构建需要 99")

                        HoverHandler { id: gpuHover }
                    }
                }

                FieldRow {
                    label: qsTr("优先 GPU")
                    AppToggle {
                        Layout.alignment: Qt.AlignVCenter
                        checked: backend.localUseGpu
                        onToggled: backend.localUseGpu = checked
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("装了 Vulkan 运行时就走 GPU；关掉只用 CPU。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                FieldRow {
                    label: qsTr("附加参数")
                    Stub { }
                    ThemedField {
                        font.family: Theme.fontMono
                        placeholderText: "--no-warmup --flash-attn"
                    }
                }

                // 状态行：端口 / PID / 日志 + 启动 / 停止
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3

                    StatusDot {
                        Layout.alignment: Qt.AlignVCenter
                        label: page.localLlmReady ? qsTr("运行中") : qsTr("已停止")
                        state: page.localLlmReady ? 0 : 3
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 16
                        color: Theme.outlineVariant
                    }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("端口")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    Stub { }
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: page.localPort
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.ink
                    }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: "PID"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    Stub { }
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: page.localPid
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.ink
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 16
                        color: Theme.outlineVariant
                    }

                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("日志")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    Stub { }
                    Text {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: page.localLlmReady ? qsTr("llama-server 已就绪") : qsTr("等待启动…")
                        elide: Text.ElideRight
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    AppButton {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("启动")
                        variant: "primary"
                        onClicked: backend.startLocalServer()
                    }

                    AppButton {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("停止")
                        buttonEnabled: page.localLlmReady
                        onClicked: backend.stopLocalServer()
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.s6 }
        }
    }

    // 「这个引擎还没配好」拦截框。点行才有；点徽标 / 点「去配置」都不再拦第二次。
    Popup {
        id: notReadyDialog

        property string engineKey: ""

        function openFor(key) {
            engineKey = key
            open()
        }

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(420, parent ? parent.width - Theme.s6 * 2 : 420)
        modal: true
        focus: true
        padding: Theme.s5
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        contentItem: ColumnLayout {
            spacing: Theme.s2

            Text {
                Layout.fillWidth: true
                text: qsTr("%1 还没有配置好").arg(page.engineDisplayName(notReadyDialog.engineKey))
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("需要先填好连接参数才能使用。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.s4
                spacing: Theme.s2

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("取消")
                    onClicked: notReadyDialog.close()
                }

                AppButton {
                    text: qsTr("去配置")
                    variant: "primary"
                    onClicked: {
                        var key = notReadyDialog.engineKey
                        notReadyDialog.close()
                        page.openEngineConfig(key, false)
                    }
                }
            }
        }
    }
}
