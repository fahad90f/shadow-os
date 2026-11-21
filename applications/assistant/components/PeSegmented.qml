// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import org.shadow.assistant

// A row of mutually exclusive choices (radio semantics, arrow keys move).
Rectangle {
    id: seg
    property var model: []          // [{ value, label, enabled? }]
    property string value: ""
    property string accessibleName: ""
    signal picked(string value)

    implicitHeight: 34
    implicitWidth: row.implicitWidth + 6
    radius: height / 2
    color: Pe.sunken
    border.width: 1
    border.color: Pe.line
    Accessible.role: Accessible.Grouping
    Accessible.name: accessibleName

    // the chosen option (-1: none of them)
    readonly property int current: {
        for (let i = 0; i < model.length; ++i)
            if (model[i].value === value)
                return i
        return -1
    }
    // The one Tab stop of the group: the chosen option, else the first
    // that can be chosen (radio-group keyboard behaviour).
    readonly property int tabStop: {
        if (current >= 0)
            return current
        for (let i = 0; i < model.length; ++i)
            if (model[i].enabled !== false)
                return i
        return -1
    }
    // Arrow keys walk through the options from the chosen one; the keyboard
    // focus follows the choice.
    function step(dir: int) {
        const from = current >= 0 ? current : (dir > 0 ? -1 : seg.model.length)
        for (let i = from + dir; i >= 0 && i < seg.model.length; i += dir) {
            if (seg.model[i].enabled !== false) {
                seg.picked(seg.model[i].value)
                const it = options.itemAt(i)
                if (it)
                    it.forceActiveFocus(Qt.TabFocusReason)
                return
            }
        }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            id: options
            model: seg.model
            delegate: T.Button {
                id: opt
                required property var modelData
                required property int index
                readonly property bool on: seg.value === modelData.value
                implicitHeight: 28
                implicitWidth: label.implicitWidth + 28
                enabled: modelData.enabled !== false
                hoverEnabled: true
                focusPolicy: index === seg.tabStop ? Qt.TabFocus : Qt.NoFocus
                checkable: true
                checked: on
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.label
                onClicked: seg.picked(modelData.value)
                Keys.onLeftPressed: seg.step(-1)
                Keys.onRightPressed: seg.step(1)
                background: Rectangle {
                    radius: height / 2
                    color: opt.on ? (Pe.dark ? Pe.surfaceHi : "#FFFFFF") : opt.hovered ? Pe.alpha(Pe.ink, 0.05) : "transparent"
                    border.width: opt.on ? 1 : 0
                    border.color: Pe.lineStrong
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: height / 2
                        color: "transparent"
                        border.width: 2
                        border.color: Pe.cyan
                        visible: opt.visualFocus
                    }
                }
                contentItem: Text {
                    id: label
                    textFormat: Text.PlainText
                    text: opt.modelData.label
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: Pe.sans
                    font.pixelSize: Pe.small
                    font.weight: opt.on ? Font.DemiBold : Font.Medium
                    color: !opt.enabled ? Pe.inkFaint : opt.on ? Pe.ink : Pe.inkSoft
                }
            }
        }
    }
}
