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
