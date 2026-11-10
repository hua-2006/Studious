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
//       └ 左栏二级导航
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

    // ---- 历史对话 ----
    // 三个来源各一个库（截屏 / 小窗 / 实时），后端把它们合并、按时间倒序给这里。
    property var historyItems: []
    // 列表只要最近的这些条；库里的老记录靠「记录库」目录自己清理。
    readonly property int historyLimit: 50

    function refreshHistory() {
        historyItems = backend.historyRecords(historyLimit)
    }

    // 切到「缓存」分区就重读一次：刚在别的页翻过一屏的话，回来要能看到新记录。
    onCurrentSectionChanged: {
        sectionHover = -1
        // 换分区一律回到顶部：否则在长分区滚到底再切过去，contentY 还停在
        // 旧位置，短分区就是一片空白（看着像没渲染）。
        if (contentScroll.contentItem)
            contentScroll.contentItem.contentY = 0
        if (currentSection === 2)
            refreshHistory()
    }

    function historyKindLabel(kind) {
        if (kind === "mini")
            return qsTr("小窗翻译")
        if (kind === "live")
            return qsTr("实时翻译")
        return qsTr("截屏翻译")
    }

    // ISO 时间 → 「MM-dd HH:mm」，历史列表里没人想看秒和时区。
    function historyTimeText(iso) {
        const d = new Date(iso)
        if (isNaN(d.getTime()))
            return iso
        const pad = function (n) { return n < 10 ? "0" + n : "" + n }
        return pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes())
    }

    // 摘要：优先显示译文第一行（没有就退回原文），一行放不下就省略。
    function historyPreview(item) {
        const text = (item.translatedText || "").length > 0 ? item.translatedText : item.originalText
        const first = (text || "").split("\n")[0]
        return first.length > 0 ? first : qsTr("（空）")
    }

    // ---- 左侧二级导航 ----
    readonly property int navWidth: 190
    // 当前选中的分区下标，同时驱动左栏高亮与右栏 StackLayout。
    property int currentSection: 0
    // 鼠标停在左栏第几项（-1 = 不在）。整列只有一个 HoverHandler 在算它。
    property int sectionHover: -1

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
    readonly property int colId: 210
    readonly property int colState: 150
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
    //   llm = qwen 模型 + llama.cpp 运行时；mt = opus-mt 语言对 + onnxruntime 运行时；
    //   ocr = PP-OCRv4 的检测 / 识别模型 + 字典（视觉，跟 LLM 权重无关）。
    property string assetTab: "all"

    // 「从文件导入」的中间状态：资产已经选好了，文件选择框还没回结果。
    property string pendingImportId: ""

    // 「全部」栏点导入时那个「装哪一类」的弹窗开着没有。
    property bool importCategoryOpen: false

    readonly property var filteredAssets: {
        var out = []
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (page.assetTab === "all" || list[i].category === page.assetTab)
                out.push(list[i])
        }
        return out
    }

    // 表格实际渲染的行：引擎驱动（runtime）排在前，模型跟在后，
    // 两类之间由 delegate 画一条分割线（来源不同，混排容易看串行）。
    readonly property var assetRows: {
        var list = page.filteredAssets
        var runtimes = []
        var models = []
        for (var i = 0; i < list.length; ++i) {
            if (list[i].type === "runtime")
                runtimes.push(list[i])
            else
                models.push(list[i])
        }
        var out = []
        for (var r = 0; r < runtimes.length; ++r)
            out.push({ entry: runtimes[r], dividerBefore: false })
        for (var m = 0; m < models.length; ++m)
            out.push({ entry: models[m], dividerBefore: runtimes.length > 0 && m === 0 })
        return out
    }

    // 用户自己导入的条目：接在对应分类的清单条目下面，由下面那条分割线隔开；
    // 一条都没有时整段（连分割线）都不显示。"全部"栏里 LLM 和 MT 混排说不清
    // 归属，所以只在两个分类栏里出现。
    readonly property var userRows: {
        if (page.assetTab !== "llm" && page.assetTab !== "mt")
            return []
        var out = []
        var list = backend.userModels
        for (var i = 0; i < list.length; ++i) {
            if (list[i].kind === page.assetTab)
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

    // ---- 从文件导入：先定「装到哪一条」，再挑本地文件 ----
    // 一类只认一个目标：优先清单里没有下载渠道的占位条目，
    // 其次第一个还没装的，最后才落到同类的第一条（覆盖已装的）。
    // 运行时（llama-server / onnxruntime）不在导入范围内，跳过。
    function importTargetFor(category) {
        var list = backend.assets
        var placeholder = ""
        var missing = ""
        var first = ""
        for (var i = 0; i < list.length; ++i) {
            var a = list[i]
            if (a.category !== category || a.type !== "model")
                continue
            if (first === "")
                first = a.id
            if (a.placeholder && placeholder === "")
                placeholder = a.id
            if (!a.installed && missing === "")
                missing = a.id
        }
        if (placeholder !== "")
            return placeholder
        if (missing !== "")
            return missing
        // 都装齐了：挑这一类里最可能要换的那条。
        // MT 换的是某个语言对（拿 en-zh 举例），OCR 换的是识别模型。
        if (category === "ocr" && page.hasAsset("ocr.rec.ppocrv5"))
            return "ocr.rec.ppocrv5"
        if (category === "mt" && page.hasAsset("mt.opus.en-zh"))
            return "mt.opus.en-zh"
        return first
    }

    function hasAsset(id) {
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return true
        }
        return false
    }

    function assetInstalled(id) {
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id === id)
                return list[i].installed === true
        }
        return false
    }

    function beginImport(category) {
        page.importCategoryOpen = false
        // 本地 LLM：用户自己的 gguf 放 models/llm-user，只验文件头，不套清单的 sha256。
        // 哈希校验只针对「线上那份下载」，两种导入不再共用一条路径。
        if (category === "llm") {
            page.openUserModelDialog()
            return
        }
        // 本地 MT：六件套的打包导入还没做，直接说清楚该把目录放哪儿。
        if (category === "mt") {
            backend.notifyMtImportUnavailable()
            return
        }
        page.pendingImportId = page.importTargetFor(category)
        if (page.pendingImportId === "") {
            // 这一类清单里一条都没有：退回逐条挑选的老界面，至少能看到是空的。
            importPicker.open()
            return
        }
        page.openImportDialog()
    }

    // 文件选择框默认打开「这条资产所在的目录」，而不是每次从 C:\ 开始翻。
    // 后端 assets 每项带 path（安装路径），取它的父目录就是这一类的位置。
    function assetFolder(id) {
        var list = backend.assets
        for (var i = 0; i < list.length; ++i) {
            if (list[i].id !== id)
                continue
            var p = "" + (list[i].path || "")
            var cut = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"))
            return cut > 0 ? p.substring(0, cut) : p
        }
        return ""
    }

    function openImportDialog() {
        var folder = page.assetFolder(page.pendingImportId)
        // Qt 6 的 FileDialog 只有 currentFolder（Qt 5 才叫 folder）。
        // 之前写成 folder 会在这一行抛 "Cannot assign to non-existent property"，
        // 整个函数中断、下面的 open() 根本没执行 —— 表现就是「点了没反应」。
        if (folder !== "")
            importFileDialog.currentFolder = "file:///" + encodeURI(folder)
        importFileDialog.open()
    }

    // 用户自己的模型（任意 gguf）：不认清单条目，也不做哈希比对。
    function openUserModelDialog() {
        userModelFileDialog.open()
    }

    // 外部跳转入口：配置页「本地 MT → 下载 / 导入」直接落到资产分区的 mt 分类。
    // kind 取 "mt" / "llm" / "all"；空串按 "all" 处理。
    function openAssets(kind) {
        page.currentSection = 4
        page.assetTab = (kind === "mt" || kind === "llm" || kind === "ocr") ? kind : "all"
        // 资产卡不折叠，切过去就在最上面；把右栏滚回顶部，免得停在上一分区
        // 留下的滚动位置上、看到一片空白。
        // ScrollBar.position 是只读的，得改内容项的 contentY / 或 contentItem 的 origin。
        if (contentScroll.contentItem)
            contentScroll.contentItem.contentY = 0
    }

    // ---- 数据目录树（等宽块里逐行排版，├─ / └─ 是纯文本）----
    // 分支名后面补空格对齐到同一列说明；中文是全角，视觉上会差半个字宽，不用较真。
    readonly property string dirTree: [
        // 根目录用后端给的真实路径，迁移过的用户也能看到自己那份。
        backend.assetInstallRoot + "/",
        "├─ models/                         " + qsTr("模型，按用途分块"),
        "│  ├─ llm/                         " + qsTr("清单里下载的 GGUF"),
        "│  │  └─ model.qwen2.5-3b-instruct.q4km",
        "│  ├─ llm-user/                    " + qsTr("你自己导入的 GGUF（只验文件头）"),
        "│  │  └─ Qwen2.5-0.5B-Instruct-Q4_K_M.gguf",
        "│  ├─ mt/                          " + qsTr("清单里下载的语言包"),
        "│  │  └─ mt.opus.en-zh/",
        "│  ├─ mt-user/                     " + qsTr("你自己放进来的语言包"),
        "│  │  └─ mt.opus.en-zh/            " + qsTr("六件套齐全才认"),
        "│  └─ ocr/                         " + qsTr("OCR 三件套（det / rec / 字典）"),
        "│     └─ run/                      " + qsTr("收拢后的 det.onnx / rec.onnx"),
        "├─ runtime/                        " + qsTr("llama-server 运行时"),
        "│  └─ runtime.llama.cpu.win-x64/",
        "├─ state/                          " + qsTr("导入记录与资产来源"),
        "├─ downloads/                      " + qsTr("下载中的 .part（可续传）"),
        "└─ logs/                           " + qsTr("llama-server 日志")
    ].join("\n")

    // 后端不会主动推缓存统计，进页面时拉一次，数字才有内容。
    Component.onCompleted: {
        appliedLanguage = backend.uiLanguage
        backend.refreshCacheStats()
        refreshHistory()
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
                text: qsTr("管「应用怎么跑」：通用行为、翻译行为、缓存、网络、下载、数据。")
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
            Item {
                Layout.preferredWidth: page.navWidth
                Layout.fillHeight: true

                // 选中块：跟着分区往下滑的胶囊。原来每项各自涂底色，切页时是"闪一下"，
                // 现在整块滑过去，眼睛能看出是从哪一项移到哪一项。
                Rectangle {
                    id: sectionPill
                    width: page.navWidth
                    height: 40
                    radius: Theme.rMd
                    color: Theme.primaryContainer
                    y: page.currentSection * (40 + Theme.s1)
                    visible: false

                    Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

                    Component.onCompleted: visible = true
                }

                // 悬浮块：跟随鼠标的那一项。逐项各挂 HoverHandler 时，鼠标划过去
                // 每一项都会各起一次颜色动画（半透明残影一串，看着像闪烁）；
                // 整列只留一个 handler，坐标一次算完。
                Rectangle {
                    id: sectionHoverPill
                    width: page.navWidth
                    height: 40
                    radius: Theme.rMd
                    color: Theme.surfaceHigh
                    y: Math.max(0, page.sectionHover) * (40 + Theme.s1)
                    opacity: (page.sectionHover >= 0 && page.sectionHover !== page.currentSection) ? 1 : 0

                    Behavior on y { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }

                Column {
                    id: sectionNav
                    width: page.navWidth
                    spacing: Theme.s1

                    // 整列共用一个悬浮检测：鼠标落在哪一行就报哪一行的下标。
                    HoverHandler {
                        id: navHover
                        onPointChanged: page.sectionHover = navHover.hovered
                                                  ? Math.floor(point.position.y / (40 + Theme.s1))
                                                  : -1
                        onHoveredChanged: if (!navHover.hovered) page.sectionHover = -1
                    }

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
                            // 选中与悬浮都由外层那两颗胶囊表示，这里保持透明。
                            color: "transparent"
    
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
                                    Behavior on color { ColorAnimation { duration: 220 } }
                                }
                            }
    
    
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: page.currentSection = navItem.index
                            }
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
                        id: sectionStack
                        Layout.fillWidth: true
                        // StackLayout 默认按**所有**子项里最高的那个撑开 —— 缓存那张卡
                        // （带历史列表）比别的长得多，于是切到短分区后底下多出一大片
                        // 空白，滚到底再切换还会停在越界的 contentY 上、整页看着像空的。
                        // 高度只跟当前项走。
                        // 高度只跟当前项走（StackLayout 没有 currentItem，用 itemAt()）。
                        Layout.preferredHeight: {
                            var it = itemAt(currentIndex)
                            return it ? it.implicitHeight : 0
                        }
                        currentIndex: page.currentSection

                        // 和主界面切页用同一套：淡入 + 8px 上浮。
                        onCurrentIndexChanged: sectionFade.restart()

                        NumberAnimation {
                            id: sectionFade
                            target: sectionStack
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: 380
                            easing.type: Easing.OutCubic
                        }

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

                                // 开机自启：Windows 写 HKCU\...\Run，类 Unix 写
                                // $XDG_CONFIG_HOME/autostart/*.desktop。失败时后端自己回滚并弹状态。
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
                                                checked = backend.autoStart // 写系统自启动项失败时后端会回滚，界面跟着拉回
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: backend.autoStart
                                              ? qsTr("已写入当前用户的自启动项。")
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
                                              : qsTr("现在无法显示托盘图标。")
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
                                        text: qsTr("主窗口只是收起来，软件仍在后台运行；从托盘图标可以重新打开或退出。")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }
                                }

                                // 新手引导：启动时弹那张 4 页的卡片。
                                // 关掉 = 以后不弹；卡片上的「不再提示」也是改这一项。
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s1

                                    FieldRow {
                                        label: qsTr("启动时显示新手引导")

                                        AppToggle {
                                            Layout.alignment: Qt.AlignVCenter
                                            checked: backend.onboardingEnabled
                                            onToggled: function (checked) {
                                                backend.onboardingEnabled = checked
                                                checked = backend.onboardingEnabled
                                            }
                                        }

                                        Item { Layout.fillWidth: true }
                                    }

                                    Text {
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        text: qsTr("打开后下次启动会再弹一次；卡片上的「不再提示」会把它关掉。")
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
                                        text: qsTr("再次打开时只会把已有窗口叫到前面，不会开第二份。改完需要重启生效。")
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
                                hint: qsTr("同一句原文、同样的语言和引擎，才会直接复用已有译文。")

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
                                    label: qsTr("已缓存译文")

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
                                    label: qsTr("复用 / 新翻译")

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
                                    label: qsTr("缓存文件")

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

                                // ---- 历史对话（先立骨架，写入 / 浏览后续接） ----
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: Theme.outlineVariant
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("历史对话")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        font.weight: Font.DemiBold
                                        color: Theme.ink
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                FieldRow {
                                    label: qsTr("保存翻译历史")

                                    AppToggle {
                                        Layout.alignment: Qt.AlignVCenter
                                        checked: backend.historyEnabled
                                        onToggled: {
                                            backend.historyEnabled = checked
                                            checked = backend.historyEnabled
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("开启后翻译完成就自动存档，不用再手动点保存。截屏、小窗、实时各存一个库，下面把三个库按时间合并着列出来。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }

                                FieldRow {
                                    label: qsTr("历史对话目录")

                                    Text {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignVCenter
                                        text: backend.historyDir
                                        elide: Text.ElideMiddle
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.inkVariant
                                    }

                                    AppButton {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("打开目录")
                                        variant: "outline"
                                        onClicked: backend.openHistoryDir()
                                    }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("三个库是 records/capture.db、mini.db、live.db；实时翻译从开启到关闭算一组，写一条记录。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }

                                // ---- 合并后的记录列表 ----
                                RowLayout {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        text: qsTr("最近 %1 条").arg(page.historyItems.length)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    Item { Layout.fillWidth: true }

                                    AppButton {
                                        text: qsTr("刷新")
                                        variant: "outline"
                                        onClicked: page.refreshHistory()
                                    }
                                }

                                // 表头
                                Rectangle {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 26
                                    visible: page.historyItems.length > 0
                                    color: Theme.surfaceLow
                                    radius: Theme.rXs

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.s2
                                        anchors.rightMargin: Theme.s2
                                        spacing: Theme.s2

                                        Text {
                                            Layout.preferredWidth: 92
                                            text: qsTr("时间")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            color: Theme.inkVariant
                                        }
                                        Text {
                                            Layout.preferredWidth: 72
                                            text: qsTr("来源")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            color: Theme.inkVariant
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("摘要")
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            color: Theme.inkVariant
                                        }
                                    }
                                }

                                // 行间距自己管：外层 ColumnLayout 的 spacing 会把这些行撑得很散
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                Repeater {
                                    model: page.historyItems

                                    delegate: Item {
                                        id: historyRow
                                        required property var modelData
                                        Layout.leftMargin: page.labelWidth + Theme.s3
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 34

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: Theme.rXs
                                            color: historyHover.hovered ? Theme.surfaceHigh : "transparent"
                                        }

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: Theme.s2
                                            anchors.rightMargin: Theme.s2
                                            spacing: Theme.s2

                                            Text {
                                                Layout.preferredWidth: 92
                                                text: page.historyTimeText(historyRow.modelData.createdAt)
                                                elide: Text.ElideRight
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }
                                            Text {
                                                Layout.preferredWidth: 72
                                                text: page.historyKindLabel(historyRow.modelData.kind)
                                                elide: Text.ElideRight
                                                font.family: Theme.fontUi
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.ink
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: page.historyPreview(historyRow.modelData)
                                                elide: Text.ElideRight
                                                font.family: Theme.fontUi
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }
                                        }

                                        HoverHandler { id: historyHover }

                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.bottom: parent.bottom
                                            height: 1
                                            color: Theme.outlineVariant
                                            opacity: 0.5
                                        }
                                    }
                                }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    visible: page.historyItems.length === 0
                                    text: backend.historyEnabled
                                              ? qsTr("还没有记录。翻一屏或译一句就会自动出现在这里。")
                                              : qsTr("历史关着，不写记录。打开上面的开关后，翻译完成会自动存档。")
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
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
                                hint: qsTr("只影响本软件联网时使用的代理与等待时间。")

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
                                    text: qsTr("留空表示跟随系统代理设置。改完下次联网生效。")
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
                                    text: qsTr("毫秒。只影响联网请求，本地引擎不受影响。")
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
                                hint: qsTr("在这里下载或导入模型与组件。分三类：本地 AI 模型、翻译模型、图像识别组件。")

                                // 分类标签：全部 / LLM / 本地 MT / 图像识别
                                Segmented {
                                    items: [
                                        { code: "all", label: qsTr("全部") },
                                        { code: "llm", label: "LLM" },
                                        { code: "mt", label: qsTr("本地 MT") },
                                        { code: "ocr", label: qsTr("图像识别") }
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
                                        // 本地 MT 暂不支持导入（原始 opus-mt 包要先转换），
                                        // 在 MT 栏里直接把按钮置灰，点了也只会在状态栏多一句废话。
                                        buttonEnabled: page.assetTab !== "mt"
                                        // 「全部」栏里说不清要装哪一类，先问一句；
                                        // 已经在某个分类栏里就直接挑文件。
                                        onClicked: {
                                            if (page.assetTab === "all")
                                            page.importCategoryOpen = true
                                            else
                                                page.beginImport(page.assetTab)
                                        }
                                    }
                                }

                                Text {
                                    Layout.leftMargin: page.labelWidth + Theme.s3
                                    Layout.fillWidth: true
                                    text: qsTr("自己导入的文件来自本地，没有可信来源，只确认文件在、能读；校验哈希只针对线上下载的那份。")
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
                                        text: qsTr("标识")
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
                                        // 用户导入的行也算内容，别在只有用户模型时显示"还没有内容"。
                                        visible: page.filteredAssets.length === 0
                                                 && page.userRows.length === 0
                                        text: qsTr("这一分类下还没有内容")
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.inkVariant
                                    }

                                    Repeater {
                                        model: page.assetRows

                                        // 每行是一个 ColumnLayout：需要时先画分割线，再放表格行本身。
                                        delegate: ColumnLayout {
                                            id: assetGroup
                                            required property var modelData

                                            // 这一行的资产数据（分割线只是它前面的装饰）。
                                            readonly property var asset: assetGroup.modelData.entry

                                            Layout.fillWidth: true
                                            spacing: Theme.s2

                                            Rectangle {
                                                // 引擎驱动（llama.cpp 运行时）与模型权重之间的分割线。
                                                visible: assetGroup.modelData.dividerBefore
                                                Layout.fillWidth: true
                                                Layout.topMargin: Theme.s2
                                                Layout.bottomMargin: Theme.s2
                                                Layout.preferredHeight: 1
                                                color: Theme.outlineVariant
                                            }

                                            RowLayout {
                                                id: assetRow
                                                property var modelData: assetGroup.asset

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
                                                        // 用户自己导入的资产不做内容校验（没有可信哈希可比），在状态里写明。
                                                        text: assetRow.modelData.installed && assetRow.modelData.origin === "import"
                                                              ? page.stateLabel(assetRow.modelData.state) + " · " + qsTr("用户导入")
                                                              : page.stateLabel(assetRow.modelData.state)
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
                                                            if (assetRow.modelData.placeholder) {
                                                                // 模型条目撤下下载渠道后仍可自行导入：清单里有 id，
                                                                // 导入就是把它装进 models/ 并记成 Import 来源。
                                                                return assetRow.modelData.type === "model"
                                                                       ? qsTr("导入文件")
                                                                       : qsTr("暂不支持")
                                                            }
                                                            return assetRow.modelData.installed ? qsTr("卸载") : qsTr("下载")
                                                        }

                                                        // 占位资产（清单里 urls 为空）：运行时类不给点，
                                                        // 模型类允许「自行导入」。
                                                        buttonEnabled: assetRow.downloading
                                                                       || !assetRow.modelData.placeholder
                                                                       || assetRow.modelData.type === "model"

                                                        onClicked: {
                                                            if (assetRow.downloading) {
                                                                backend.cancelAssetDownload(assetRow.assetId)
                                                            } else if (assetRow.modelData.installed) {
                                                                uninstallConfirm.assetId = assetRow.assetId
                                                                uninstallConfirm.open()
                                                            } else if (assetRow.modelData.placeholder) {
                                                                // 模型类没有下载渠道时，「导入文件」= 把用户自己的
                                                                // 文件放进 models/llm-user，不套清单哈希。
                                                                page.pendingImportId = assetRow.assetId
                                                                if (assetRow.modelData.type === "model")
                                                                    page.openUserModelDialog()
                                                                else
                                                                    page.openImportDialog()
                                                            } else {
                                                                backend.downloadAsset(assetRow.assetId)
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // 用户自己导入的条目：接在清单条目后面，用一条分割线隔开。
                                    // 一条都没有时整段（连分割线）都不占位。
                                    Rectangle {
                                        visible: page.userRows.length > 0
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.s2
                                        Layout.bottomMargin: Theme.s2
                                        Layout.preferredHeight: 1
                                        color: Theme.outlineVariant
                                    }

                                    Repeater {
                                        model: page.userRows

                                        delegate: RowLayout {
                                            id: userRow
                                            required property var modelData

                                            Layout.fillWidth: true
                                            spacing: Theme.s3

                                            Text {
                                                Layout.preferredWidth: page.colId
                                                Layout.alignment: Qt.AlignVCenter
                                                text: userRow.modelData.name
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
                                                    // 用户导入的条目没有清单 id、也没做哈希校验，
                                                    // 用「用户导入」代替「已安装」，一眼能分清来源。
                                                    tone: "good"
                                                    text: qsTr("用户导入")
                                                }
                                            }

                                            Text {
                                                Layout.preferredWidth: page.colSize
                                                Layout.alignment: Qt.AlignVCenter
                                                text: userRow.modelData.sizeText
                                                elide: Text.ElideRight
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                Layout.alignment: Qt.AlignVCenter
                                                text: userRow.modelData.path
                                                elide: Text.ElideMiddle
                                                font.family: Theme.fontMono
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.inkVariant
                                            }

                                            Item {
                                                Layout.preferredWidth: page.colAction
                                                Layout.preferredHeight: userRow.implicitHeight

                                                AppButton {
                                                    anchors.right: parent.right
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    variant: "text"
                                                    text: qsTr("删除")
                                                    onClicked: {
                                                        userModelRemoveConfirm.fileName = userRow.modelData.id
                                                        userModelRemoveConfirm.kind = userRow.modelData.kind
                                                        userModelRemoveConfirm.open()
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
                                hint: qsTr("数据目录 / 日志 / 重置。重置会清掉配置与缓存，保留已安装资产。")

                                // ---- 数据目录 ----
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.s2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.s3

                                        Text {
                                            Layout.alignment: Qt.AlignVCenter
                                            text: qsTr("数据目录")
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
                                        text: qsTr("重置会清掉配置与缓存，已安装的模型保留；迁移会把整个数据目录搬到新位置，目标必须是空目录或不存在。")
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
                                            // 版名来自 version.json 的 codename（可空）：
                                            // 有就显示成「版本 0.9.9 Erstling」，没有就只显示版本号。
                                            text: qsTr("版本 %1 · 提交 %2 · 构建 %3 / Qt %4")
                                                  .arg(appCodename ? appVersion + " " + appCodename
                                                                   : appVersion)
                                                  .arg(appCommit).arg(buildArch).arg(qtVersion)
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
                                    // 版本与许可证以仓库里的实际文件为准：逐条核对过
                                    // backend/third_party/<name>/LICENSE 与版本号文件，
                                    // 不要凭印象填（这里以前把 onnxruntime 写成 Apache-2.0）。
                                    model: [
                                        { name: "onnxruntime",  version: "1.31.1", use: qsTr("本地翻译推理组件"), license: "MIT" },
                                        { name: "sentencepiece", version: "0.2.0",  use: qsTr("分词"),               license: "Apache-2.0" },
                                        { name: "monocypher",   version: "4.0.2",  use: qsTr("下载校验 / 加密"),     license: "BSD-2-Clause" },
                                        { name: "miniz",        version: "11.0.2", use: qsTr("压缩包解压"),       license: "MIT" },
                                        { name: "Qt",           version: "6.12.0", use: qsTr("界面框架（动态链接）"), license: "LGPL-3.0" },
                                        { name: qsTr("内置字体"), version: "—",      use: qsTr("思源黑体 / 霞鹜文楷"), license: "OFL-1.1" }
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
        // OCR 三件套是唯一"删了就彻底不能干活"的资产：det 找框、rec 认字、
        // dict 查表，缺任何一个，截图翻译与实时翻译都会直接报「缺少 OCR 模型」。
        // 模型/运行时的删除都能靠重新下载恢复，且不影响其它功能，所以只对 OCR 加这句。
        readonly property bool isOcr: assetId.startsWith("ocr.")
        // 挂到窗口覆盖层上居中：直接 centerIn 父项时，页内 StackLayout 里的
        // 坐标会把弹窗顶到左上角（出过一次半截在窗外的图）。
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 440
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        ColumnLayout {
            // 宽度绑到 Popup 的可用宽度，否则长文本会把按钮顶出弹窗
            width: uninstallConfirm.availableWidth
            spacing: Theme.s3

            Text {
                text: qsTr("卸载")
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

            Rectangle {
                Layout.fillWidth: true
                visible: uninstallConfirm.isOcr
                implicitHeight: ocrWarning.implicitHeight + Theme.s3 * 2
                radius: Theme.rMd
                color: Theme.dangerContainer
                border.width: 1
                border.color: Theme.danger

                Text {
                    id: ocrWarning
                    anchors.fill: parent
                    anchors.margins: Theme.s3
                    visible: parent.visible
                    text: qsTr("删掉之后就没法识别图像了：截图翻译和实时翻译都会停在「缺少 OCR 模型」，直到你把这三个组件重新下载回来。")
                    wrapMode: Text.Wrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.dangerContainerText
                }
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

    // ---- 删除用户导入的条目：这是真删文件（清单资产那条路只是"卸载后能重下"），单独问一次 ----
    Popup {
        id: userModelRemoveConfirm
        property string fileName: ""
        // "llm" = models/llm-user 里的单文件；"mt" = models/mt-user 里的语言包目录。
        property string kind: "llm"
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        modal: true
        width: 460
        padding: Theme.s5

        background: Rectangle {
            radius: Theme.rLg
            color: Theme.surfaceHigh
            border.width: 1
            border.color: Theme.outlineVariant
        }

        // 宽度必须显式绑到 Popup 的可用宽度：否则 ColumnLayout 会按文本的
        // implicitWidth（一整行不换行）撑开，按钮被顶到弹窗外面去。
        ColumnLayout {
            width: userModelRemoveConfirm.availableWidth
            spacing: Theme.s3

            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: userModelRemoveConfirm.kind === "mt"
                      ? qsTr("删除用户语言包")
                      : qsTr("删除用户模型")
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSection
                font.weight: Font.DemiBold
                color: Theme.ink
            }

            Text {
                Layout.fillWidth: true
                text: userModelRemoveConfirm.kind === "mt"
                      ? qsTr("确定删除「%1」？\n这会把 models/mt-user 里这个语言包目录整个删掉，清单里的资产不受影响。").arg(userModelRemoveConfirm.fileName)
                      : qsTr("确定删除「%1」？\n这会删掉 models/llm-user 里的那份文件，清单里的资产不受影响。").arg(userModelRemoveConfirm.fileName)
                wrapMode: Text.Wrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.inkVariant
            }

            Text {
                Layout.fillWidth: true
                visible: userModelRemoveConfirm.kind !== "mt"
                text: qsTr("如果你还想留着，先从下载目录重新导入一次即可（原始下载文件不会被删）。")
                wrapMode: Text.Wrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.inkVariant
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("取消")
                    onClicked: userModelRemoveConfirm.close()
                }

                AppButton {
                    text: qsTr("删除")
                    variant: "danger"
                    onClicked: {
                        backend.removeUserModel(userModelRemoveConfirm.fileName)
                        userModelRemoveConfirm.close()
                    }
                }
            }
        }
    }

    // ---- 从文件导入（「全部」栏）：先问装哪一类，再直接挑文件 ----
    Popup {
        visible: page.importCategoryOpen
        onClosed: page.importCategoryOpen = false
        id: importCategoryPicker
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
            // 宽度绑到 Popup 的可用宽度，否则长文本会把按钮顶出弹窗
            width: importCategoryPicker.availableWidth
            anchors.fill: parent
            spacing: Theme.s3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.s2

                Text {
                    text: qsTr("要导入哪一类？")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSection
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    text: qsTr("取消")
                    variant: "text"
                    onClicked: page.importCategoryOpen = false
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("选好类别再挑本地文件；装到哪一条写在按钮下面。")
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

            Repeater {
                // mt 暂时禁用：原始 opus-mt 包需要导出 + 量化才能用，转换器还没做，
                // 硬把 PyTorch 包当语言包装进去只会得到"装好了却翻不出话"。
                model: [
                    { code: "llm", label: qsTr("本地 LLM"), disabled: false },
                    { code: "mt", label: qsTr("本地 MT"), disabled: true },
                    { code: "ocr", label: qsTr("OCR"), disabled: false }
                ]

                delegate: ColumnLayout {
                    id: categoryRow
                    required property var modelData
                    readonly property string target: page.importTargetFor(categoryRow.modelData.code)

                    Layout.fillWidth: true
                    spacing: Theme.s1

                    AppButton {
                        Layout.fillWidth: true
                        text: categoryRow.modelData.label
                        variant: "tonal"
                        buttonEnabled: categoryRow.target !== "" && !categoryRow.modelData.disabled
                        onClicked: page.beginImport(categoryRow.modelData.code)
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.s2
                        text: categoryRow.modelData.disabled
                              ? qsTr("暂不支持从文件导入：语言包请在资产页下载")
                              : (categoryRow.target !== ""
                                 ? (page.assetInstalled(categoryRow.target)
                                    ? qsTr("装到 %1（已安装，会覆盖）").arg(categoryRow.target)
                                    : qsTr("装到 %1").arg(categoryRow.target))
                                 : qsTr("这一类还没有可装的条目，先去下载。"))
                        elide: Text.ElideMiddle
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
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
                text: qsTr("先选要装的条目，再挑本地文件；已安装的也可以覆盖。")
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
                        text: qsTr("这一分类下暂时没有可下载的内容。")
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
                                    page.openImportDialog()
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
        title: page.pendingImportId !== ""
               ? qsTr("选择要装进 %1 的文件").arg(page.pendingImportId)
               : qsTr("选择文件")
        fileMode: FileDialog.OpenFile
        onAccepted: backend.importAssetFromFile(page.pendingImportId,
                                                page.localPath(importFileDialog.selectedFile))
    }

    // 用户自己的模型：直接复制进 models/llm-user，跟清单资产完全分开。
    FileDialog {
        id: userModelFileDialog
        title: qsTr("选择 GGUF 模型文件")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("GGUF 模型 (*.gguf)"), qsTr("所有文件 (*)")]
        onAccepted: backend.importLocalModel(selectedFile)
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
            // 宽度绑到 Popup 的可用宽度，否则长文本会把按钮顶出弹窗
            width: resetConfirm.availableWidth
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
                text: qsTr("会清掉全部设置与翻译缓存，已下载的模型保留，随后自动重启。")
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

    // 原生目录选择框：迁移数据目录。
    FolderDialog {
        id: migrateDialog
        title: qsTr("选择新的数据目录")
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
                        qsTr("本程序包含以下第三方开源组件，各自遵循其原始许可证。完整许可证文本随各自源码分发，也能在仓库的 backend/third_party/<组件名>/LICENSE 里读到。\n\n")
                        + "onnxruntime 1.31.1 — Microsoft — MIT License\n"
                        + qsTr("  本地翻译推理组件；随包分发，无需另外下载\n")
                        + "  https://github.com/microsoft/onnxruntime\n\n"
                        + "sentencepiece 0.2.0 — Google — Apache License 2.0\n"
                        + qsTr("  源文本 / 译文分词（SentencePiece 模型）；静态编入，随附其 vendored 子库：\n")
                        + qsTr("  absl（Apache-2.0）、darts-clone（BSD-3-Clause）、esaxx（MIT）、protobuf-lite（BSD-3-Clause）\n")
                        + "  https://github.com/google/sentencepiece\n\n"
                        + "monocypher 4.0.2 — Loup Vaillant — BSD-2-Clause OR CC0-1.0\n"
                        + qsTr("  下载内容的签名校验；本项目按 BSD-2-Clause 使用\n")
                        + "  https://monocypher.org\n\n"
                        + "miniz 11.0.2 — Rich Geldreich / RAD Game Tools / Valve — MIT License\n"
                        + qsTr("  压缩包解压\n")
                        + "  https://github.com/richgel999/miniz\n\n"
                        + "Qt 6.12.0 — The Qt Company — LGPL-3.0\n"
                        + qsTr("  桌面界面框架；以动态链接方式分发，允许替换 Qt 库文件\n")
                        + "  https://www.qt.io\n\n"
                        + qsTr("字体：\n")
                        + qsTr("  思源黑体 Source Han Sans SC — Adobe — SIL Open Font License 1.1\n")
                        + qsTr("  霞鹜文楷 LXGW WenKai — 落霞孤鹜 — SIL Open Font License 1.1\n\n")
                        + qsTr("推断与翻译模型（Qwen2.5、OPUS-MT、PP-OCRv4）不属于本程序，按需单独下载，各自遵循其原始许可证。")
                }
            }
         }
    }

}
