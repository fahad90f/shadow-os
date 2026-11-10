// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// Lock / log out / suspend / restart / power off. Actions that end the
// session ask for confirmation first.
OverlaySurface {
    id: root

    scope: "nexus-session-menu"
    scrimOpacity: 0.7

    // The action waiting for confirmation ("" = choosing).
    property string pending: ""
    onOpenChanged: if (open) pending = ""

    readonly property var actions: [
        { key: "lock", icon: "lock", label: qsTr("Lock"), enabled: session.canLock, confirm: "" },
        { key: "logout", icon: "logout", label: qsTr("Log out"), enabled: true,
          confirm: qsTr("Log out of NEXUS OS? Applications that are still open will be closed.") },
        { key: "suspend", icon: "moon", label: qsTr("Suspend"), enabled: session.canSuspend, confirm: "" },
        { key: "hibernate", icon: "harddrive", label: qsTr("Hibernate"), enabled: session.canHibernate, confirm: "" },
        { key: "reboot", icon: "refresh", label: qsTr("Restart"), enabled: session.canReboot,
          confirm: qsTr("Restart the computer? Unsaved work will be lost.") },
        { key: "poweroff", icon: "power", label: qsTr("Power off"), enabled: session.canPowerOff,
          confirm: qsTr("Power off the computer? Unsaved work will be lost.") },
    ]

    function perform(key) {
        root.hide()
        if (key === "lock") session.lock()
        else if (key === "logout") session.logout()
        else if (key === "suspend") session.suspend()
        else if (key === "hibernate") session.hibernate()
        else if (key === "reboot") session.reboot()
        else if (key === "poweroff") session.powerOff()
    }

    function choose(action) {
        if (action.confirm !== "") pending = action.key
        else perform(action.key)
    }

    function actionFor(key) {
        for (let i = 0; i < actions.length; i++) if (actions[i].key === key) return actions[i]
        return null
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spaceXxl

        // ---- choosing ---------------------------------------------------------
        Row {
            Layout.alignment: Qt.AlignHCenter
            visible: root.pending === ""
            spacing: Theme.spaceXl

            Repeater {
                model: root.actions.filter(a => a.enabled)
                Column {
                    id: entry
                    required property var modelData
                    required property int index
                    spacing: Theme.spaceMd
                    Rectangle {
                        id: disc
                        width: 84; height: 84; radius: 42
                        color: discTap.pressed ? Theme.bgPressed : discHover.hovered || disc.activeFocus ? Theme.bgHover : Theme.bgRaised
                        border.width: discHover.hovered || disc.activeFocus ? 2 : Theme.borderWidth
                        border.color: discHover.hovered || disc.activeFocus ? Theme.accent : Theme.border
                        activeFocusOnTab: true
                        focus: entry.index === 0
                        Accessible.role: Accessible.Button
                        Accessible.name: entry.modelData.label
                        Keys.onReturnPressed: root.choose(entry.modelData)
                        Keys.onSpacePressed: root.choose(entry.modelData)
                        KeyNavigation.right: nextItemInFocusChain(true)
                        KeyNavigation.left: nextItemInFocusChain(false)
                        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
                        NxIcon {
                            anchors.centerIn: parent
                            name: entry.modelData.icon
                            size: 30
                            color: discHover.hovered || disc.activeFocus ? Theme.accent : Theme.textPrimary
                        }
                        HoverHandler { id: discHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { id: discTap; onTapped: root.choose(entry.modelData) }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: entry.modelData.label
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightMedium
                    }
                }
            }
        }

        // ---- confirming ---------------------------------------------------------
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            visible: root.pending !== ""
            implicitWidth: 460
            implicitHeight: confirmColumn.implicitHeight + Theme.spaceXl * 2
            radius: Theme.radiusLg
            color: Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.border
            TapHandler { }

            ColumnLayout {
                id: confirmColumn
                anchors.fill: parent
                anchors.margins: Theme.spaceXl
                spacing: Theme.spaceLg
                Text {
                    Layout.fillWidth: true
                    text: root.pending !== "" ? root.actionFor(root.pending).confirm : ""
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Item { Layout.fillWidth: true }
                    NxButton {
                        text: qsTr("Cancel")
                        onClicked: root.pending = ""
                    }
                    NxButton {
                        id: confirmButton
                        text: root.pending !== "" ? root.actionFor(root.pending).label : ""
                        kind: "danger"
                        onVisibleChanged: if (visible) forceActiveFocus()
                        onClicked: root.perform(root.pending)
                    }
                }
            }
        }

        Text {
            Layout.alignment: Qt.AlignHCenter
            visible: root.pending === ""
            text: session.fullName + "   ·   " + session.hostName
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }
}
