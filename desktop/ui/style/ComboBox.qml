// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.nexus.ui

T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: Theme.spaceMd
    rightPadding: Theme.spaceMd + Theme.iconSmall + Theme.spaceSm
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    delegate: ItemDelegate {
        required property var model
        required property int index

        width: ListView.view.width
        text: model[control.textRole]
        highlighted: control.highlightedIndex === index
        font.weight: control.currentIndex === index ? Theme.weightSemibold : Theme.weightRegular
    }

    indicator: NxIcon {
        x: control.mirrored ? control.padding : control.width - width - Theme.spaceMd
        y: control.topPadding + (control.availableHeight - height) / 2
        name: "chevron-down"
        size: Theme.iconSmall
        color: control.enabled ? Theme.textSecondary : Theme.textDisabled
    }

    contentItem: T.TextField {
        leftPadding: 0
        rightPadding: 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        selectionColor: Theme.accentFillStrong
        selectedTextColor: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        font: control.font
    }

    background: Rectangle {
        implicitWidth: 160
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSm
        color: Theme.bgInput
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        border.width: control.visualFocus || control.popup.visible ? Theme.focusRing : Theme.borderWidth
        border.color: control.visualFocus || control.popup.visible ? Theme.accent
            : control.hovered ? Theme.borderStrong : Theme.border
    }

    popup: T.Popup {
        y: control.height + 4
        width: control.width
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: Theme.spaceXs

        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 320)
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: ScrollIndicator { }
        }

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionInstant }
        }

        background: Rectangle {
            color: Theme.bgOverlay
            radius: Theme.radiusMd
            border.width: Theme.borderWidth
            border.color: Theme.border
            // See Popup.qml: presses on the popup never reach the content beneath it.
            TapHandler {
                acceptedButtons: Qt.AllButtons
                gesturePolicy: TapHandler.ReleaseWithinBounds
            }
        }
    }
}
