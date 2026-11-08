import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

// 浮层页：三个浮层（小窗口 / 截图翻译 / 实时翻译）的外观与操作习惯。
//
// 边界划在这里：
//   · 上半页是三个浮层共用的：放大镜、遮罩压暗、方向键微调；
//   · 下半页「按功能设置」用分段切换，分别放小窗口 / 截图翻译 / 实时翻译
//     各自的开关 —— 这些值和浮层里那颗「设置」打开的面板是同一份后端属性，
//     改哪边都一样，不存在两套；
//   · 截图 / 实时 / 小窗的热键归「快捷键」页。
Item {
    id: page

    // 「按功能设置」当前选中的入口：mini / capture / live。
    property string funcTab: "mini"

    objectName: "overlayPage"

    // 取证钩子（--ui-shot 的调用只支持零参方法）。
    function showFuncMini() { funcTab = "mini" }
    function showFuncCapture() { funcTab = "capture" }
    function showFuncLive() { funcTab = "live" }

    // 「− 值 +」：跟设置页截图分区同一个形状。
    // 组件在 SettingsPage.qml 里是页内 component，外面 import 不到，这里照抄一份。
    // 与那份的唯一差别：点击只发 modified()，不自己改 value ——
    // value 直接绑到 backend 属性上，自己去赋值会把绑定打断，切页回来就显示旧值了。
    component NumStepper: RowLayout {
        id: stepper

        property int from: 0
        property int to: 100
        property int step: 1
        property int value: 0
        property string unit: ""

        signal modified(int value)

        function clampTo(v) {
            return Math.max(stepper.from, Math.min(stepper.to, v));
        }

        spacing: Theme.s2

        AppButton {
            text: "\u2212"
            buttonEnabled: stepper.enabled && stepper.value > stepper.from
            onClicked: stepper.modified(stepper.clampTo(stepper.value - stepper.step))
        }

        Text {
            Layout.preferredWidth: 66
            horizontalAlignment: Text.AlignHCenter
            text: stepper.value + stepper.unit
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeBody
            color: Theme.ink
        }

        AppButton {
            text: "+"
            buttonEnabled: stepper.enabled && stepper.value < stepper.to
            onClicked: stepper.modified(stepper.clampTo(stepper.value + stepper.step))
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.s4

            // ------------------------------------------------------------ 页头
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                Layout.topMargin: Theme.s5
                spacing: Theme.s2

                Text {
                    text: qsTr("浮层")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Font.Bold
                    color: Theme.ink
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("三个浮层共用的观感在上半页；每个入口各自的开关在下面「按功能设置」里切换。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.inkVariant
                }
            }

            // ------------------------------------------------------------ 放大镜
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("放大镜")
                hint: qsTr("拖框时贴在光标旁边，用来对齐和取色；松手就收起。")

                FieldRow {
                    label: qsTr("拖框时显示放大镜")
                    AppToggle {
                        Layout.alignment: Qt.AlignVCenter
                        checked: backend.overlayMagnifier
                        onToggled: {
                            backend.overlayMagnifier = checked
                            // 回灌：开关内部先改了自己的 checked，绑定已被打断；
                            // 按后端实际值拉回来，界面才不会跟设置打架。
                            checked = backend.overlayMagnifier
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("关掉就只剩十字光标，对机器压力最小。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                FieldRow {
                    label: qsTr("放大镜位置")

                    Segmented {
                        enabled: backend.overlayMagnifier
                        opacity: enabled ? 1.0 : 0.45
                        items: [
                            { code: "bottomRight", label: qsTr("右下") },
                            { code: "bottomLeft", label: qsTr("左下") },
                            { code: "topRight", label: qsTr("右上") },
                            { code: "topLeft", label: qsTr("左上") }
                        ]
                        current: backend.magnifierCorner
                        onSelected: function (code) { backend.magnifierCorner = code }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("贴着光标那一侧放；靠边放不下时会自动翻到对面。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                FieldRow {
                    label: qsTr("放大倍率")
                    NumStepper {
                        from: 2
                        to: 8
                        step: 1
                        unit: "×"
                        value: backend.overlayZoom
                        enabled: backend.overlayMagnifier
                        opacity: enabled ? 1.0 : 0.45
                        onModified: function (v) { backend.overlayZoom = v }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("倍数越高看得越细，视野也越小。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }
            }

            // ------------------------------------------------------------ 遮罩
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("遮罩")
                hint: qsTr("选区之外压多暗。")

                FieldRow {
                    label: qsTr("压暗程度")
                    NumStepper {
                        from: 20
                        to: 80
                        step: 5
                        unit: "%"
                        value: backend.overlayDim
                        onModified: function (v) { backend.overlayDim = v }
                    }
                }

                Text {
                    Layout.leftMargin: 132 + Theme.s3
                    Layout.fillWidth: true
                    text: qsTr("越大越暗；框选时只有选区露出来。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("边框颜色跟随强调色，不单独暴露。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                }
            }

            // ------------------------------------------------------------ 操作
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("操作")
                hint: qsTr("框好之后怎么用手，而不是用鼠标继续磨。")

                FieldRow {
                    label: qsTr("方向键微调选区")
                    AppToggle {
                        Layout.alignment: Qt.AlignVCenter
                        checked: backend.overlayArrowNudge
                        onToggled: {
                            backend.overlayArrowNudge = checked
                            checked = backend.overlayArrowNudge
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("选中后按方向键微调 1px，按住 Shift 10px。")
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("截图热键在「快捷键」页；三个入口各自的开关都在下面那张卡里。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                }
            }

            // ------------------------------------------------------------ 按功能
            // 三个浮层各自的设置集中在这里：分段切换，值就是浮层里那颗「设置」
            // 打开的面板里那几项，改哪边都一样（同一个后端属性）。
            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("按功能设置")
                hint: qsTr("小窗口、截图翻译、实时翻译各自的开关；和浮层里那颗「设置」是同一份值。")

                Segmented {
                    Layout.fillWidth: true
                    items: [
                        { code: "mini", label: qsTr("小窗口") },
                        { code: "capture", label: qsTr("截图翻译") },
                        { code: "live", label: qsTr("实时翻译") }
                    ]
                    current: page.funcTab
                    onSelected: function (code) { page.funcTab = code }
                }

                // ---- 小窗口 ----
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.s2
                    spacing: Theme.s3
                    visible: page.funcTab === "mini"

                    FieldRow {
                        label: qsTr("固定在最前")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.miniAlwaysOnTop
                            onToggled: {
                                backend.miniAlwaysOnTop = checked
                                checked = backend.miniAlwaysOnTop
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("关掉就跟随普通窗口，被别的程序盖住。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }

                    FieldRow {
                        label: qsTr("译文带原文")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.showSource
                            onToggled: {
                                backend.showSource = checked
                                checked = backend.showSource
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("每段译文上方多一行小字原文，方便对照。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }
                }

                // ---- 截图翻译 ----
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.s2
                    spacing: Theme.s3
                    visible: page.funcTab === "capture"

                    FieldRow {
                        label: qsTr("选完直接翻译")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.captureAutoRun
                            onToggled: {
                                backend.captureAutoRun = checked
                                checked = backend.captureAutoRun
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("关掉就是二次确认：框完自己点工具栏上的「翻译」。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }

                    FieldRow {
                        label: qsTr("保留原文标注")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.overlayKeepOriginal
                            onToggled: {
                                backend.overlayKeepOriginal = checked
                                checked = backend.overlayKeepOriginal
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("译文条上方留一条薄薄的原文，便于核对。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }

                    FieldRow {
                        label: qsTr("翻译完自动复制")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.captureAutoCopy
                            onToggled: {
                                backend.captureAutoCopy = checked
                                checked = backend.captureAutoCopy
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("出结果就把整段译文放进剪贴板。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }

                    FieldRow {
                        label: qsTr("译文不透明度")
                        NumStepper {
                            from: 40
                            to: 100
                            step: 5
                            unit: "%"
                            value: backend.overlayOpacity
                            onModified: function (v) { backend.overlayOpacity = v }
                        }
                    }

                    Text {
                        Layout.leftMargin: 132 + Theme.s3
                        Layout.fillWidth: true
                        text: qsTr("越低越能看见底下的原图。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    FieldRow {
                        label: qsTr("译文字号")
                        NumStepper {
                            from: 10
                            to: 20
                            step: 1
                            unit: qsTr(" px")
                            value: backend.overlayFontSize
                            onModified: function (v) { backend.overlayFontSize = v }
                        }
                    }

                    Text {
                        Layout.leftMargin: 132 + Theme.s3
                        Layout.fillWidth: true
                        text: qsTr("按行高自动收一点，行太窄时不会再缩小到看不清。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    FieldRow {
                        label: qsTr("复制后延迟关闭")
                        NumStepper {
                            from: 0
                            to: 10
                            step: 1
                            unit: qsTr(" 秒")
                            value: backend.overlayAutoHideDelay
                            onModified: function (v) { backend.overlayAutoHideDelay = v }
                        }
                    }

                    Text {
                        Layout.leftMargin: 132 + Theme.s3
                        Layout.fillWidth: true
                        text: qsTr("0 = 复制完立刻收浮层。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }

                // ---- 实时翻译 ----
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.s2
                    spacing: Theme.s3
                    visible: page.funcTab === "live"

                    FieldRow {
                        label: qsTr("刷新间隔")
                        NumStepper {
                            from: 400
                            to: 1500
                            step: 100
                            unit: qsTr(" 毫秒")
                            value: backend.liveIntervalMs
                            // 点击只发 modified，回写交给后端 —— 自己改 value 会打断上面这根绑定。
                            onModified: function (v) { backend.liveIntervalMs = v }
                        }
                    }

                    Text {
                        Layout.leftMargin: 132 + Theme.s3
                        Layout.fillWidth: true
                        text: qsTr("每帧之间等多久。太小会把引擎压满，太大会看不出是实时的。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    FieldRow {
                        label: qsTr("只在内容变化时翻译")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.liveOnlyOnChange
                            onToggled: {
                                backend.liveOnlyOnChange = checked
                                // 回灌：开关内部先改了自己的 checked，绑定已被打断；
                                // 按后端实际值拉回来，界面才不会跟设置打架。
                                checked = backend.liveOnlyOnChange
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("画面没变就不重翻。关掉会每帧重发一次请求，只适合验证引擎。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeTiny
                            color: Theme.inkVariant
                        }
                    }

                    FieldRow {
                        label: qsTr("区域底色")
                        NumStepper {
                            from: 0
                            to: 100
                            step: 10
                            unit: "%"
                            value: backend.overlayBackdrop
                            onModified: function (v) { backend.overlayBackdrop = v }
                        }
                    }

                    Text {
                        Layout.leftMargin: 132 + Theme.s3
                        Layout.fillWidth: true
                        text: qsTr("整块选区铺一层底色的浓度；0 = 不铺底，只留译文。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }

                    FieldRow {
                        label: qsTr("译文不透明度")
                        NumStepper {
                            from: 40
                            to: 100
                            step: 5
                            unit: "%"
                            value: backend.overlayOpacity
                            onModified: function (v) { backend.overlayOpacity = v }
                        }
                    }

                    FieldRow {
                        label: qsTr("译文字号")
                        NumStepper {
                            from: 10
                            to: 20
                            step: 1
                            unit: qsTr(" px")
                            value: backend.overlayFontSize
                            onModified: function (v) { backend.overlayFontSize = v }
                        }
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.s6 }
        }
    }
}
