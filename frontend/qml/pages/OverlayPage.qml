import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

// 浮层页：截屏翻译浮层里「拖框那一下」的外观与操作习惯。
//
// 边界划在这里：
//   · 本页只管放大镜、压暗、方向键微调、复制后延迟关闭 —— 都是浮层自己的事；
//   · 不透明度 / 字号 / 保留原文 / 自动复制这些是「浮层里显示什么」，
//     归「设置 · 截图」那一栏，两处入口别放重复的开关；
//   · 截图热键归「快捷键」页。
// 所以副标题一上来就把另外两页点名，省得用户在本页翻半天。
Item {

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
                    text: qsTr("截屏翻译浮层的外观与操作习惯。不透明度、字号、自动复制这些在「设置 · 截图」里。")
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

                Text {
                    Layout.fillWidth: true
                    text: qsTr("截图热键在「快捷键」页；不透明度 / 字号 / 保留原文在「设置 · 截图」。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                }
            }

            Item { Layout.preferredHeight: Theme.s6 }
        }
    }
}
