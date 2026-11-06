import QtQuick
import QtQuick.Controls.Basic
import Studious

// 统一外观的下拉框。理由同 AppTextField：Basic 样式的默认背景会掉进系统深色。
ComboBox {
    id: combo

    implicitHeight: 34
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    contentItem: Text {
        leftPadding: Theme.s2 + 2
        rightPadding: Theme.s2 + 8
        text: combo.displayText
        font: combo.font
        color: Theme.ink
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Theme.rSm
        color: Theme.surface
        border.width: 1
        border.color: combo.activeFocus ? Theme.primary : Theme.outlineVariant
    }

    indicator: Text {
        x: combo.width - width - Theme.s2
        y: (combo.height - height) / 2
        text: "\u25BE"
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.inkVariant
    }

    // 列表项必须自己画。
    // Basic 样式的默认 delegate 走 QPalette —— 而 app 的 palette 是启动时按
    // 浅色写死的一份，用户切到深色主题后不会跟着变；反过来 palette 是深色时，
    // 浅色主题下就会出现「选中项黑底白字、其余项白字看不见」。
    // 这里所有颜色都取 Theme 令牌，跟主题联动。
    delegate: ItemDelegate {
        id: comboItem
        required property int index
        required property var modelData

        width: combo.width
        height: 34
        highlighted: combo.highlightedIndex === index
        hoverEnabled: true

        contentItem: Text {
            leftPadding: Theme.s2 + 2
            rightPadding: Theme.s2 + 2
            text: combo.textRole.length > 0
                  ? comboItem.modelData[combo.textRole]
                  : comboItem.modelData
            font: combo.font
            color: comboItem.highlighted ? Theme.primaryContainerText : Theme.ink
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: Theme.rSm
            color: comboItem.highlighted
                   ? Theme.primaryContainer
                   : (comboItem.hovered ? Theme.surfaceHigh : "transparent")
        }
    }

    popup: Popup {
        y: combo.height + 2
        width: combo.width
        implicitHeight: Math.min(contentItem.implicitHeight + Theme.s2, 280)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: combo.popup.visible ? combo.delegateModel : null
            currentIndex: combo.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }

        background: Rectangle {
            radius: Theme.rSm
            color: Theme.surface
            border.width: 1
            border.color: Theme.outlineVariant
        }
    }
}
