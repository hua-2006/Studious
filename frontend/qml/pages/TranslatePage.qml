import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把 ComboBox 做成模板里的 .field-like。
import QtQuick.Controls.Basic
import Studious

// 翻译页：按 docs/TempUI/index.html 的 translate 段一比一复刻。
//
// 结构（对应模板）：
//   .page-head        页头
//   .tr-controls      源语言 / 目标语言 / 引擎 三组控制
//   .tr-toolbar       原文对照开关 + 缓存键提示
//   .tr-action        右侧运行按钮（FAB）+ 提示
//   .panes            原文 / 译文 双栏
//   .statusbar        底部引擎状态条
Item {
    id: page

    // 跳到其它页面（Main.qml 接手，页面本身不切页）。
    signal navigateRequested(int index)

    // 后端启动时不会主动探测引擎与缓存，进页面时拉一次，状态条才有内容。
    Component.onCompleted: {
        backend.refreshEngineStatuses()
        backend.refreshCacheStats()
        // 前端改成单选了：把历史设置里残留的多目标收敛成第一个，
        // 否则界面上会同时亮两个胶囊。后端接口不动。
        if (backend.dstLangs.length > 1) {
            backend.dstLangs = [backend.dstLangs[0]]
        }
    }

    // ---- 与模板 CSS 对齐的尺寸常量（像素值直接对应 CSS）----
    readonly property int hField: 44        // .field-like 高
    readonly property int hPaneHead: 44     // .pane-head 高
    readonly property int hPaneFoot: 40     // .pane-foot 高
    readonly property int hRunFab: 48       // .run-fab 高
    readonly property int wRunFab: 190      // 运行按钮宽
    readonly property int wSourceField: 200 // .tr-controls 第一列 200px
    readonly property int hStatusBar: 48    // .statusbar 高
    readonly property int hDivider: 22      // .tr-toolbar 竖分隔高
    readonly property int hMenuItem: 40     // .menu-item 高
    readonly property int paneFontSize: 16  // textarea.src 1rem

    // 语言名直接用后端给的：LanguageRegistry::displayName() 已经按界面语言双语取词，
    // 前端不再自己维护一份 code→中文映射表（两份表迟早会不一致）。
    // 注意不要把后端的 label 再塞进 entryLabel() 里二次映射 ——
    // 引擎里也有 code 为 "auto" 的项，会串味成「自动检测」。

    // 取语言 / 引擎条目的显示名（条目形如 {code,label}）。
    function entryLabel(entry) {
        if (entry === undefined || entry === null)
            return ""
        return entry.label !== undefined ? entry.label : String(entry)
    }

    // 取语言 / 引擎条目的代码。
    function entryCode(entry) {
        if (entry === undefined || entry === null)
            return ""
        return entry.code !== undefined ? entry.code : String(entry)
    }

    readonly property var languages: backend.languages

    // 语言码 → 显示名。
    function langName(code) {
        var list = page.languages
        for (var i = 0; i < list.length; ++i) {
            if (entryCode(list[i]) === code)
                return entryLabel(list[i])
        }
        return code
    }

    // 引擎 id → 显示名（空 id 即「自动」）。
    function engineName(id) {
        var list = backend.engines
        for (var i = 0; i < list.length; ++i) {
            if (entryCode(list[i]) === id)
                return entryLabel(list[i])
        }
        return id
    }

    // 常用语言直接铺开，其余收进「更多语言」——对齐模板（5 个常用 + 折叠菜单）。
    property bool showAllLangs: false
    readonly property var primaryLangCodes: ["zh", "zh-TW", "en", "ja", "ko"]

    function isPrimaryLang(code) {
        return primaryLangCodes.indexOf(code) >= 0
    }

    // 目标语言下拉的条目：从 languages 里去掉「自动检测」。
    readonly property var targetLanguages: {
        var out = []
        var list = page.languages
        for (var i = 0; i < list.length; ++i) {
            if (entryCode(list[i]) !== "auto")
                out.push(list[i])
        }
        return out
    }

    // 语言检测：后端 i18n::detectLanguage 一直在，只是界面从没用过。
    // 只在源语言是「自动」时显示 —— 用户已经手选了语言，再报一个检测结果纯属噪音。
    readonly property string detectedLang: backend.srcLang === "auto"
                                           ? backend.detectLanguage(backend.sourceText) : ""

    // 目标语言增删；删空时回落到中文(简体)，保证至少一个目标。
    // 目标语言单选：后端接口仍然支持多目标（dstLangs 是数组），
    // 但前端只暴露一个值 —— 多选对实际使用没什么价值，先在前端禁掉，后端不动。
    function toggleDst(code) {
        if (backend.dstLangs.length === 1 && backend.dstLangs[0] === code)
            return
        backend.dstLangs = [code]
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s6
        spacing: Theme.s4

        // ------------------------------------------------------------ 页头
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.s2

            Text {
                text: qsTr("欢迎来到 Studious")
                font.family: Theme.fontUi
                // 控制项挪到下面后这一行空出来了，标题放大占住这块空白。
                // 仍跟着「字号缩放」走（fontSizeDisplay 本身已经乘过 fontScale）。
                font.pixelSize: Math.round(Theme.fontSizeDisplay * 1.8)
                font.weight: Font.Bold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("文字如舟，渡你过语言的河。选好目标语言就能出发。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }
        }

        // -------------------------------------------------------- 三组控制
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.s6
            spacing: Theme.s4

            // 语言组：源语言在左、目标语言在右（横排，每列按单个字段宽）。
            // 用 Row 而不是 RowLayout：子项自带 width，位置器不会重新协商宽度，
            // 否则两个下拉会被外层 RowLayout 撑开。
            Row {
                id: langGroup
                Layout.alignment: Qt.AlignTop
                spacing: Theme.s4

            // 源语言：下拉
            ColumnLayout {
                id: srcCol
                width: page.wSourceField
                spacing: Theme.s1

                Text {
                    Layout.fillWidth: true
                    text: page.detectedLang.length > 0
                          ? qsTr("源语言 · 检测到%1").arg(page.langName(page.detectedLang))
                          : qsTr("源语言")
                    elide: Text.ElideRight
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.inkVariant
                }

                // 用 AppComboBox：它自带主题化的背景/文字/popup，
                // 裸 ComboBox 会吃掉 QPalette::Base，深色下变成一块白底。
                AppComboBox {
                    id: srcCombo
                    // ColumnLayout 里用 Layout.preferredWidth（width 不参与布局协商）。
                    Layout.preferredWidth: page.wSourceField
                    Layout.preferredHeight: page.hField

                    model: page.languages
                    textRole: "label"
                    valueRole: "code"

                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody

                    leftPadding: Theme.s3
                    rightPadding: Theme.s3
                    topPadding: 0
                    bottomPadding: 0

                    onActivated: backend.srcLang = currentValue

                    // 后端改了源语言（切页 / 恢复设置）时同步显示项。
                    function syncIndex() {
                        var list = page.languages
                        for (var i = 0; i < list.length; ++i) {
                            if (page.entryCode(list[i]) === backend.srcLang) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = 0
                    }

                    Component.onCompleted: syncIndex()

                    background: Rectangle {
                        radius: Theme.rMd
                        color: srcCombo.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
                        border.width: 0

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    }

                    contentItem: Text {
                        rightPadding: srcCombo.indicator.width + Theme.s2
                        text: srcCombo.displayText
                        font: srcCombo.font
                        color: Theme.ink
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    indicator: Text {
                        x: srcCombo.width - width - srcCombo.rightPadding
                        y: srcCombo.topPadding + (srcCombo.availableHeight - height) / 2
                        text: "\u25BE"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.inkVariant
                    }

                    delegate: ItemDelegate {
                        id: langItem
                        width: srcCombo.width - Theme.s3
                        height: page.hMenuItem

                        required property int index
                        required property var modelData

                        text: page.entryLabel(langItem.modelData)
                        highlighted: srcCombo.highlightedIndex === index

                        contentItem: Text {
                            text: langItem.text
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            color: Theme.ink
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }

                        background: Rectangle {
                            radius: Theme.rSm
                            color: langItem.highlighted ? Theme.surfaceHighest : "transparent"
                        }
                    }

                    // 下拉列表样式统一在 components/AppComboBox.qml 里定义，
                    // 这里不再单独覆盖 popup —— 否则两个下拉会长得不一样。
                }
            }

            // 目标语言：和源语言同款下拉，并排放右边。
            // 单选 —— 后端 dstLangs 仍是数组，前端只放一个值。
            ColumnLayout {
                id: dstCol
                width: page.wSourceField
                spacing: Theme.s1

                Text {
                    Layout.preferredWidth: page.wSourceField
                    text: qsTr("目标语言")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.inkVariant
                }

                AppComboBox {
                    id: dstCombo
                    Layout.preferredWidth: page.wSourceField
                    Layout.preferredHeight: page.hField

                    model: page.targetLanguages
                    textRole: "label"
                    valueRole: "code"

                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody

                    leftPadding: Theme.s3
                    rightPadding: Theme.s3
                    topPadding: 0
                    bottomPadding: 0

                    onActivated: backend.dstLangs = [currentValue]

                    // 后端改了目标语言（切页 / 恢复设置）时同步显示项。
                    function syncIndex() {
                        var list = page.targetLanguages
                        var want = backend.dstLangs.length > 0 ? backend.dstLangs[0] : "zh"
                        for (var i = 0; i < list.length; ++i) {
                            if (page.entryCode(list[i]) === want) {
                                currentIndex = i
                                return
                            }
                        }
                        currentIndex = 0
                    }

                    Component.onCompleted: syncIndex()

                    Connections {
                        target: backend
                        function onDstLangsChanged() { dstCombo.syncIndex() }
                    }

                    background: Rectangle {
                        radius: Theme.rMd
                        color: dstCombo.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
                        border.width: 0

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    }

                    contentItem: Text {
                        rightPadding: dstCombo.indicator.width + Theme.s2
                        text: dstCombo.displayText
                        font: dstCombo.font
                        color: Theme.ink
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }

                    indicator: Text {
                        x: dstCombo.width - width - dstCombo.rightPadding
                        y: dstCombo.topPadding + (dstCombo.availableHeight - height) / 2
                        text: "\u25BE"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.inkVariant
                    }
                }
            }

            }   // 语言组结束

            Item { Layout.fillWidth: true }

            // 引擎：控制行右侧
            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: Theme.s1

                Text {
                    text: qsTr("引擎")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.inkVariant
                }

                Segmented {
                    id: engineSeg
                    items: backend.engines
                    current: backend.engineId
                    onSelected: function (code) { backend.engineId = code }
                }
            }
        }

        // ------------------------------------------- 工具行 + 运行按钮
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s3

            // 原文对照 + 缓存键：独立一行，在源语言/目标语言下面
            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.s3

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("原文对照")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.inkVariant
                }

                AppToggle {
                    id: sourceToggle
                    Layout.alignment: Qt.AlignVCenter
                    checked: backend.showSource
                    onToggled: function (on) { backend.showSource = on }
                }

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: page.hDivider
                    color: Theme.outlineVariant
                }

                Badge {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("缓存键含引擎")
                }
            }

            Item { Layout.fillWidth: true }

            // .tr-action：按钮右对齐，提示在下方
            ColumnLayout {
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
                spacing: Theme.s1

                Rectangle {
                    id: runFab
                    Layout.alignment: Qt.AlignRight
                    Layout.preferredWidth: page.wRunFab
                    Layout.preferredHeight: page.hRunFab
                    radius: Theme.rFull
                    color: backend.running
                           ? Theme.surfaceHighest
                           : (runHover.hovered ? Theme.primaryContainer : Theme.primary)

                    Behavior on color { ColorAnimation { duration: Theme.durFast } }

                    Text {
                        anchors.centerIn: parent
                        text: backend.running ? qsTr("翻译中…") : qsTr("开始翻译")
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody + 2   // .run-label .9375rem
                        font.weight: Font.DemiBold
                        color: backend.running ? Theme.inkVariant
                                               : (runHover.hovered ? Theme.primaryContainerText
                                                                  : Theme.primaryText)
                    }

                    HoverHandler { id: runHover }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (backend.running)
                                backend.cancel()
                            else
                                backend.translate()
                        }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("点击开始翻译 · 翻译中可取消")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }
            }
        }

        // ------------------------------------------------------------ 双栏
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s4

            // ---- 原文 ----
            Rectangle {
                id: srcPane
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.rXl
                color: Theme.surface
                border.width: 1
                border.color: Theme.outlineVariant
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // .pane-head
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: page.hPaneHead
                        color: Theme.surfaceLow
                        topLeftRadius: Theme.rXl
                        topRightRadius: Theme.rXl

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: Theme.outlineVariant
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s4
                            anchors.rightMargin: Theme.s3
                            spacing: Theme.s2

                            Text {
                                text: qsTr("原文")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.DemiBold
                                color: Theme.inkVariant
                            }

                            Badge { text: qsTr("%1 段").arg(backend.paragraphCount) }

                            Item { Layout.fillWidth: true }

                            AppButton {
                                text: qsTr("粘贴")
                                variant: "text"
                                onClicked: backend.pasteFromClipboard()
                            }

                            AppButton {
                                text: qsTr("清空")
                                variant: "text"
                                onClicked: backend.clearSource()
                            }
                        }
                    }

                    // .pane-body
                    ScrollView {
                        id: srcScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded

                        TextArea {
                            id: srcArea
                            width: srcScroll.availableWidth
                            background: null
                            padding: Theme.s4
                            wrapMode: TextArea.Wrap
                            selectByMouse: true
                            text: backend.sourceText
                            placeholderText: qsTr("在这里输入或粘贴要翻译的文字…")
                            color: Theme.ink
                            placeholderTextColor: Theme.inkVariant
                            selectionColor: Theme.primaryContainer
                            selectedTextColor: Theme.primaryContainerText
                            // 正文用「内容字体」（霞鹜文楷），界面其它地方用思源黑体。
                            font.family: Theme.fontContent
                            font.pixelSize: page.paneFontSize

                            // 双向绑定：用户输入回写后端，后端改值不会再回环。
                            onTextChanged: {
                                if (backend.sourceText !== text)
                                    backend.sourceText = text
                            }
                        }
                    }

                    // .pane-foot
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: page.hPaneFoot
                        color: Theme.surfaceLow
                        bottomLeftRadius: Theme.rXl
                        bottomRightRadius: Theme.rXl

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 1
                            color: Theme.outlineVariant
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s4
                            anchors.rightMargin: Theme.s3
                            spacing: Theme.s2

                            Text {
                                text: qsTr("源语言 · %1").arg(backend.srcLang)
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }

                            Item { Layout.fillWidth: true }

                            Badge { text: qsTr("UTF-8 · %1 字符").arg(backend.charCount) }
                        }
                    }
                }
            }

            // ---- 译文 ----
            Rectangle {
                id: dstPane
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.rXl
                color: Theme.surface
                border.width: 1
                border.color: Theme.outlineVariant
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: page.hPaneHead
                        color: Theme.surfaceLow
                        topLeftRadius: Theme.rXl
                        topRightRadius: Theme.rXl

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: Theme.outlineVariant
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s4
                            anchors.rightMargin: Theme.s3
                            spacing: Theme.s2

                            Text {
                                text: qsTr("译文")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.DemiBold
                                color: Theme.inkVariant
                            }

                            // 目标语言 tab：多目标时切换看哪个译文（模板的 dst-tabs）。
                            Row {
                                spacing: Theme.s1

                                Repeater {
                                    model: backend.dstLangs

                                    delegate: Rectangle {
                                        id: dstTab
                                        required property string modelData

                                        height: 24
                                        width: tabLabel.implicitWidth + Theme.s3
                                        radius: Theme.rFull
                                        color: backend.activeDst === modelData
                                               ? Theme.primary : "transparent"
                                        border.width: backend.activeDst === modelData ? 0 : 1
                                        border.color: Theme.outlineVariant

                                        Behavior on color {
                                            ColorAnimation { duration: Theme.durFast }
                                        }

                                        Text {
                                            id: tabLabel
                                            anchors.centerIn: parent
                                            text: page.langName(dstTab.modelData)
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            font.weight: backend.activeDst === dstTab.modelData
                                                         ? Font.DemiBold : Font.Normal
                                            color: backend.activeDst === dstTab.modelData
                                                   ? Theme.primaryText : Theme.inkVariant
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: backend.activeDst = dstTab.modelData
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            AppButton {
                                text: qsTr("复制全部")
                                variant: "text"
                                onClicked: backend.copyTranslated()
                            }
                        }
                    }

                    ScrollView {
                        id: dstScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded

                        // 逐段渲染：每段一个文本块，passthrough 的段带徽标。
                        // 用 Column 而不是单个 TextArea，才能把段与段之间的标记表达出来。
                        Column {
                            width: dstScroll.availableWidth
                            spacing: 0

                            Text {
                                x: Theme.s4
                                topPadding: Theme.s4
                                width: parent.width - Theme.s4 * 2
                                visible: backend.blocks.length === 0
                                text: qsTr("译文会出现在这里")
                                wrapMode: Text.Wrap
                                color: Theme.inkVariant
                                // 占位也是译文区的内容，字体要跟正文一致（霞鹜文楷），
                                // 之前用了界面字体，看起来「不对」就是这个原因。
                                font.family: Theme.fontContent
                                font.pixelSize: page.paneFontSize
                            }

                            Repeater {
                                model: backend.blocks

                                delegate: Column {
                                    id: blockCol
                                    required property var modelData

                                    x: Theme.s4
                                    width: dstScroll.availableWidth - Theme.s4 * 2
                                    topPadding: Theme.s3
                                    spacing: Theme.s1

                                    // 「这是原文」徽标：源 = 目标时后端原样返回
                                    Rectangle {
                                        visible: blockCol.modelData.passthrough
                                        width: passLabel.implicitWidth + Theme.s2 * 2
                                        height: 18
                                        radius: Theme.rFull
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Theme.outlineVariant

                                        Text {
                                            id: passLabel
                                            anchors.centerIn: parent
                                            text: qsTr("这是原文")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            color: Theme.inkVariant
                                        }
                                    }

                                    // 原文对照：开关打开时，每段译文上方显示对应原文。
                                    Text {
                                        width: parent.width
                                        visible: backend.showSource
                                        text: blockCol.modelData.source
                                        wrapMode: Text.Wrap
                                        color: Theme.inkVariant
                                        font.family: Theme.fontContent
                                        font.pixelSize: Math.max(10, page.paneFontSize - 1)
                                        opacity: 0.85
                                    }

                                    // 用 TextEdit 而不是 Text：译文要能用鼠标选中复制。
                                    TextEdit {
                                        width: parent.width
                                        height: contentHeight
                                        text: blockCol.modelData.empty
                                              ? qsTr("（该段无译文）")
                                              : blockCol.modelData.text
                                        wrapMode: TextEdit.Wrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Theme.primaryContainer
                                        selectedTextColor: Theme.primaryContainerText
                                        color: blockCol.modelData.empty
                                               ? Theme.inkVariant : Theme.ink
                                        // 译文同样用内容字体。
                                        font.family: Theme.fontContent
                                        font.pixelSize: page.paneFontSize
                                    }
                                }
                            }

                            Item { width: 1; height: Theme.s4 }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: page.hPaneFoot
                        color: Theme.surfaceLow
                        bottomLeftRadius: Theme.rXl
                        bottomRightRadius: Theme.rXl

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 1
                            color: Theme.outlineVariant
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s4
                            anchors.rightMargin: Theme.s3
                            spacing: Theme.s2

                            Text {
                                text: qsTr("引擎 · %1").arg(page.engineName(backend.engineId))
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }

                            Item { Layout.fillWidth: true }

                            Badge {
                                visible: backend.cacheHits > 0
                                filled: true
                                text: qsTr("命中 %1").arg(backend.cacheHits)
                            }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------- 状态行
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            spacing: Theme.s1
            visible: backend.statusText.length > 0 || backend.statusWarning.length > 0

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: backend.statusText
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: backend.statusIsError ? Theme.danger : Theme.inkVariant
            }

            // .tr-warning：警告提示条
            Rectangle {
                Layout.fillWidth: true
                visible: backend.statusWarning.length > 0
                implicitHeight: warnRow.implicitHeight + Theme.s2 * 2
                radius: Theme.rSm
                color: Theme.cautionContainer

                RowLayout {
                    id: warnRow
                    anchors.fill: parent
                    anchors.leftMargin: Theme.s2
                    anchors.rightMargin: Theme.s2
                    spacing: Theme.s1

                    Text {
                        Layout.alignment: Qt.AlignTop
                        text: "⚠"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.caution
                    }

                    Text {
                        Layout.fillWidth: true
                        text: backend.statusWarning
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.caution
                    }
                }
            }
        }

        // ------------------------------------------------------ 引擎状态条
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: page.hStatusBar
            radius: Theme.rLg
            color: Theme.surfaceLow
            border.width: 1
            border.color: Theme.outlineVariant
            clip: true

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.s4
                anchors.rightMargin: Theme.s3
                spacing: Theme.s5

                Repeater {
                    model: backend.engineStatuses

                    delegate: Item {
                        id: statusSlot
                        required property var modelData

                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: statusDot.implicitWidth
                        Layout.preferredHeight: statusDot.implicitHeight

                        StatusDot {
                            id: statusDot
                            anchors.verticalCenter: parent.verticalCenter
                            label: statusSlot.modelData.id !== undefined ? statusSlot.modelData.id : ""
                            detail: statusSlot.modelData.detail !== undefined ? statusSlot.modelData.detail : ""
                            state: statusSlot.modelData.ready ? 0 : 3
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("查看引擎状态")
                    variant: "text"
                    onClicked: page.navigateRequested(2)
                }
            }
        }
    }

    // 后端改了源语言时，下拉显示项跟上。
    Connections {
        target: backend
        function onSrcLangChanged() { srcCombo.syncIndex() }
        function onShowSourceChanged() { sourceToggle.checked = backend.showSource }
        function onEngineIdChanged() { engineSeg.current = backend.engineId }

        // 用户输入会打断 TextArea.text 的绑定，这里兜住后端主动改原文的情况（清空 / 粘贴）。
        function onSourceTextChanged() {
            if (srcArea.text !== backend.sourceText)
                srcArea.text = backend.sourceText
        }
    }
}
