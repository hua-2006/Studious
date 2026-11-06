import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
// 资产「从文件导入」与「迁移目录」要选本地文件 / 目录。
import QtQuick.Dialogs
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把 TextField / SpinBox 做成模板里的 .input。
import QtQuick.Controls.Basic
// SVG 描边色是写死的，图标要跟着调色板变色只能靠 MultiEffect 重新上色。
import QtQuick.Effects
import Studious

// 设置页：按 docs/TempUI/index.html 的 settings 段复刻。
//
// 结构是「页头 + 左侧二级导航 + 右侧内容区」：
//   页头（标题 / 副标题）
//   └ RowLayout
//       ├ 左栏 190px：七个分区项，选中态高亮
//       └ 右栏 ScrollView：StackLayout 一次只显示当前分区那张卡
//
// 开关与输入框全部直连 AppBackend 的 Q_PROPERTY（值落 QSettings，写完即生效）；
// 页面本身不存任何状态，除了「导入哪个资产」这种一次性的交互上下文。
Item {
    id: page

    // 启动时实际生效的界面语言。与 backend.uiLanguage 不一致 = 改过但没重启，
    // 语言那一行会因此冒出「重启软件」按钮。
    // （赋值在下面的 Component.onCompleted 里，QML 不允许写两个 onCompleted。）
    property string appliedLanguage: "system"

    // 把「跟随系统」解析成具体语言再比。
    // 系统本来就是中文时，「中文」→「跟随系统」结果一模一样，不该提示重启；
    // 反过来也一样。
    function effectiveLanguage(code) {
        return code === "system" ? backend.systemLanguage : code
    }

    // FieldRow 的默认标签列宽；行内提示文字靠它对齐到控件列。
    readonly property int labelWidth: 132
    // 输入框 / 按钮统一行高，同排不齐高会显得歪。
    readonly property int hField: 34

    // ---- 左侧二级导航 ----
    readonly property int navWidth: 190
    // 当前选中的分区下标，同时驱动左栏高亮与右栏 StackLayout。
    property int currentSection: 0

    // 七个分区：图标用 qrc 里的 SVG，文字保持中文。
    readonly property var sections: [
        { icon: "qrc:/qt/qml/Studious/resources/icons/settings.svg",  label: qsTr("通用") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/translate.svg", label: qsTr("翻译行为") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/database.svg",  label: qsTr("缓存") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/globe.svg",     label: qsTr("网络") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/download.svg",  label: qsTr("资产") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/folder.svg",    label: qsTr("数据") },
        { icon: "qrc:/qt/qml/Studious/resources/icons/info.svg",      label: qsTr("关于") }
    ]

    // ---- 资产表列宽（表头与数据行共用，保证上下对齐）----
    readonly property int colId: 260
    readonly property int colState: 84
    readonly property int colSize: 96
    readonly property int colAction: 96

    // ---- 界面语言 ----
    readonly property var langItems: [
        { code: "system", label: qsTr("跟随系统") },
        { code: "zh", label: qsTr("中文") },
        { code: "en", label: "English" }
    ]

    // ---- 关闭主窗口时的行为 ----
    readonly property var closeItems: [
        { code: "exit", label: qsTr("退出程序") },
        { code: "tray", label: qsTr("最小化到托盘") }
    ]

    // ---- 资产：分类标签 + 过滤 ----
    // 分类由后端清单决定（AppBackend::assets 的 category 字段）：
    //   llm = qwen 模型 + llama.cpp 运行时；mt = opus-mt 语言对 + onnxruntime 运行时。
    property string assetTab: "all"

    // 「从文件导入」的中间状态：资产已经选好了，文件选择框还没回结果。
    property string pendingImportId: ""

    readonly property var filteredAssets: {
        var out = []
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (page.assetTab === "all" || list[i].category === page.assetTab)
                out.push(list[i])
        }
        return out
    }

    // 已安装资产的总占用。后端给的是字节，这里格式化。
    readonly property string installedTotalText: {
        var total = 0
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (list[i].installed)
                total += list[i].sizeBytes
        }
        return page.humanBytes(total)
    }

    // 资产状态码 → 界面文案（后端只给码，文案归界面管）。
    function stateLabel(code) {
        if (code === "installed")
            return qsTr("已安装")
        if (code === "corrupted")
            return qsTr("损坏")
        return qsTr("未安装")
    }

    function humanBytes(bytes) {
        if (bytes < 1024)
            return bytes + " B"
        var kb = bytes / 1024
        if (kb < 1024)
            return kb.toFixed(1) + " KB"
        var mb = kb / 1024
        if (mb < 1024)
            return mb.toFixed(1) + " MB"
        return (mb / 1024).toFixed(1) + " GB"
    }

    // 文件/目录选择器给的是 url（file:///C:/...），后端要的是本地路径。
    // 直接传 url 给 QString 形参会得到带 scheme 的字符串，QFileInfo 认不出来。
    function localPath(url) {
        var s = "" + url
        if (s.indexOf("file:///") === 0)
            return decodeURIComponent(s.substring(8))
        if (s.indexOf("file://") === 0)
            return "//" + decodeURIComponent(s.substring(7))
        return s
    }

    // 外部跳转入口：配置页「本地 MT → 下载 / 导入」直接落到资产分区的 mt 分类。
    // kind 取 "mt" / "llm" / "all"；空串按 "all" 处理。
    function openAssets(kind) {
        page.currentSection = 4
        page.assetTab = (kind === "mt" || kind === "llm") ? kind : "all"
        // 资产卡不折叠，切过去就在最上面；把右栏滚回顶部，免得停在上一分区
        // 留下的滚动位置上、看到一片空白。
        // ScrollBar.position 是只读的，得改内容项的 contentY / 或 contentItem 的 origin。
        if (contentScroll.contentItem)
            contentScroll.contentItem.contentY = 0
    }

    // ---- 安装根目录树（等宽块里逐行排版，├─ / └─ 是纯文本）----
    // 分支名后面补空格对齐到同一列说明；中文是全角，视觉上会差半个字宽，不用较真。
    readonly property string dirTree: [
        // 根目录用后端给的真实路径，迁移过的用户也能看到自己那份。
        backend.assetInstallRoot + "/",
        "├─ models/                         " + qsTr("MT 语言对 + LLM 模型（.gguf）"),
        "│  ├─ mt.opus.en-zh/",
        "│  ├─ mt.opus.zh-en/",
        "│  └─ model.qwen2.5-3b-instruct.q4km/",
        "├─ runtime/                        " + qsTr("运行时"),
        "│  └─ runtime.llama.cpu.win-x64/",
        "├─ downloads/                      " + qsTr(".part 临时文件（可续传）"),
        "├─ dist/                           " + qsTr("打包产物"),
        "└─ logs/                           " + qsTr("运行日志")
    ].join("\n")

    // 后端不会主动推缓存统计，进页面时拉一次，数字才有内容。
    Component.onCompleted: {
        appliedLanguage = backend.uiLanguage
        backend.refreshCacheStats()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.s4

        // ------------------------------------------------------ 页头
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s6
            Layout.rightMargin: Theme.s6
            Layout.topMargin: Theme.s6
            spacing: Theme.s2

            Text {
                text: qsTr("设置")
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeDisplay
                font.weight: Font.Bold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("管「应用怎么跑」：通用行为、翻译行为、缓存、网络、资产、数据。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.s6
            Layout.rightMargin: Theme.s6
            Layout.bottomMargin: Theme.s6
            spacing: Theme.s6

            // -------------------------------------------------- 左栏：二级导航
            Column {
                Layout.preferredWidth: page.navWidth
                Layout.fillHeight: true
                spacing: Theme.s1

                Repeater {
                    model: page.sections

                    delegate: Rectangle {
                        id: navItem
                        required property int index
                        required property var modelData

                        // Column 不管子项尺寸，宽高必须自己给。
                        width: page.navWidth
                        height: 40
                        radius: Theme.rMd
                        color: page.currentSection === navItem.index
                               ? Theme.primaryContainer
                               : (navHover.hovered ? Theme.surfaceHigh : "transparent")

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }

                        Row {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: Theme.s3
                            anchors.rightMargin: Theme.s2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.s2

                            Image {
                                anchors.verticalCenter: parent.verticalCenter
                                source: navItem.modelData.icon
                                sourceSize.width: 16
                                sourceSize.height: 16
                                // SVG 描边写死成深紫，换主题不会变，这里按调色板重新上色。
                                layer.enabled: true
                                layer.effect: MultiEffect {
                                    colorizationColor: page.currentSection === navItem.index
                                                       ? Theme.primaryContainerText
                                                       : Theme.inkVariant
                                    colorization: 1.0
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 16 - Theme.s2
                                text: navItem.modelData.label
                                elide: Text.ElideRight
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: page.currentSection === navItem.index
                                             ? Font.DemiBold : Font.Normal
                                color: page.currentSection === navItem.index
                                       ? Theme.primaryContainerText : Theme.inkVariant
                            }
                        }

                        HoverHandler { id: navHover }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: page.currentSection = navItem.index
                        }
                    }
                }
            }

            // -------------------------------------------------- 右栏：内容区
            ScrollView {
                id: contentScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: parent.width
                    spacing: 0

                    // 一次只显示一张卡；currentIndex 直接绑左栏选中项。
                    StackLayout {
                        Layout.fillWidth: true
                        currentIndex: page.currentSection

                        // -------------------------------------------- 通用
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("通用")
                                hint: qsTr("应用行为，与翻译链路无关。")

                                // 界面语言：Qt 没有运行时换语言机制，改完要重启。
                                FieldRow {
                                    label: qsTr("界面语言")

                                    Segmented {
                                        Layout.alignment: Qt.AlignVCenter
                                        items: page.langItems
                                        current: backend.uiLanguage
                                        onSelected: function (code) { backend.uiLanguage = code }
                                    }

                                    // 只在「改过了、还没重启」时出现。
                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("重启软件")
                                        variant: "tonal"
                                        visible: page.effectiveLanguage(backend.uiLanguage)
                                                 !== page.effectiveLanguage(page.appliedLanguage)
                                        onClicked: backend.restartApplication()
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                // 开机自启：写 HKCU\...\Run，失败时后端自己回滚并弹状态。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("开机自启")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.autoStart
                                            onToggled: {
                                                backend.autoStart = checked
                                                checked = backend.autoStart // 写注册表失败时后端会回滚，界面跟着拉回
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: backend.autoStart
                                              ? qsTr("已写入当前用户的启动项（HKCU\\...\\Run）。")
                                              : qsTr("程序不会随系统启动。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 最小化到托盘：托盘图标本身装不上时置灰。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("最小化到托盘")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.minimizeToTray
                                            toggleEnabled: backend.trayAvailable
                                            onToggled: {
                                                backend.minimizeToTray = checked
                                                checked = backend.minimizeToTray
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: backend.trayAvailable
                                              ? qsTr("点最小化时收进系统托盘，而不是留在任务栏。")
                                              : qsTr("当前会话装载不了托盘图标。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: backend.trayAvailable ? Theme.inkVariant : Theme.caution
                                    }
                                }

                                // 关闭主窗口时：退出 / 收进托盘（进程留着）。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("关闭时")

                                        Segmented {
                                            Layout.alignment: Qt.AlignVCenter
                                            items: page.closeItems
                                            current: backend.closeToTray ? "tray" : "exit"
                                            onSelected: function (code) {
                                                backend.closeToTray = code === "tray"
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("选「最小化到托盘」后主窗口只是隐藏，进程还在跑；从托盘菜单可以退出。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 单实例：QLocalServer 防多开，第二个实例把请求转给已运行进程。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("单实例运行")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.singleInstance
                                            onToggled: {
                                                backend.singleInstance = checked
                                                checked = backend.singleInstance
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("重复启动时只把已有窗口叫到前面，不再开第二个进程。改完需要重启软件生效。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }
                            }
                        }

                        // -------------------------------------------- 翻译行为
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("翻译行为")
                                hint: qsTr("决定结果怎么表达。")

                                // 同语言直返：源 == 目标时不调引擎，只有灰字提示。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("同语言直返提示")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.sameLangHint
                                            onToggled: {
                                                backend.sameLangHint = checked
                                                checked = backend.sameLangHint
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("源语言与目标语言相同时提示「这是原文」。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 源语言可疑：warning 非空时黄字提示，不覆盖译文。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("源语言可疑警告")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.suspiciousLangWarn
                                            onToggled: {
                                                backend.suspiciousLangWarn = checked
                                                checked = backend.suspiciousLangWarn
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("源语言可疑时给出警告。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 自动检测：关掉后用用户手选的源语言。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("自动检测源语言")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.autoDetectSource
                                            onToggled: {
                                                backend.autoDetectSource = checked
                                                checked = backend.autoDetectSource
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("关掉后源语言必须手选；本地 MT 这类要求明确源语言的引擎会更好用。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }
                            }
                        }

                        // -------------------------------------------- 缓存
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("缓存")
                                hint: qsTr("按 引擎 + 源语言 + 目标语言 + 原文 命中。")

                                headerActions: [
                                    AppButton {
                                        text: qsTr("清空缓存")
                                        variant: "outline"
                                        onClicked: backend.clearCache()
                                    },
                                    AppButton {
                                        text: qsTr("重置统计")
                                        variant: "text"
                                        onClicked: backend.resetCacheStats()
                                    }
                                ]

                                FieldRow {
                                    label: qsTr("启用翻译缓存")

                                    AppToggle {
                                        Layout.alignment: Qt.AlignVCenter
                                        checked: backend.cacheEnabled
                                        onToggled: {
                                            backend.cacheEnabled = checked
                                            checked = backend.cacheEnabled
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                FieldRow {
                                    label: qsTr("条数")

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: backend.cacheEntries
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.ink
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                FieldRow {
                                    label: qsTr("命中 / 未命中")

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: backend.cacheHits + " / " + backend.cacheMisses
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.ink
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                FieldRow {
                                    label: qsTr("数据库路径")

                                    AppTextField {
                                        id: cacheDbField
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: page.hField
                                        readOnly: true
                                        text: "%APPDATA%\\studious\\cache\\translations.db"
                                        selectByMouse: true
                                        color: Theme.ink
                                        selectionColor: Theme.primaryContainer
                                        selectedTextColor: Theme.primaryContainerText
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        leftPadding: Theme.s3
                                        rightPadding: Theme.s3
                                        topPadding: 0
                                        bottomPadding: 0
                                        verticalAlignment: TextInput.AlignVCenter

                                        background: Rectangle {
                                            radius: Theme.rMd
                                            color: Theme.surfaceHighest
                                            border.width: 0
                                        }
                                    }

                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("复制")
                                        variant: "text"
                                        onClicked: {
                                            cacheDbField.selectAll()
                                            cacheDbField.copy()
                                        }
                                    }
                                }
                            }
                        }

                        // -------------------------------------------- 网络
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("网络")
                                hint: qsTr("代理与超时对本程序的外网请求生效。")

                                FieldRow {
                                    label: qsTr("代理")

                                    AppTextField {
                                        id: proxyField
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: page.hField
                                        placeholderText: "http://127.0.0.1:7897"
                                        selectByMouse: true
                                        color: Theme.ink
                                        placeholderTextColor: Theme.outline
                                        selectionColor: Theme.primaryContainer
                                        selectedTextColor: Theme.primaryContainerText
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        leftPadding: Theme.s3
                                        rightPadding: Theme.s3
                                        topPadding: 0
                                        bottomPadding: 0
                                        verticalAlignment: TextInput.AlignVCenter

                                        background: Rectangle {
                                            radius: Theme.rMd
                                            color: proxyField.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
                                            border.width: 0

                                            Behavior on color { ColorAnimation { duration: Theme.durFast } }
                                        }

                                        // 只在进页面时灌一次值：写成 `text: backend.proxyUrl`
                                        // 会在用户敲第一个字时把绑定断掉，虽然不崩，
                                        // 但后端改值（比如另存配置）之后界面就再也不同步了。
                                        Component.onCompleted: text = backend.proxyUrl
                                        onEditingFinished: backend.proxyUrl = text
                                    }
                                }

                                // 空串 = 不覆盖环境变量，后端按 HTTPS_PROXY 等回落。
                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("留空 = 跟随系统环境变量（HTTPS_PROXY / HTTP_PROXY / ALL_PROXY）。改完下一次网络请求生效。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }

                                FieldRow {
                                    label: qsTr("超时")

                                    SpinBox {
                                        id: timeoutSpin
                                        Layout.alignment: Qt.AlignVCenter
                                        Layout.preferredWidth: 190
                                        Layout.preferredHeight: page.hField

                                        from: 1000
                                        to: 600000
                                        stepSize: 1000
                                        value: backend.httpTimeoutMs
                                        onValueModified: backend.httpTimeoutMs = value
                                        editable: true
                                        // C locale：不要千分位，「30000」而不是「30,000」。
                                        locale: Qt.locale("C")

                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        palette.text: Theme.ink
                                        palette.highlight: Theme.primaryContainer
                                        palette.highlightedText: Theme.primaryContainerText

                                        contentItem: TextInput {
                                            z: 2
                                            text: timeoutSpin.displayText
                                            clip: width < implicitWidth
                                            padding: 0
                                            leftPadding: Theme.s3
                                            rightPadding: Theme.s2
                                            font: timeoutSpin.font
                                            color: Theme.ink
                                            selectionColor: Theme.primaryContainer
                                            selectedTextColor: Theme.primaryContainerText
                                            horizontalAlignment: Qt.AlignLeft
                                            verticalAlignment: Qt.AlignVCenter
                                            readOnly: !timeoutSpin.editable
                                            validator: timeoutSpin.validator
                                            selectByMouse: true
                                        }

                                        up.indicator: Item {
                                            x: timeoutSpin.width - width
                                            height: timeoutSpin.height / 2
                                            implicitWidth: 24
                                            implicitHeight: 16

                                            Text {
                                                anchors.centerIn: parent
                                                text: "\u25B2"
                                                font.family: Theme.fontUi
                                                font.pixelSize: 7
                                                color: timeoutSpin.up.pressed ? Theme.ink : Theme.inkVariant
                                            }
                                        }

                                        down.indicator: Item {
                                            x: timeoutSpin.width - width
                                            y: timeoutSpin.height / 2
                                            height: timeoutSpin.height / 2
                                            implicitWidth: 24
                                            implicitHeight: 16

                                            Text {
                                                anchors.centerIn: parent
                                                text: "\u25BC"
                                                font.family: Theme.fontUi
                                                font.pixelSize: 7
                                                color: timeoutSpin.down.pressed ? Theme.ink : Theme.inkVariant
                                            }
                                        }

                                        background: Rectangle {
                                            radius: Theme.rMd
                                            color: Theme.surfaceHighest
                                            border.width: 0
                                        }
                                    }

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("毫秒")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("毫秒。只作用于外网请求，本地 llama-server 探测不受影响。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }
                            }
                        }

                        // -------------------------------------------- 资产
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("资产")
                                hint: qsTr("模型 / 运行时管理。分类来自后端清单：LLM 含 qwen 模型与 llama.cpp 运行时，本地 MT 含 OPUS-MT 语言对（ONNX Runtime 随应用内置，不在下载清单里）。")

                                // 分类标签：全部 / LLM / 本地 MT
                                Segmented {
                                    items: [
                                        { code: "all", label: qsTr("全部") },
                                        { code: "llm", label: "LLM" },
                                        { code: "mt", label: qsTr("本地 MT") }
                                    ]
                                    current: page.assetTab
                                    onSelected: function (code) { page.assetTab = code }
                                }

                                // 总占用 + 导入
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s3

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("总占用：%1").arg(page.installedTotalText)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.ink
                                    }

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("（当前分类 %1 项）").arg(page.filteredAssets.length)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    Item { Layout.fillWidth: true }

                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("从文件导入")
                                        variant: "tonal"
                                        onClicked: importPicker.open()
                                    }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("导入的文件会先按资产清单校验 sha256，不匹配直接拒绝。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }

                                // 表头
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s3

                                    Text {
                                        Layout.preferredWidth: page.colId
                                        text: qsTr("资产 id")
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }

                                    Text {
                                        Layout.preferredWidth: page.colState
                                        text: qsTr("状态")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }

                                    Text {
                                        Layout.preferredWidth: page.colSize
                                        text: qsTr("大小")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("路径")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }

                                    Text {
                                        Layout.preferredWidth: page.colAction
                                        Layout.rightMargin: Theme.s2
                                        text: qsTr("操作")
                                        horizontalAlignment: Text.AlignRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }
                                }

                                // 表头下的分隔线
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                // 数据行
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    Text {
                                        Layout.fillWidth: true
                                        visible: page.filteredAssets.length === 0
                                        text: qsTr("该分类下暂无资产")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    Repeater {
                                        model: page.filteredAssets

                                        delegate: RowLayout {
                                            id: assetRow
                                            required property var modelData

                                            // 下载状态：后端 assetProgress 是 id → 百分比（-1 = 未知总量）
                                            readonly property string assetId: assetRow.modelData.id
                                            readonly property bool downloading:
                                                backend.assetProgress.hasOwnProperty(assetRow.assetId)
                                            readonly property int percent:
                                                assetRow.downloading
                                                    ? backend.assetProgress[assetRow.assetId] : -1

                                            Layout.fillWidth: true
                                            spacing: Theme.s3

                                            Text {
                                                Layout.preferredWidth: page.colId
                                                Layout.alignment: Qt.AlignVCenter
                                                text: assetRow.modelData.id
                                                elide: Text.ElideRight
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeBody
                                                color: Theme.ink
                                            }

                                            Item {
                                                Layout.preferredWidth: page.colState
                                                Layout.preferredHeight: 22

                                                Badge {
                                                    anchors.left: parent.left
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    text: page.stateLabel(assetRow.modelData.state)
                                                    filled: assetRow.modelData.installed
                                                }
                                            }

                                            Text {
                                                Layout.preferredWidth: page.colSize
                                                Layout.alignment: Qt.AlignVCenter
                                                text: assetRow.modelData.sizeText
                                                elide: Text.ElideRight
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                Layout.alignment: Qt.AlignVCenter
                                                text: assetRow.modelData.path
                                                elide: Text.ElideMiddle
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }

                                            Item {
                                                Layout.preferredWidth: page.colAction
                                                Layout.preferredHeight: assetRow.implicitHeight

                                                AppButton {
                                                    anchors.right: parent.right
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    variant: "text"

                                                    text: {
                                                        if (assetRow.downloading)
                                                            return assetRow.percent >= 0
                                                                   ? qsTr("下载中 %1%").arg(assetRow.percent)
                                                                   : qsTr("下载中…")
                                                        if (assetRow.modelData.placeholder)
                                                            return qsTr("暂不支持")
                                                        return assetRow.modelData.installed ? qsTr("卸载") : qsTr("下载")
                                                    }

                                                    // 占位资产（清单里 urls 为空，比如还没发布的 Linux 运行时）不给点。
                                                    buttonEnabled: assetRow.downloading
                                                                   || !assetRow.modelData.placeholder

                                                    onClicked: {
                                                        if (assetRow.downloading) {
                                                            backend.cancelAssetDownload(assetRow.assetId)
                                                        } else if (assetRow.modelData.installed) {
                                                            uninstallConfirm.assetId = assetRow.assetId
                                                            uninstallConfirm.open()
                                                        } else {
                                                            backend.downloadAsset(assetRow.assetId)
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // -------------------------------------------- 数据
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("数据")
                                hint: qsTr("安装根目录 / 日志 / 重置。重置会清掉配置与缓存，保留已安装资产。")

                                // ---- 安装根目录 ----
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.s3

                                        Text {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("安装根目录")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeBody
                                            font.weight: Font.DemiBold
                                            color: Theme.ink
                                        }

                                        Item { Layout.fillWidth: true }

                                        AppButton {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("打开目录")
                                            variant: "outline"
                                            onClicked: backend.openPath(backend.assetInstallRoot)
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: backend.assetInstallRoot
                                        elide: Text.ElideMiddle
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    // 等宽代码块：目录树按行排版，底色走 surface。
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: treeText.implicitHeight + Theme.s4 * 2
                                        radius: Theme.rMd
                                        color: Theme.surface
                                        border.width: 1
                                        border.color: Theme.outlineVariant

                                        Text {
                                            id: treeText
                                            anchors.fill: parent
                                            anchors.margins: Theme.s4
                                            text: page.dirTree
                                            wrapMode: Text.NoWrap
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSizeSmall
                                            lineHeight: 1.35
                                            color: Theme.inkVariant
                                        }
                                    }
                                }

                                // 分隔线
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                // ---- 日志目录 ----
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.s3

                                        Text {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("日志目录")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeBody
                                            font.weight: Font.DemiBold
                                            color: Theme.ink
                                        }

                                        Item { Layout.fillWidth: true }

                                        AppButton {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("打开")
                                            variant: "outline"
                                            onClicked: backend.openPath(backend.assetInstallRoot + "/logs")
                                        }

                                        AppButton {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("清理 30 天前")
                                            variant: "text"
                                            onClicked: backend.clearOldLogs(30)
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: backend.assetInstallRoot + "/logs"
                                        elide: Text.ElideMiddle
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 分隔线
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                // ---- 重置 / 迁移 ----
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s3

                                    Text {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("重置会清掉配置与缓存（保留已装资产）；迁移把整个安装根目录搬到新位置，新目录必须是空目录或不存在，搬完会自动重新扫描资产。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("迁移目录")
                                        variant: "outline"
                                        onClicked: migrateDialog.open()
                                    }

                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("重置应用")
                                        variant: "danger"
                                        onClicked: resetConfirm.open()
                                    }
                                }
                            }
                        }

                        // -------------------------------------------- 关于
                        ColumnLayout {
                            spacing: 0

                            SectionCard {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.s4
                                Layout.bottomMargin: Theme.s4
                                title: qsTr("关于")
                                hint: qsTr("版本 / 第三方组件 / 许可证。")

                                // ---- 版本条 ----
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s4

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.s1

                                        Text {
                                            text: appName
                                            font.family: Theme.fontUi
                                            font.pixelSize: Math.round(Theme.fontSizeDisplay * 0.78)
                                            font.weight: Font.Bold
                                            color: Theme.ink
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("版本 %1 · 提交 %2 · 构建 %3 / Qt %4")
                                                  .arg(appVersion).arg(appCommit).arg(buildArch).arg(qtVersion)
                                            wrapMode: Text.Wrap
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSizeSmall
                                            color: Theme.inkVariant
                                        }
                                    }

                                    // 渠道标记：stable 绿、其它（beta/测试版）琥珀。
                                    // 文案和渠道都来自 version.json，不写死。
                                    Rectangle {
                                        Layout.alignment: Qt.AlignTop
                                        implicitWidth: relLabel.implicitWidth + Theme.s3
                                        implicitHeight: 24
                                        radius: Theme.rFull
                                        color: appChannel === "stable" ? Theme.good : Theme.caution

                                        Text {
                                            id: relLabel
                                            anchors.centerIn: parent
                                            text: appChannelLabel
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            font.weight: Font.DemiBold
                                            color: Theme.surface
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                // ---- 第三方组件表 ----
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s3

                                    Text {
                                        Layout.preferredWidth: 200
                                        text: qsTr("第三方组件")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }
                                    Text {
                                        Layout.preferredWidth: 110
                                        text: qsTr("版本")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("用途")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        font.weight: Font.DemiBold
                                        color: Theme.inkVariant
                                    }
                                    Text {
                                        Layout.preferredWidth: 130
                                        text: qsTr("许可证")
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
                                    model: [
                                        { name: "onnxruntime",  version: "1.31.1", use: qsTr("本地 MT 推理运行时"), license: "Apache-2.0" },
                                        { name: "sentencepiece", version: "0.2.0",  use: qsTr("分词"),               license: "Apache-2.0" },
                                        { name: "monocypher",   version: "4.0.2",  use: qsTr("清单验签 / 加密"),     license: "BSD-2-Clause" },
                                        { name: "miniz",        version: "3.0.2",  use: qsTr("资产 zip 解压"),       license: "MIT" }
                                    ]

                                    delegate: RowLayout {
                                        id: thirdPartyRow
                                        required property var modelData

                                        Layout.fillWidth: true
                                        spacing: Theme.s3

                                        Text {
                                            Layout.preferredWidth: 200
                                            text: thirdPartyRow.modelData.name
                                            elide: Text.ElideRight
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSizeBody
                                            color: Theme.ink
                                        }
                                        Text {
                                            Layout.preferredWidth: 110
                                            text: thirdPartyRow.modelData.version
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSizeBody
                                            color: Theme.inkVariant
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: thirdPartyRow.modelData.use
                                            elide: Text.ElideRight
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeBody
                                            color: Theme.ink
                                        }
                                        Text {
                                            Layout.preferredWidth: 130
                                            text: thirdPartyRow.modelData.license
                                            font.family: Theme.fontMono
                                            font.pixelSize: Theme.fontSizeBody
                                            color: Theme.inkVariant
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                // ---- 底部按钮 ----
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s3

                                    AppButton {
                                        text: qsTr("查看许可证")
                                        variant: "outline"
                                        onClicked: licensePopup.open()
                                    }

                                    AppButton {
                                        text: qsTr("第三方声明")
                                        variant: "text"
                                        onClicked: thirdPartyPopup.open()
                                    }

                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }

                    // 底部留白，滚到底时最后一张卡片不贴边。
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.s2
                    }
                }
            }
        }
    }

    // ---- 卸载确认：删磁盘文件是不可逆的，先问一句 ----
    Popup {
        id: uninstallConfirm
        property string assetId: ""
        modal: true
        anchors.centerIn: parent
        width: 440
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            spacing: Theme.s3

            Text {
                text: qsTr("卸载资产")
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("确定删除 %1 ？\n文件会被移除，之后可以重新下载。").arg(uninstallConfirm.assetId)
                wrapMode: Text.Wrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("取消")
                    onClicked: uninstallConfirm.close()
                }

                AppButton {
                    text: qsTr("卸载")
                    variant: "danger"
                    onClicked: {
                        backend.uninstallAsset(uninstallConfirm.assetId)
                        uninstallConfirm.close()
                    }
                }
            }
        }
    }

    // ---- 从文件导入：先选资产，再挑本地文件 ----
    Popup {
        id: importPicker
        modal: true
        anchors.centerIn: parent
        width: 520
        height: 460
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.s3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    text: qsTr("从文件导入")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSection
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("关闭")
                    variant: "text"
                    onClicked: importPicker.close()
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("先选资产，再挑本地文件。已安装的也可以覆盖重装。")
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.inkVariant
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.outlineVariant
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                ColumnLayout {
                    width: importPicker.availableWidth
                    spacing: 2

                    Text {
                        Layout.fillWidth: true
                        visible: page.filteredAssets.length === 0
                        text: qsTr("当前分类下没有可选资产。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }

                    Repeater {
                        model: page.filteredAssets

                        delegate: Rectangle {
                            id: importRow
                            required property var modelData

                            Layout.fillWidth: true
                            implicitHeight: 36
                            radius: Theme.rSm
                            color: importHover.hovered ? Theme.surfaceHighest : "transparent"

                            Behavior on color { ColorAnimation { duration: Theme.durFast } }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.s2
                                anchors.rightMargin: Theme.s2
                                spacing: Theme.s2

                                Text {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    text: importRow.modelData.id
                                    elide: Text.ElideRight
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fontSizeBody
                                    color: Theme.ink
                                }

                                Text {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: importRow.modelData.installed
                                    text: qsTr("已安装")
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeTiny
                                    color: Theme.inkVariant
                                }
                            }

                            HoverHandler { id: importHover }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    page.pendingImportId = importRow.modelData.id
                                    importPicker.close()
                                    importFileDialog.open()
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 原生文件选择框：选完把 url 换成本地路径再交给后端。
    FileDialog {
        id: importFileDialog
        title: qsTr("选择资产文件")
        fileMode: FileDialog.OpenFile
        onAccepted: backend.importAssetFromFile(page.pendingImportId,
                                                page.localPath(importFileDialog.selectedFile))
    }

    // ---- 重置确认：清配置 + 清缓存，不可撤销 ----
    Popup {
        id: resetConfirm
        modal: true
        anchors.centerIn: parent
        width: 460
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            spacing: Theme.s3

            Text {
                text: qsTr("重置应用")
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("会清掉全部设置与翻译缓存，已下载的资产保留，随后程序自动重启。")
                wrapMode: Text.Wrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("取消")
                    onClicked: resetConfirm.close()
                }

                AppButton {
                    text: qsTr("确认重置")
                    variant: "danger"
                    onClicked: {
                        resetConfirm.close()
                        backend.resetApplication()
                    }
                }
            }
        }
    }

    // 原生目录选择框：迁移安装根目录。
    FolderDialog {
        id: migrateDialog
        title: qsTr("选择新的安装根目录")
        onAccepted: backend.migrateAssetRoot(page.localPath(migrateDialog.selectedFolder))
    }

    // ---- 许可证正文（与仓库根 LICENSE 一致，MIT）----
    readonly property string mitLicenseText:
        "MIT License\n\n"
        + "Copyright (c) 2026 hua-2006\n\n"
        + "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
        + "of this software and associated documentation files (the \"Software\"), to deal\n"
        + "in the Software without restriction, including without limitation the rights\n"
        + "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
        + "copies of the Software, and to permit persons to whom the Software is\n"
        + "furnished to do so, subject to the following conditions:\n\n"
        + "The above copyright notice and this permission notice shall be included in all\n"
        + "copies or substantial portions of the Software.\n\n"
        + "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
        + "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
        + "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
        + "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
        + "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
        + "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
        + "SOFTWARE.\n"

    // ---- 查看许可证 ----
    Popup {
        id: licensePopup
        modal: true
        anchors.centerIn: parent
        width: 640
        height: 560
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.s3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    text: qsTr("%1 许可证").arg(appName)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSection
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("复制")
                    variant: "text"
                    onClicked: {
                        backend.copyText(page.mitLicenseText)
                    }
                }

                AppButton {
                    text: qsTr("关闭")
                    variant: "text"
                    onClicked: licensePopup.close()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.outlineVariant
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                TextEdit {
                    width: licensePopup.availableWidth
                    text: page.mitLicenseText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.ink
                    selectionColor: Theme.primaryContainer
                    selectedTextColor: Theme.primaryContainerText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }

    // ---- 第三方声明 ----
    Popup {
        id: thirdPartyPopup
        modal: true
        anchors.centerIn: parent
        width: 680
        height: 560
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.s3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    text: qsTr("第三方声明")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSection
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("关闭")
                    variant: "text"
                    onClicked: thirdPartyPopup.close()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.outlineVariant
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                TextEdit {
                    width: thirdPartyPopup.availableWidth
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.inkVariant
                    selectionColor: Theme.primaryContainer
                    selectedTextColor: Theme.primaryContainerText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    text:
                        qsTr("本程序包含以下第三方开源组件，各自遵循其原始许可证：\n\n")
                        + "onnxruntime 1.31.1 — Microsoft — Apache License 2.0\n"
                        + qsTr("  本地 MT（OPUS-MT / Marian）推理运行时\n")
                        + "  https://github.com/microsoft/onnxruntime\n\n"
                        + "sentencepiece 0.2.0 — Google — Apache License 2.0\n"
                        + qsTr("  源文本 / 译文分词（SentencePiece 模型）\n")
                        + "  https://github.com/google/sentencepiece\n\n"
                        + "monocypher 4.0.2 — Loup Vaillant — BSD-2-Clause OR CC0-1.0\n"
                        + qsTr("  资产清单的 ed25519 签名校验\n")
                        + "  https://monocypher.org\n\n"
                        + qsTr("miniz 3.0.2 — Rich Geldreich 等 — MIT / Public Domain\n")
                        + qsTr("  资产 zip 包解压\n")
                        + "  https://github.com/richgel999/miniz\n\n"
                        + qsTr("Qt 6 — The Qt Company — LGPL-3.0（动态链接使用）\n")
                        + "  https://www.qt.io\n\n"
                        + qsTr("字体：\n")
                        + qsTr("  思源黑体 Source Han Sans SC — Adobe — SIL Open Font License 1.1\n")
                        + qsTr("  霞鹜文楷 LXGW WenKai — 落霞孤鹜 — SIL Open Font License 1.1\n\n")
                        + qsTr("以上组件的完整许可证文本随各自的源分发提供。")
                }
            }
        }
    }
}
