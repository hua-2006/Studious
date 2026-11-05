import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Studious

// 快捷键页。产品要求：键位一律留空；平台层未接入时整页置灰并说明原因。
Item {
    id: page

    readonly property var rows: [
        { group: "截图翻译", action: "区域截图翻译", scope: "全局" },
        { group: "截图翻译", action: "窗口翻译", scope: "全局" },
        { group: "截图翻译", action: "全屏翻译", scope: "全局" },
        { group: "截图翻译", action: "重复上次区域", scope: "全局" },
        { group: "剪贴板", action: "翻译剪贴板", scope: "全局" },
        { group: "剪贴板", action: "译文写回剪贴板", scope: "应用内" },
        { group: "窗口 / 进程", action: "显示 / 隐藏主窗口", scope: "全局" },
        { group: "窗口 / 进程", action: "取消当前翻译", scope: "应用内" },
        { group: "窗口 / 进程", action: "锁定浮层", scope: "应用内" },
        { group: "窗口 / 进程", action: "退出", scope: "全局" }
    ]

    // 每行四列的固定宽度，保证表头与数据对齐。
    readonly property int colAction: 260
    readonly property int colKey: 170
    readonly property int colScope: 100

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s6
        spacing: Theme.s3

        Text {
            text: "快捷键"
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeDisplay
            font.weight: Font.Bold
            color: Theme.ink
        }

        Text {
            Layout.fillWidth: true
            text: "怎么触发。每行四要素:功能 / 键位 / 作用域 / 状态。截图与选区能力尚未接入,键位一律留空。"
            wrapMode: Text.WordWrap
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            color: Theme.inkVariant
        }

        // 平台能力提示
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: warnText.implicitHeight + Theme.s3 * 2
            radius: Theme.rSm
            color: Theme.cautionContainer

            Text {
                id: warnText
                anchors.fill: parent
                anchors.margins: Theme.s3
                text: "当前平台尚未接入全局热键;录制功能置灰,键位无法设置。"
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.caution
            }
        }

        // 整页没有平台层，一个标记代表整张表（不要每行都放，太吵）。
        Stub { }

        SectionCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "场景快捷键"

            // 表头
            RowLayout {
                Layout.fillWidth: true
                spacing: 0

                Text {
                    Layout.preferredWidth: page.colAction
                    text: "功能"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.outline
                }
                Text {
                    Layout.preferredWidth: page.colKey
                    text: "键位"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.outline
                }
                Text {
                    Layout.preferredWidth: page.colScope
                    text: "作用域"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.outline
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "状态"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.outline
                }
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.outlineVariant
            }

            Repeater {
                model: page.rows

                delegate: RowLayout {
                    id: hotkeyRow
                    required property var modelData

                    Layout.fillWidth: true
                    spacing: 0

                    Text {
                        Layout.preferredWidth: page.colAction
                        text: hotkeyRow.modelData.action
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.ink
                    }
                    Text {
                        Layout.preferredWidth: page.colKey
                        text: "未设置"
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.outline
                    }
                    Text {
                        Layout.preferredWidth: page.colScope
                        text: hotkeyRow.modelData.scope
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }
                    Item { Layout.fillWidth: true }

                    AppButton {
                        text: "录制"
                        variant: "text"
                        buttonEnabled: false
                    }
                    AppButton {
                        text: "清除"
                        variant: "text"
                        buttonEnabled: false
                    }
                }
            }
        }
    }
}
