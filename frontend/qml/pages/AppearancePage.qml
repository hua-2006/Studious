import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

// 外观页：主题 / 字体 / 主窗口 / 语言显示名。
Item {
    id: page

    // 强调色预置：8 个常用 seed（第一个是默认紫，和 Theme.defaultSeed 一致）。
    readonly property var accentPresets: [
        { color: "#6750A4", name: qsTr("紫罗兰（默认）") },
        { color: "#7D5260", name: qsTr("玫瑰") },
        { color: "#00658F", name: qsTr("蓝") },
        { color: "#2E7D32", name: qsTr("绿") },
        { color: "#B26A00", name: qsTr("琥珀") },
        { color: "#8E244D", name: qsTr("酒红") },
        { color: "#4A5C92", name: qsTr("靛蓝") },
        { color: "#5D4037", name: qsTr("棕") }
    ]

    // 只是输入框的校验态，不是配色状态：真值永远在 backend.accentColor。
    property bool accentInvalid: false

    // 当前强调色是否等于某个预置色（大小写不敏感，后端已规范化成大写）。
    function isCurrentAccent(value) {
        return backend.accentColor.toLowerCase() === String(value).toLowerCase()
    }

    // 提交输入框里的 "#RRGGBB"：合法就写后端，非法就标红并保持原色。
    function commitAccent() {
        var text = accentField.text.trim()
        // 允许省略 #：用户常直接粘 6750A4。
        if (text.length === 6 && text.charAt(0) !== "#") {
            text = "#" + text
        }
        if (/^#[0-9a-fA-F]{6}$/.test(text) && Qt.color(text).valid) {
            backend.accentColor = text
            accentField.text = backend.accentColor
            page.accentInvalid = false
        } else {
            page.accentInvalid = true
        }
    }

    // 启动时把落盘的强调色灌进 Theme 单例；之后跟随后端的 accentColorChanged。
    // Main.qml 也会做同样的事，这里不持有任何配色状态，重复调用是幂等的。
    Component.onCompleted: Theme.applySeed(backend.accentColor)

    Connections {
        target: backend
        function onAccentColorChanged() {
            page.accentInvalid = false
            accentField.text = backend.accentColor
            Theme.applySeed(backend.accentColor)
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.s4

            ColumnLayout {
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                Layout.topMargin: Theme.s4
                spacing: Theme.s2

                Text {
                    text: qsTr("外观")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Font.Bold
                    color: Theme.ink
                }
                Text {
                    text: qsTr("长什么样。")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.inkVariant
                }
            }

            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("主题")

                FieldRow {
                    label: qsTr("配色")
                    Segmented {
                        items: [
                            { code: "system", label: qsTr("跟随系统") },
                            { code: "light", label: qsTr("浅色") },
                            { code: "dark", label: qsTr("深色") }
                        ]
                        current: backend.themeMode
                        onSelected: function (code) { backend.themeMode = code }
                    }
                }

                FieldRow {
                    label: qsTr("强调色")

                    // 预置色板：当前色套一圈外环，点一下直接写后端。
                    RowLayout {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Theme.s2

                        Repeater {
                            model: page.accentPresets

                            delegate: Rectangle {
                                id: swatch
                                required property var modelData

                                readonly property bool selected: page.isCurrentAccent(modelData.color)

                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: swatch.selected
                                              ? Theme.ink
                                              : (swatchHover.hovered ? Theme.outline : "transparent")

                                Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 20
                                    height: 20
                                    radius: width / 2
                                    color: swatch.modelData.color
                                    border.width: 1
                                    border.color: Theme.outlineVariant
                                }

                                HoverHandler { id: swatchHover }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.accentColor = swatch.modelData.color
                                }
                            }
                        }
                    }

                    // 自定义色：失焦或回车提交，非法值标红但不动配色。
                    AppTextField {
                        id: accentField
                        Layout.preferredWidth: 116
                        Layout.alignment: Qt.AlignVCenter
                        placeholderText: "#6750A4"
                        maximumLength: 9
                        color: page.accentInvalid ? Theme.danger : Theme.ink

                        background: Rectangle {
                            radius: Theme.rSm
                            color: Theme.surface
                            border.width: 1
                            border.color: page.accentInvalid
                                          ? Theme.danger
                                          : (accentField.activeFocus ? Theme.primary
                                                                     : Theme.outlineVariant)
                        }

                        onAccepted: page.commitAccent()
                        onActiveFocusChanged: {
                            if (activeFocus) {
                                page.accentInvalid = false
                            } else {
                                page.commitAccent()
                            }
                        }

                        Component.onCompleted: text = backend.accentColor
                    }

                    AppButton {
                        text: qsTr("应用")
                        variant: "tonal"
                        Layout.alignment: Qt.AlignVCenter
                        onClicked: page.commitAccent()
                    }
                }
            }

            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("字体")

                FieldRow {
                    label: qsTr("界面字体")
                    Stub { }
                    AppTextField {
                        Layout.fillWidth: true
                        text: Theme.fontUi
                        font.family: Theme.fontUi
                    }
                }
                FieldRow {
                    label: qsTr("译文字体")
                    Stub { }
                    AppTextField {
                        Layout.fillWidth: true
                        // 译文用的是内容字体（霞鹜文楷），别显示成界面字体。
                        text: Theme.fontContent
                        font.family: Theme.fontContent
                    }
                }
                FieldRow {
                    label: qsTr("字号缩放")

                    // 裸 Slider 走 Basic 默认色（深色下是灰黑），这里显式自绘轨道与滑块，
                    // 并用 backend.fontScale 真正驱动 Theme.fontScale。
                    Slider {
                        id: scaleSlider
                        Layout.preferredWidth: 220
                        from: 80
                        to: 150
                        stepSize: 5
                        value: backend.fontScale

                        onMoved: backend.fontScale = Math.round(value)

                        // 后端值变化（切页 / 重置）时同步回来。
                        Connections {
                            target: backend
                            function onFontScaleChanged() { scaleSlider.value = backend.fontScale }
                        }

                        background: Rectangle {
                            x: scaleSlider.leftPadding
                            y: scaleSlider.topPadding + scaleSlider.availableHeight / 2 - height / 2
                            width: scaleSlider.availableWidth
                            height: 4
                            radius: height / 2
                            color: Theme.surfaceHighest

                            Rectangle {
                                width: scaleSlider.visualPosition * parent.width
                                height: parent.height
                                radius: height / 2
                                color: Theme.primary
                            }
                        }

                        handle: Rectangle {
                            x: scaleSlider.leftPadding + scaleSlider.visualPosition
                               * (scaleSlider.availableWidth - width)
                            y: scaleSlider.topPadding + scaleSlider.availableHeight / 2 - height / 2
                            width: 18
                            height: 18
                            radius: width / 2
                            color: Theme.primary
                            border.width: 2
                            border.color: Theme.surface
                        }
                    }

                    Text {
                        text: backend.fontScale + "%"
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.ink
                    }
                }
            }

            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("主窗口")
                Stub { }

                FieldRow {
                    label: qsTr("布局密度")
                    Stub { }
                    Segmented {
                        items: [
                            { code: "compact", label: qsTr("紧凑") },
                            { code: "standard", label: qsTr("标准") }
                        ]
                        current: "standard"
                    }
                }
                FieldRow {
                    label: qsTr("状态栏信息密度")
                    Stub { }
                    Segmented {
                        items: [
                            { code: "minimal", label: "minimal" },
                            { code: "normal", label: "normal" },
                            { code: "detailed", label: "detailed" }
                        ]
                        current: "normal"
                    }
                }
            }

            SectionCard {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("语言显示名")

                FieldRow {
                    label: qsTr("显示方式")
                    Stub { }
                    Segmented {
                        items: [
                            { code: "short", label: qsTr("简短(zh / en)") },
                            { code: "full", label: qsTr("完整(中文(简体) / English)") }
                        ]
                        current: "full"
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("预览:中文(简体) · English")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.inkVariant
                }
            }

            Item { Layout.preferredHeight: Theme.s6 }
        }
    }
}
