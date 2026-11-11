// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// An NxRow for lists whose rows come from a Repeater.
//
// NxRow decides whether to draw its separator by asking whether it is the
// first child of its parent — which a Repeater's first delegate never is
// (the Repeater itself comes first), so that row would get a stray line at
// the top of the card. Here the NxRow is the first child of its own wrapper
// and the separator is drawn according to 'first'.
Item {
    id: root

    default property alias trailing: row.trailing
    property alias icon: row.icon
    property alias iconColor: row.iconColor
    property alias title: row.title
    property alias subtitle: row.subtitle
    property alias value: row.value
    property alias monoValue: row.monoValue
    property bool first: false

    Layout.fillWidth: true
    implicitHeight: row.implicitHeight

    NxRow {
        id: row
        anchors.left: parent.left
        anchors.right: parent.right
    }
    Rectangle {
        visible: !root.first
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.spaceLg
        height: 1
        color: Theme.borderSubtle
    }
}
