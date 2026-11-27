// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Lock / log out / suspend / restart / power off. Actions that end the
// session ask for confirmation first.
//
// How it opens (windows of the overlay's 'reveal' clock; see
// OverlaySurface): a line grows from the middle of the screen through the
// places of the discs, and the discs light up along it from the middle
// outwards, each followed by its label. The first disc has the keyboard
// from the first frame. The confirmation card is drawn with a clock of its
// own: its outline is traced and brackets close onto it.
OverlaySurface {
    id: root

    scope: "shadow-session-menu"
    scrimOpacity: 0.7

    // The action waiting for confirmation ("" = choosing).
    property string pending: ""
    onOpenChanged: if (open) { pending = ""; focusDisc("") }

    // Gives the keyboard to the disc of an action ("": the first one). The
    // confirmation takes the focus for its button; without this, the menu
    // would come back with no disc selected.
    function focusDisc(key) {
        for (let i = 0; i < discs.count; i++) {
            const entry = discs.itemAt(i)
            if (entry && (key === "" || entry.modelData.key === key)) { entry.takeFocus(); return }
        }
    }

    // The confirmation card's clock (0..1) and the settling of its colours.
    property real confirmReveal: 0
    property real confirmSettle: 0
    SequentialAnimation {
        id: confirmAnimation
        NumberAnimation { target: root; property: "confirmReveal"; from: 0; to: 1; duration: Theme.motionRevealMenu }
        NumberAnimation { target: root; property: "confirmSettle"; from: 0; to: 1; duration: Theme.motionSettle }
    }
    onPendingChanged: {
        confirmAnimation.stop()
        confirmReveal = 0
        confirmSettle = 0
        if (pending !== "") confirmAnimation.start()
    }

    readonly property var actions: [
        { key: "lock", icon: "lock", label: qsTr("Lock"), enabled: session.canLock, confirm: "" },
        { key: "logout", icon: "logout", label: qsTr("Log out"), enabled: true,
          confirm: qsTr("Log out of Shadow OS? Applications that are still open will be closed.") },
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
        Item {
            Layout.alignment: Qt.AlignHCenter
            visible: root.pending === ""
            implicitWidth: discRow.width
            implicitHeight: discRow.height

            // The line the discs sit on: from the middle outwards, a little
            // past the outer discs, with a node at each end. It is drawn in
            // pieces that stop at the edge of each disc, so it never shows
            // through a disc that is still fading in.
            Item {
                id: bus
                readonly property real span: discRow.width / 2 + Theme.spaceXxxl
                readonly property real reach: span * root.ph(0, 0.5)
                readonly property color color: Theme.mix(Theme.accent, Theme.borderStrong, root.settle)
                x: discRow.width / 2
                y: 42
                visible: reach > 0
                Repeater {
                    // Piece k runs from disc k-1 (or the left end) to disc k
                    // (or the right end).
                    model: discs.count + 1
                    Rectangle {
                        required property int index
                        // discRow.width is read so that these are looked up
                        // again once the discs exist and are laid out.
                        readonly property var before: (discRow.width, index > 0 ? discs.itemAt(index - 1) : null)
                        readonly property var after: (discRow.width, index < discs.count ? discs.itemAt(index) : null)
                        readonly property real fromX: index === 0 ? -bus.span
                            : before ? before.x + before.discRight - discRow.width / 2 : 0
                        readonly property real untilX: index === discs.count ? bus.span
                            : after ? after.x + after.discLeft - discRow.width / 2 : 0
                        readonly property real startX: Math.max(fromX, -bus.reach)
                        x: startX
                        y: -1
                        width: Math.max(0, Math.min(untilX, bus.reach) - startX)
                        height: 2
                        color: bus.color
                    }
                }
                Repeater {
                    model: 2
                    Rectangle {
                        required property int index
                        x: (index === 0 ? -bus.reach : bus.reach) - 3
                        y: -3
                        width: 6; height: 6; radius: 3
                        color: Theme.bgBase
                        border.width: 1.5
                        border.color: bus.color
                        opacity: root.ph(0.4, 0.6)
                    }
                }
            }

        Row {
            id: discRow
            spacing: Theme.spaceXl

            Repeater {
                id: discs
                model: root.actions.filter(a => a.enabled)
                Column {
                    id: entry
                    required property var modelData
                    required property int index
                    // Discs light up from the middle outwards.
                    readonly property real start: 0.13 + 0.1 * Math.abs(index - (discs.count - 1) / 2)
                    readonly property real appear: root.ph(start, start + 0.4)
                    function takeFocus() { disc.forceActiveFocus() }
                    // Where the disc's round edge meets the line, in this column.
                    readonly property real discLeft: disc.x
                    readonly property real discRight: disc.x + disc.width
                    spacing: Theme.spaceMd
                    Rectangle {
                        id: disc
                        width: 84; height: 84; radius: 42
                        opacity: entry.appear
                        scale: 0.9 + 0.1 * entry.appear
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
                        opacity: root.ph(entry.start + 0.13, entry.start + 0.53)
                    }
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

            FxFrame {
                anchors.fill: parent
                radius: parent.radius
                origin: "top"
                progress: Math.min(1, root.confirmReveal / 0.8)
                color: Theme.mix(Theme.accent, Theme.border, root.confirmSettle)
                glow: 1 - root.confirmSettle
            }
            FxBrackets {
                anchors.fill: parent
                anchors.margins: -Theme.spaceXs
                readonly property real arrive: Math.max(0, Math.min(1, (root.confirmReveal - 0.4) / 0.6))
                spread: 12 * (1 - arrive) * (1 - arrive)
                strength: arrive * (1 - 0.6 * root.confirmSettle)
            }

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
                        onClicked: { const key = root.pending; root.pending = ""; root.focusDisc(key) }
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
            opacity: root.ph(0.6, 1)
            text: session.fullName + "   ·   " + session.hostName
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }
}
