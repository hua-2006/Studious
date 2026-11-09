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

    // 「− 值 +」现在只有一份：components/NumStepper.qml（设置页与这里共用）。
    // 契约：只发 modified()，不自己改 value，避免打断 value: backend.x 的绑定。

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
                    hint: qsTr("越大越暗；框选时只有选区露出来。")
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
                    Layout.fillWidth: true
                    text: qsTr("边界颜色跟着强调色走。")
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
                hint: qsTr("小窗口、截图翻译、实时翻译各自的开关，和浮层里「设置」里的是同一套。")

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

                // 三组控件只有一份实现（qml/overlay/OverlaySettingsGroups.qml）：
                // 浮层里那颗齿轮面板实例化的是同一个组件，两边文案与后端绑定不会再漂。
                OverlaySettingsGroups {
                    Layout.fillWidth: true
                    mode: page.funcTab
                }

            Item { Layout.preferredHeight: Theme.s6 }
            }
        }
    }
}
