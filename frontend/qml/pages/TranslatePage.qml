import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把 ComboBox 做成模板里的 .field-like。
import QtQuick.Controls.Basic
import QtQuick.Effects
import Studious

// 起点页（原「翻译」页）。
//
// 这一页现在只负责「调配置 + 看信息」：语言对、引擎、引擎状态。
// 真正的翻译动作全部交给三个入口卡片：
//   小窗口 / 截图翻译 / 实时翻译
// 三张卡的动作还没实现，先留 signal launchRequested(kind)，Main.qml 以后接。
//
// 结构：
//   .page-head   页头
//   .tr-controls 源语言 / 目标语言 / 引擎
//   .tr-toolbar  左侧提示 + 右侧「缓存键含引擎」提示
//   .launchers   三个入口卡片
//   .statusbar   底部引擎状态条
Item {
    id: page

    // 跳到其它页面（Main.qml 接手，页面本身不切页）。
    signal navigateRequested(int index)

    // 三个入口的动作。kind 取值："mini" / "capture" / "live"。
    // 现在没有消费者，等平台层（抓图 / 框选 / 全局热键 / OCR）落地后再接。
    signal launchRequested(string kind)

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
    readonly property int wSourceField: 200 // .tr-controls 第一列 200px
    readonly property int hStatusBar: 48    // .statusbar 高
    readonly property int hMenuItem: 40     // .menu-item 高

    // 入口卡被点了但功能还没接通时的回执，显示在工具行左侧。
    property string lastHint: ""

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

    // 状态条用的短名：backend.engineStatuses 每项只带裸 id（web / llm-cloud / llm-local /
    // local-mt），直接把 id 印出来是给开发者看的，不是给用户看的。
    // 只在本页做这一份映射，不去调 ConfigPage 的 engineName（跨文件调用会把两页耦死）。
    function statusEngineName(id) {
        // 状态条只带裸 id，名字由后端统一给（与三个浮层的下拉一致）。
        return backend.engineName(id)
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

    // 源语言下拉的条目：设置里关掉「自动检测源语言」时，把「自动」整项摘掉。
    // 摘掉之后当前值一定不能还是 auto（后端的 setAutoDetectSource 会顺手改成具体语言）。
    readonly property var sourceLanguages: {
        if (backend.autoDetectSource)
            return page.languages
        var out = []
        var list = page.languages
        for (var i = 0; i < list.length; ++i) {
            if (entryCode(list[i]) !== "auto")
                out.push(list[i])
        }
        return out
    }

    // 同语言提示：源=目标时后端原样回填、根本不走引擎，
    // 不提前说一句用户会以为「翻译坏了」。
    readonly property string sameLangNotice: {
        if (!backend.sameLangHint)
            return ""
        var src = backend.srcLang
        if (src === "" || src === "auto")
            return ""
        for (var i = 0; i < backend.dstLangs.length; ++i) {
            if (backend.dstLangs[i] === src)
                return qsTr("源语言和目标语言相同，会原样返回。")
        }
        return ""
    }

    // 工具行左侧那行提示：同语言警告优先，其次是入口点击回执。
    readonly property string hintText: page.sameLangNotice.length > 0
                                       ? page.sameLangNotice
                                       : (page.lastHint.length > 0
                                          ? page.lastHint
                                          : qsTr("选好语言和引擎，再从下面的入口开始。"))

    // ------------------------------------------------------------ 入口卡
    //
    // 三张卡长得一样，所以做成内联 component。悬浮时的反馈同一条规则：
    // 底色抬一档、描边换强调色、图标跟着变色、整卡轻微放大、右下箭头滑进来。
    // 动画只用 Theme.durFast（150ms）与 transform 类属性，不触发重新布局。
    component LauncherCard: Rectangle {
        id: card

        required property string iconSource
        required property string title
        required property string description
        required property string tags
        // 底部那行小字：做完了的入口说"点一下开始"，没做的老实说"接口已预留"。
        required property string footer
        required property string kind

        signal activated()

        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 240
        Layout.minimumHeight: 156
        Layout.preferredHeight: 200

        radius: Theme.rXl
        color: hover.hovered ? Theme.surfaceHigh : Theme.surface
        border.width: 1
        border.color: hover.hovered ? Theme.primary : Theme.outlineVariant

        scale: hover.pressed ? 0.99 : (hover.hovered ? 1.012 : 1.0)

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
        Behavior on scale {
            NumberAnimation { duration: Theme.durFast; easing.type: Easing.OutCubic }
        }

        HoverHandler {
            id: hover
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: card.activated()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.s5
            spacing: Theme.s3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s3

                Item {
                    Layout.alignment: Qt.AlignTop
                    implicitWidth: 32
                    implicitHeight: 32

                    Image {
                        anchors.fill: parent
                        source: card.iconSource
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        // 图标源文件是纯白描边（不是深紫），所以这里的着色等于
                        // 直接把描边换成 tint —— 深色主题给亮色，浅色主题给深色。
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            colorizationColor: hover.hovered ? Theme.primary : Theme.inkVariant
                            colorization: 1.0
                            Behavior on colorizationColor {
                                ColorAnimation { duration: Theme.durFast }
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: card.title
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: card.description
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.inkVariant
                lineHeight: 1.25
            }

            // 一行要点，填住卡片中段，也把「这个入口大概长什么样」提前交代清楚。
            Text {
                Layout.fillWidth: true
                text: card.tags
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTiny
                color: hover.hovered ? Theme.primary : Theme.inkVariant
                opacity: hover.hovered ? 1.0 : 0.7

                Behavior on color { ColorAnimation { duration: Theme.durFast } }
            }

            Item { Layout.fillHeight: true }

            // 卡高了之后中间会空一大块，给底部一条分隔线 + 一行页脚，
            // 视觉上收住，也把「→」放到该在的位置（悬浮才浮现）。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: hover.hovered ? Theme.outline : Theme.outlineVariant

                Behavior on color { ColorAnimation { duration: Theme.durFast } }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    Layout.fillWidth: true
                    text: card.footer
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: hover.hovered ? Theme.inkVariant : Theme.inkVariant
                    opacity: 0.75
                }

                Text {
                    text: "\u2192"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.primary
                    opacity: hover.hovered ? 1.0 : 0.0

                    Behavior on opacity {
                        NumberAnimation { duration: Theme.durFast }
                    }
                }
            }
        }
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
                text: qsTr("文字如舟，渡你过语言的河。先选好语言与引擎，再挑一个入口。")
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

                        model: page.sourceLanguages
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
                            var list = page.sourceLanguages
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

        // ------------------------------------------------------------ 工具行
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.s3

            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: page.hintText
                elide: Text.ElideRight
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: page.sameLangNotice.length > 0 ? Theme.caution : Theme.inkVariant
            }

            Badge {
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("缓存键含引擎")
            }
        }

        // -------------------------------------------------------- 三个入口
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 176
            spacing: Theme.s4

            LauncherCard {
                iconSource: "qrc:/qt/qml/Studious/resources/icons/mini.svg"
                title: qsTr("小窗口")
                description: qsTr("一个置顶小窗，随手输入随手出译文，不打断你正在做的事。")
                tags: qsTr("全局热键　·　置顶不抢焦点　·　回车即译")
                objectName: "cardMini"
                footer: qsTr("点一下打开小窗")
                kind: "mini"
                onActivated: page.launchRequested("mini")
            }

            LauncherCard {
                iconSource: "qrc:/qt/qml/Studious/resources/icons/screenshot.svg"
                title: qsTr("截图翻译")
                description: qsTr("框选屏幕任意区域，识别文字后直接给译文，图片不外传。")
                tags: qsTr("框选即译　·　本地 OCR　·　自动复制")
                objectName: "cardCapture"
                footer: qsTr("点一下开始框选")
                kind: "capture"
                onActivated: page.launchRequested("capture")
            }

            LauncherCard {
                iconSource: "qrc:/qt/qml/Studious/resources/icons/realtime.svg"
                title: qsTr("实时翻译")
                description: qsTr("盯住一块屏幕，字幕和对话边说边翻，只翻变化的部分。")
                tags: qsTr("锁定区域　·　增量翻译　·　悬浮字幕")
                objectName: "cardLive"
                footer: qsTr("点一下开始框选")
                kind: "live"
                onActivated: page.launchRequested("live")
            }
        }

        // ---------------------------------------------------------- 状态行
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            spacing: Theme.s1
            visible: backend.statusText.length > 0 || backend.statusWarning.length > 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s3

                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: backend.statusText
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: backend.statusIsError ? Theme.danger : Theme.inkVariant
                }

                // 翻译在跑时给一个出口：点一下立刻停，不必等这一单跑完。
                AppButton {
                    Layout.alignment: Qt.AlignVCenter
                    visible: backend.running
                    text: qsTr("取消")
                    onClicked: backend.cancel()
                }
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
                spacing: Theme.s3

                // 四个引擎 + 各自的 detail 在小窗口下一行排不下。以前是硬挤，
                // 右边直接被窗口边缘裁掉；现在给状态点一条横向滚动带（默认看不出滚动条）。
                // detail 本身也有 detailMaxWidth 上限，滚动条只是兜底。
                ScrollView {
                    id: statusScroll
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 26
                    clip: true

                    ScrollBar.horizontal.policy: ScrollBar.AsNeeded
                    ScrollBar.vertical.policy: ScrollBar.AlwaysOff

                    Row {
                        spacing: Theme.s4

                        Repeater {
                            model: backend.engineStatuses

                            delegate: Item {
                                id: statusSlot
                                required property var modelData

                                // 用 implicitWidth/Height 而不是 width/height：定位器
                                // （Row）与 ScrollView 都是靠隐式尺寸算内容的，
                                // 写死 height 会让 contentHeight 变成 0。
                                implicitWidth: statusDot.implicitWidth
                                implicitHeight: 24

                                StatusDot {
                                    id: statusDot
                                    anchors.verticalCenter: parent.verticalCenter
                                    // 裸 id（llm-cloud）是给配置看的，这里换成人话。
                                    label: page.statusEngineName(statusSlot.modelData.id !== undefined
                                                                 ? String(statusSlot.modelData.id) : "")
                                    detail: statusSlot.modelData.detail !== undefined ? statusSlot.modelData.detail : ""
                                    // 1120 宽下四个引擎一行排不下，detail 收窄到 150，
                                    // 让第四项也完整露出来；真放不下还有外面的横向滚动兜底。
                                    detailMaxWidth: 120
                                    // 颜色分三层：装好了且这条请求能走 = 绿；装好了但当前源语言/目标语言
                                    // 走不通（比如本地 MT 不支持自动检测源语言）= 黄；缺资产 = 红。
                                    state: statusSlot.modelData.state === "ready"
                                           ? (statusSlot.modelData.ready ? 0 : 1)
                                           : (statusSlot.modelData.state === "missing-assets" ? 2 : 1)
                                }
                            }
                        }
                    }
                }

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
        // 「自动检测源语言」关掉后「自动」那一项会从列表里消失，索引得重新落一次。
        function onPrefsChanged() { srcCombo.syncIndex() }
        function onEngineIdChanged() { engineSeg.current = backend.engineId }
    }
}
