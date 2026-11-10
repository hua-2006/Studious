import QtQuick
import QtQuick.Layouts
import Studious

// 新手引导：启动时在窗口正中弹一张 4 页卡片。
//
// 为什么做成卡片而不是独立窗口：引导要盖住整个界面又不能抢焦点，
// 独立窗口在无边框主窗上还得跟着移动/缩放，麻烦且容易错位。
//
// 文案长度两种语言差很多（中文短、英文长），所以卡片宽度有上限 560、
// 高度按内容撑，超过可用高度就让正文滚动 —— 不允许出现字被裁掉。
Item {
    id: root

    // 由外部控制：Main.qml 启动时按 backend.onboardingEnabled 打开。
    property bool open: false
    property int page: 0

    readonly property int pageCount: 4
    // 窄窗口下别贴边：左右各留 32。
    readonly property int cardWidth: Math.min(width - Theme.s7 * 2, 560)
    readonly property int maxCardHeight: height - Theme.s6 * 2

    // 卡片里除正文以外的固定开销：标题行 + 按钮行 + 上下内边距 + 两处行距。
    readonly property int cardChrome: headerRow.implicitHeight + footerRow.implicitHeight
                                      + Theme.s5 * 2 + Theme.s4 * 2
    // 正文可用高度。Flickable 自己不会撑高，必须显式给；给多了卡片会超出窗口，
    // 给少了（默认 0）只剩标题行 —— 中文长句和英文长句都要能装下，所以按内容取小。
    readonly property int contentCap: Math.max(96, maxCardHeight - cardChrome)

    signal finished()      // 走完 4 页 / 点了「开始使用」
    signal dismissed()     // 点了「不再提示」
    signal openConfig()    // 最后一页想去引擎配置

    function next() {
        if (page < pageCount - 1)
            page += 1
        else
            root.finished()
    }

    function back() {
        if (page > 0)
            page -= 1
    }

    function show() {
        page = 0
        open = true
        card.opacity = 0
        card.scale = 0.96
        appear.restart()
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    z: 120
    Behavior on opacity { NumberAnimation { duration: Theme.durBase; easing.type: Easing.OutCubic } }

    // 星星：字体自带 ★ / ☆，不用 emoji（emoji 会走彩色字体，和界面不是一套）。
    function starCount(filled, total) {
        let s = ""
        for (let i = 0; i < total; i++)
            s += i < filled ? "★" : "☆"
        return s
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: card; property: "opacity"; from: 0; to: 1; duration: Theme.durBase; easing.type: Easing.OutCubic }
        NumberAnimation { target: card; property: "scale"; from: 0.96; to: 1; duration: Theme.durBase; easing.type: Easing.OutBack }
    }

    // ---------------- 遮罩 ----------------
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: root.open ? 0.28 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durBase } }
    }

    MouseArea {
        anchors.fill: parent
        // 吞掉所有点击：引导期间不该误触到下面的界面。
        onClicked: {}
    }

    // 悬浮也要拦：MouseArea 只挡点击，hover 不靠 grab，底下的入口卡片照样会亮描边。
    // blocking 让下面的 HoverHandler / MouseArea 收不到悬浮（拿不到 hovered）。
    // 不用 enabled:false 停用底层 —— 那会把 hovered 冻在当前值，鼠标离开也不恢复。
    HoverHandler {
        blocking: true
    }

    // ---------------- 卡片 ----------------
    Rectangle {
        id: card

        anchors.centerIn: parent
        width: root.cardWidth
        height: Math.min(body.implicitHeight + Theme.s5 * 2, root.maxCardHeight)
        radius: Theme.rMd
        color: Theme.surfaceHigh
        border.width: 1
        border.color: Theme.outlineVariant
        opacity: 0

        ColumnLayout {
            id: body

            anchors.fill: parent
            anchors.margins: Theme.s5
            spacing: Theme.s4

            // ---- 标题行 ----
            RowLayout {
                id: headerRow
                Layout.fillWidth: true
                spacing: Theme.s3

                Text {
                    Layout.fillWidth: true
                    text: root.page === 0 ? qsTr("欢迎使用 Studious")
                        : root.page === 1 ? qsTr("四种翻译引擎，各有各的用处")
                        : root.page === 2 ? qsTr("怎么选")
                                          : qsTr("可以开始了")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSection + 3
                    font.weight: Font.DemiBold
                    color: Theme.ink
                }

                Text {
                    text: qsTr("%1 / %2").arg(root.page + 1).arg(root.pageCount)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }
            }

            // ---- 正文：装不下就滚 ----
            Flickable {
                id: scroller

                Layout.fillWidth: true
                Layout.fillHeight: false
                // Flickable 的 implicitHeight 默认是 0，靠它撑高会让卡片只剩标题行。
                implicitHeight: Math.min(pages.implicitHeight, root.contentCap)
                contentWidth: width
                contentHeight: pages.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                flickableDirection: Flickable.VerticalFlick

                Column {
                    id: pages

                    width: scroller.width
                    spacing: Theme.s4

                    // ---- 第 1 页：欢迎 ----
                    Column {
                        width: parent.width
                        spacing: Theme.s3
                        visible: root.page === 0

                        Text {
                            width: parent.width
                            text: qsTr("截图、划词、实时翻译，把屏幕上的任何文字变成你看得懂的语言。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            color: Theme.inkVariant
                        }

                        Text {
                            width: parent.width
                            text: qsTr("版本 %1 · %2").arg(appVersion).arg(appCodename)
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }

                        Text {
                            width: parent.width
                            text: qsTr("这份向导只出现一次；以后想再看，可以在「设置 → 通用」里打开「启动时显示新手引导」。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }
                    }

                    // ---- 第 2 页：四种引擎 ----
                    Column {
                        width: parent.width
                        spacing: Theme.s3
                        visible: root.page === 1

                        Repeater {
                            model: [
                                {
                                    "name": qsTr("Web 在线"),
                                    "text": qsTr("用公开的网页翻译接口，不用配置、不用下载，开箱就能用。")
                                },
                                {
                                    "name": qsTr("云端 LLM"),
                                    "text": qsTr("调你自己的云端大模型（OpenAI 兼容接口），填服务地址、模型名和 Key 即可。")
                                },
                                {
                                    "name": qsTr("本地 LLM"),
                                    "text": qsTr("模型跑在你自己电脑上，不联网，翻译内容不出本机。")
                                },
                                {
                                    "name": qsTr("本地 MT"),
                                    "text": qsTr("专门的机器翻译小模型，体积小、出字快，适合字幕和口语。")
                                }
                            ]

                            delegate: Column {
                                required property var modelData
                                width: pages.width
                                spacing: Theme.s1

                                Text {
                                    text: modelData.name
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: Font.DemiBold
                                    color: Theme.ink
                                }

                                Text {
                                    width: parent.width
                                    text: modelData.text
                                    wrapMode: Text.WordWrap
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    color: Theme.inkVariant
                                }
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: note2.implicitHeight + Theme.s3 * 2
                            radius: Theme.rSm
                            color: Theme.cautionContainer

                            Text {
                                id: note2
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: Theme.s3
                                text: qsTr("本地 LLM 和本地 MT 要先下载资产（模型 / 运行时），在「设置 → 资产」里下；没下完之前可以先用 Web 或云端 LLM。")
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.caution
                            }
                        }
                    }

                    // ---- 第 3 页：对比 ----
                    Column {
                        width: parent.width
                        spacing: Theme.s2
                        visible: root.page === 2

                        // 表头
                        Row {
                            width: parent.width
                            spacing: Theme.s3

                            Text {
                                width: 196
                                text: qsTr("引擎")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }
                            Text {
                                width: 96
                                text: qsTr("准确度")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }
                            Text {
                                width: 96
                                text: qsTr("速度")
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }
                        }

                        Rectangle { width: parent.width; height: 1; color: Theme.outlineVariant }

                        Repeater {
                            model: [
                                {
                                    "name": qsTr("Web 在线"),
                                    "note": qsTr("速度看网速"),
                                    "accuracy": 3,
                                    "speed": 3
                                },
                                {
                                    "name": qsTr("云端 LLM"),
                                    "note": qsTr("看服务商和模型"),
                                    "accuracy": 5,
                                    "speed": 2
                                },
                                {
                                    "name": qsTr("本地 LLM"),
                                    "note": qsTr("看模型大小和硬件"),
                                    "accuracy": 4,
                                    "speed": 4
                                },
                                {
                                    "name": qsTr("本地 MT"),
                                    "note": qsTr("够快，但只适合日常句子"),
                                    "accuracy": 1,
                                    "speed": 5
                                }
                            ]

                            delegate: Column {
                                required property var modelData
                                width: pages.width
                                spacing: Theme.s2

                                Row {
                                    width: parent.width
                                    spacing: Theme.s3

                                    Column {
                                        width: 196
                                        spacing: 1
                                        Text {
                                            text: modelData.name
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeBody
                                            color: Theme.ink
                                        }
                                        Text {
                                            width: parent.width
                                            text: modelData.note
                                            wrapMode: Text.WordWrap
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeTiny
                                            color: Theme.inkVariant
                                        }
                                    }

                                    Text {
                                        width: 96
                                        text: root.starCount(modelData.accuracy, 5)
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.ink
                                    }

                                    Text {
                                        width: 96
                                        text: root.starCount(modelData.speed, 5)
                                        font.pixelSize: Theme.fontSizeSmall
                                        color: Theme.ink
                                    }
                                }

                                Rectangle { width: parent.width; height: 1; color: Theme.outlineVariant; opacity: 0.6 }
                            }
                        }

                        Text {
                            width: parent.width
                            text: qsTr("想省事用 Web；要质量用云端 LLM；不想联网用本地 LLM；要实时字幕用本地 MT。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }
                    }

                    // ---- 第 4 页：结束 ----
                    Column {
                        width: parent.width
                        spacing: Theme.s3
                        visible: root.page === 3

                        Text {
                            width: parent.width
                            text: qsTr("先选好源语言和目标语言，再从首页任一入口开始：小窗口随手翻、截图框选翻、实时盯屏翻。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            color: Theme.inkVariant
                        }

                        Text {
                            width: parent.width
                            text: qsTr("卡住了可以在「设置 → 通用」重新打开这份向导，或去「设置 → 日志」看看发生了什么。")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.inkVariant
                        }
                    }
                }
            }

            // ---- 底部：圆点 + 按钮 ----
            RowLayout {
                id: footerRow
                Layout.fillWidth: true
                spacing: Theme.s2

                Row {
                    spacing: Theme.s1

                    Repeater {
                        model: root.pageCount

                        delegate: Rectangle {
                            required property int index
                            width: 6
                            height: 6
                            radius: 3
                            color: index === root.page ? Theme.primary : Theme.outlineVariant
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    visible: root.page > 0
                    text: qsTr("上一步")
                    variant: "text"
                    onClicked: root.back()
                }

                AppButton {
                    visible: root.page === 0
                    text: qsTr("不再提示")
                    variant: "text"
                    onClicked: root.dismissed()
                }

                AppButton {
                    // 最后一页顺带给一个直接去配置的出口：看完就想配引擎的人不用自己找。
                    visible: root.page === root.pageCount - 1
                    text: qsTr("去配置引擎")
                    variant: "outline"
                    onClicked: root.openConfig()
                }

                AppButton {
                    text: root.page === root.pageCount - 1 ? qsTr("开始使用") : qsTr("下一步")
                    variant: "primary"
                    onClicked: root.next()
                }
            }
        }
    }
}
