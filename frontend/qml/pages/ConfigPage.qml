import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把输入控件做成模板里的 .input / .field-like。
import QtQuick.Controls.Basic
import Studious

// 配置页：按 docs/TempUI/index.html 的 config 段复刻。
//
// 结构（对应模板）：
//   页头            h1 + 副标题
//   引擎            优先级列表（上移 / 下移）
//   引擎状态面板     backend.engineStatuses 的实时探测结果
//   Web 来源        provider 顺序 + Azure 凭据
//   云端 LLM        Base URL / Key / 模型 / 采样 / 提示词 / 重试
//   本地 MT         语言对 → 实际路径
//   本地 LLM        llama-server 的模型与启动参数
//
// 说明：后端 AppBackend 目前只暴露翻译页那套属性 + 引擎状态 + 本地服务启停，
// 没有这些配置项的读写接口，所以输入控件先承载界面状态（不落盘）；
// 能连的一律照常调 backend（刷新状态 / 启动 / 停止）。
Item {
    id: page

    // ------------------------------------------------------------ 本地数据模型

    // 引擎优先级：顺序即 Router(priority)。
    // 文案不进 model —— ListElement 的值必须是常量，包不了 qsTr；
    // 所以 model 只存 id，显示时用下面的函数按 id 取词，lupdate 才能抽到条目。
    ListModel {
        id: engineOrderModel
        ListElement { engineId: "mock"; }
        ListElement { engineId: "web"; }
        ListElement { engineId: "llm-cloud"; }
        ListElement { engineId: "local-mt"; }
    }

    // 引擎 id → 显示名。
    function engineDisplayName(id) {
        if (id === "web")
            return qsTr("Web")
        if (id === "llm-cloud")
            return qsTr("云端 LLM")
        if (id === "local-mt")
            return qsTr("本地 MT")
        return id
    }

    // 引擎 id → 一行说明。
    function engineNote(id) {
        if (id === "mock")
            return qsTr("开发占位 · 永远就绪")
        if (id === "web")
            return qsTr("google → mymemory · 无需 key")
        if (id === "llm-cloud")
            return qsTr("未配置 API 时自动跳过")
        if (id === "local-mt")
            return qsTr("OPUS-MT · 缺资产时跳过")
        return ""
    }

    // Web 来源：列表顺序即优先级。同样只存 id，文案由函数给。
    ListModel {
        id: webProviderModel
        ListElement {
            providerId: "google"
            active: true
            azureKey: ""
            azureRegion: ""
        }
        ListElement {
            providerId: "mymemory"
            active: true
            azureKey: ""
            azureRegion: ""
        }
        ListElement {
            providerId: "azure"
            active: false
            azureKey: ""
            azureRegion: ""
        }
    }

    // 来源 id → 显示名。
    function providerName(id) {
        if (id === "google")
            return qsTr("Google 在线翻译")
        if (id === "mymemory")
            return qsTr("MyMemory 记忆库")
        if (id === "azure")
            return qsTr("Microsoft Azure 翻译")
        return id
    }

    // 来源 id → 一行说明。
    function providerNote(id) {
        if (id === "google")
            return qsTr("无需 key · 免费 · 国内通常需要代理；高频会限流")
        if (id === "mymemory")
            return qsTr("无需 key · 源语言未知时按 en 处理 · 额度有限")
        if (id === "azure")
            return qsTr("需要 key · v1 仅保留接口，填了也不参与翻译")
        return ""
    }

    // 本地 MT 的可用路径。后端还没暴露资产清单，先留空 —— 走「需下载」空态。
    property var localMtRoutes: []

    // 本地服务端口 / PID：后端暂未暴露，先显示占位。
    readonly property string localPort: "—"
    readonly property string localPid: "—"

    // 「获取模型」「探测」暂时没有后端接口，点了先给一句实话。
    property string modelHint: qsTr("已获取的模型会出现在下拉里；当前为占位项。")
    property string probeHint: qsTr("尚未探测：会真的发一次翻译请求，用返回的 usage 判断思考是否被关掉。")

    // 本地 LLM 是否在跑：取 engineStatuses 里的 llm-local 一条。
    readonly property bool localLlmReady: page.engineReady("llm-local")

    // 在 engineStatuses 里查某个引擎是否就绪。
    function engineReady(id) {
        var list = backend.engineStatuses
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return list[i].ready === true
        }
        return false
    }

    Component.onCompleted: backend.refreshEngineStatuses()

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
            verticalAlignment: Qt.AlignVCenter
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
                hint: qsTr("优先级：靠前的先试，前一个不可用才轮到下一个；未列出的组件永不被自动选中。")

                Repeater {
                    model: engineOrderModel

                    delegate: Rectangle {
                        id: engineRow
                        required property int index
                        required property string engineId

                        Layout.fillWidth: true
                        implicitHeight: 52
                        radius: Theme.rMd
                        color: Theme.surface
                        border.width: 1
                        border.color: Theme.outlineVariant

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s3
                            anchors.rightMargin: Theme.s2
                            spacing: Theme.s2

                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                text: engineRow.index + 1
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Font.DemiBold
                                color: Theme.inkVariant
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 0

                                Text {
                                    text: page.engineDisplayName(engineRow.engineId)
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: Font.DemiBold
                                    color: Theme.ink
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: page.engineNote(engineRow.engineId)
                                    elide: Text.ElideRight
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: Theme.inkVariant
                                }
                            }

                            AppButton {
                                Layout.alignment: Qt.AlignVCenter
                                text: qsTr("上移")
                                variant: "text"
                                buttonEnabled: engineRow.index > 0
                                onClicked: engineOrderModel.move(engineRow.index,
                                                                  engineRow.index - 1, 1)
                            }

                            AppButton {
                                Layout.alignment: Qt.AlignVCenter
                                text: qsTr("下移")
                                variant: "text"
                                buttonEnabled: engineRow.index < engineOrderModel.count - 1
                                onClicked: engineOrderModel.move(engineRow.index,
                                                                  engineRow.index + 1, 1)
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

            // ------------------------------------------------ 引擎状态面板（整行 · 默认展开）
            CollapsibleCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                expanded: true
                title: qsTr("引擎状态面板")
                hint: qsTr("按当前语言对实时探测每个引擎是否可用；绿点＝就绪，灰点＝未就绪。")

                headerActions: AppButton {
                    text: qsTr("刷新")
                    variant: "text"
                    onClicked: backend.refreshEngineStatuses()
                }

                Repeater {
                    model: backend.engineStatuses

                    delegate: Rectangle {
                        id: statusRow
                        required property var modelData

                        Layout.fillWidth: true
                        implicitHeight: 40
                        radius: Theme.rSm
                        color: Theme.surface

                        StatusDot {
                            anchors.left: parent.left
                            anchors.leftMargin: Theme.s3
                            anchors.verticalCenter: parent.verticalCenter
                            label: statusRow.modelData.id !== undefined
                                   ? statusRow.modelData.id : ""
                            detail: statusRow.modelData.detail !== undefined
                                    ? statusRow.modelData.detail : ""
                            state: statusRow.modelData.ready ? 0 : 3
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: backend.engineStatuses.length === 0
                    text: qsTr("尚未探测到任何引擎。")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                }
            }

            // ------------------------ 两列区：Web 来源 / 云端 LLM / 本地 MT / 本地 LLM
            // 「引擎」「引擎状态面板」占整行，其余四张放进 Flow：
            // 单卡宽度 = (可用宽度 - s4) / 2 —— 宽屏两列，窗口窄到放不下时自动掉成一列。
            Flow {
                id: cardFlow
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                Layout.preferredHeight: implicitHeight
                spacing: Theme.s4

                // ---------------------------------------------------- Web 来源（半宽 · 默认收起）
                CollapsibleCard {
                    width: cardFlow.width >= 960 ? (cardFlow.width - Theme.s4) / 2 : cardFlow.width
                    expanded: false
                    title: qsTr("Web 来源")
                    hint: qsTr("列表顺序即优先级，靠前的先试。")
                    Stub { }

                    Repeater {
                        model: webProviderModel

                        delegate: ColumnLayout {
                            id: providerItem
                            required property int index
                            required property string providerId
                            required property bool active

                            Layout.fillWidth: true
                            spacing: Theme.s1

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.s2

                                Stub { }
                                CheckBox {
                                    id: providerCheck

                                    Layout.alignment: Qt.AlignVCenter
                                    leftPadding: 0
                                    rightPadding: 0
                                    spacing: Theme.s2
                                    checked: providerItem.active

                                    indicator: Rectangle {
                                        implicitWidth: 18
                                        implicitHeight: 18
                                        x: providerCheck.leftPadding
                                        y: providerCheck.topPadding
                                           + (providerCheck.availableHeight - height) / 2
                                        radius: Theme.rXs
                                        color: providerCheck.checked ? Theme.primary : "transparent"
                                        border.width: 1
                                        border.color: providerCheck.checked ? Theme.primary
                                                                            : Theme.outline

                                        Text {
                                            anchors.centerIn: parent
                                            visible: providerCheck.checked
                                            text: "\u2713"
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            font.weight: Font.Bold
                                            color: Theme.primaryText
                                        }
                                    }

                                    contentItem: Text {
                                        leftPadding: providerCheck.indicator.width
                                                     + providerCheck.spacing
                                        text: page.providerName(providerItem.providerId)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.ink
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    onToggled: webProviderModel.setProperty(providerItem.index,
                                                                            "active", checked)
                                }

                                Item { Layout.fillWidth: true }

                                Stub { }
                                AppButton {
                                    Layout.alignment: Qt.AlignVCenter
                                    text: qsTr("上移")
                                    variant: "text"
                                    buttonEnabled: providerItem.index > 0
                                    onClicked: webProviderModel.move(providerItem.index,
                                                                     providerItem.index - 1, 1)
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26
                                text: page.providerNote(providerItem.providerId)
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }

                            // Azure 勾选后才展开凭据
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26
                                visible: providerItem.providerId === "azure" && providerItem.active
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

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("未知的来源 id 会被静默忽略；只剩一个来源时「上移」自动置灰。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                // --------------------------------------------------- 云端 LLM（半宽 · 默认展开）
                CollapsibleCard {
                    width: cardFlow.width >= 960 ? (cardFlow.width - Theme.s4) / 2 : cardFlow.width
                    expanded: true
                    title: qsTr("云端 LLM")
                    hint: qsTr("OpenAI 兼容接口；没填 API Key 时这条链路自动跳过。")
                    Stub { }

                    FieldRow {
                        label: "Base URL"
                        Stub { }
                        ThemedField {
                            font.family: Theme.fontMono
                            placeholderText: "http://127.0.0.1:3065/v1"
                        }
                    }

                    FieldRow {
                        label: "API Key"
                        Stub { }
                        ThemedField {
                            font.family: Theme.fontMono
                            placeholderText: "sk-…"
                            echoMode: TextInput.Password
                        }
                    }

                    FieldRow {
                        label: qsTr("模型")
                        Stub { }
                        ThemedCombo {
                            id: modelCombo
                            model: ["deepseek-v4.1-flash", "qwen3-32b", "gpt-4.1-mini", "glm-4.6",
                                    "llama-3.3-70b"]
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
                        Stub { }
                        ThemedSpin {
                            from: 0
                            to: 200
                            stepSize: 10
                            value: 20

                            textFromValue: function (v) { return (v / 100).toFixed(1) }
                            valueFromText: function (t) {
                                var n = parseFloat(t)
                                return isNaN(n) ? 0 : Math.round(n * 100)
                            }
                        }
                    }

                    FieldRow {
                        label: "maxTokens"
                        Stub { }
                        ThemedSpin {
                            from: 1
                            to: 32768
                            stepSize: 1
                            value: 2048
                        }
                    }

                    FieldRow {
                        label: "timeoutMs"
                        Stub { }
                        ThemedSpin {
                            from: 1000
                            to: 600000
                            stepSize: 1000
                            value: 60000
                        }
                    }

                    FieldRow {
                        label: "systemPrompt"
                        Stub { }
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
                        Stub { }
                        ThemedCombo {
                            model: [qsTr("关闭"), qsTr("极低"), qsTr("低"), qsTr("中"), qsTr("高"),
                                    qsTr("跟随默认")]
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
                        Stub { }
                        ThemedSpin {
                            from: 1
                            to: 64
                            stepSize: 1
                            value: 8
                        }
                    }

                    FieldRow {
                        label: qsTr("重试预算")
                        Stub { }
                        ThemedSpin {
                            from: 1000
                            to: 600000
                            stepSize: 1000
                            value: 30000
                        }
                    }

                    FieldRow {
                        label: qsTr("清理模型注记")
                        Stub { }
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: false
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

                // ----------------------------------------------------- 本地 MT（半宽 · 默认收起）
                CollapsibleCard {
                    width: cardFlow.width >= 960 ? (cardFlow.width - Theme.s4) / 2 : cardFlow.width
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

                // -------------------------------------- 本地 LLM（llama-server · 半宽 · 默认收起）
                CollapsibleCard {
                    width: cardFlow.width >= 960 ? (cardFlow.width - Theme.s4) / 2 : cardFlow.width
                    expanded: false
                    title: qsTr("本地 LLM（llama-server）")
                    hint: qsTr("由主线程持有进程，启动会阻塞轮询 /v1/models；启动失败时进程一定已被回收。")

                    FieldRow {
                        label: qsTr("模型文件")
                        Stub { }
                        ThemedCombo {
                            model: ["model.qwen2.5-3b-instruct.q4km",
                                    "model.qwen2.5-7b-instruct.q4km",
                                    "model.qwen2.5-coder-7b.q4km"]
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
            }

            Item { Layout.preferredHeight: Theme.s6 }
        }
    }
}
