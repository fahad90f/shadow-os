// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// "Label: value" line for information panels. Values are selectable so they
// can be copied (addresses, serial numbers, versions).
RowLayout {
    id: root

    property string label: ""
    property string value: ""
    property bool mono: false
    property real labelWidth: 160

    spacing: Theme.spaceMd
    Layout.fillWidth: true

    Text {
        Layout.preferredWidth: root.labelWidth
        Layout.alignment: Qt.AlignTop
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        elide: Text.ElideRight
    }
    TextEdit {
        Layout.fillWidth: true
        text: root.value === "" ? "—" : root.value
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        color: Theme.textPrimary
        selectionColor: Theme.accentFillStrong
        selectedTextColor: Theme.textPrimary
        font.family: root.mono ? Theme.fontMono : Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
    }
}
