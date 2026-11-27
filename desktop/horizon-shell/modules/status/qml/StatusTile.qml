// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A quick-settings tile: a switch with an icon, a title and its current
// state, and optionally a second button that opens the full settings.
Rectangle {
    id: root

    property string icon: ""
    property string title: ""
    property string subtitle: ""
    // The thing the tile switches is on.
    property bool active: false
    // false: the tile only shows a state; 'reason' says why it cannot switch.
    property bool switchable: true
    property string reason: ""
    property bool hasDetails: false
    property string detailsTooltip: ""
    // false: the tile is a button that opens something (its state is shown,
    // not switched here), so it is not announced as a toggle.
    property bool checkable: true
    // "" | "warning" | "danger": the state needs attention.
    property string tone: ""
    // Shown on hover while the tile can be used ('reason' while it cannot).
    property string tooltip: ""
    // 0..1 while the popup opens (PopupSurface.rowAppear): the tile fades in
    // and what it shows drops 4 px into place. The tile itself, and what it
    // says about its state, never moves.
    property real appear: 1
    readonly property color toneColor: tone === "danger" ? Theme.danger : tone === "warning" ? Theme.warning : Theme.accent

    signal toggled()
    signal detailsRequested()

    Layout.fillWidth: true
    Layout.preferredWidth: 1      // equal columns whatever the texts
    implicitHeight: Theme.rowHeightLarge
    radius: Theme.radiusMd
    color: active ? Theme.accentFill : Theme.hoverOverlay
    border.width: main.activeFocus ? Theme.focusRing : Theme.borderWidth
    border.color: main.activeFocus ? Theme.accent : tone !== "" || active ? Theme.alpha(toneColor, 0.45) : Theme.borderSubtle
    Behavior on color { ColorAnimation { duration: Theme.motionFast } }
    opacity: appear

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            id: main
            Layout.fillWidth: true
            Layout.fillHeight: true
            activeFocusOnTab: root.switchable
            Accessible.role: Accessible.Button
            Accessible.name: root.title
            Accessible.description: root.subtitle
            Accessible.checkable: root.switchable && root.checkable
            Accessible.checked: root.checkable && root.active
            Accessible.onPressAction: if (root.switchable) root.toggled()
            Keys.onReturnPressed: if (root.switchable) root.toggled()
            Keys.onSpacePressed: if (root.switchable) root.toggled()

            Rectangle {
                anchors.fill: parent
                radius: root.radius
                color: tap.pressed ? Theme.pressedOverlay : hover.hovered && root.switchable ? Theme.hoverOverlay : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: root.hasDetails ? 0 : Theme.spaceMd
                spacing: Theme.spaceMd
                opacity: root.switchable ? 1 : Theme.opacityDisabled
                transform: Translate { y: -4 * (1 - root.appear) }

                Rectangle {
                    implicitWidth: Theme.controlHeight
                    implicitHeight: Theme.controlHeight
                    radius: width / 2
                    color: root.tone !== "" ? root.toneColor : root.active ? Theme.accent : Theme.bgPressed
                    Behavior on color { ColorAnimation { duration: Theme.motionFast } }
                    NxIcon {
                        anchors.centerIn: parent
                        name: root.icon
                        size: Theme.iconSmall + 2
                        color: root.tone !== "" || root.active ? Theme.textOnAccent : Theme.textPrimary
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: root.title
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                    // A tone shows in the icon circle and the border; the
                    // caption-size text stays in text colours (warning and
                    // danger as text are below 4.5:1 on the tile:
                    // tests/tst_status_tile_contrast.qml).
                    Text {
                        objectName: "statusTileSubtitle"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: root.subtitle
                        color: root.tone !== "" ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        font.weight: root.tone !== "" ? Theme.weightSemibold : Theme.weightRegular
                        elide: Text.ElideRight
                    }
                }
            }

            HoverHandler { id: hover; cursorShape: root.switchable ? Qt.PointingHandCursor : Qt.ArrowCursor }
            // Exclusive: a tap on a tile must not also reach the popup behind it.
            TapHandler {
                id: tap
                enabled: root.switchable
                gesturePolicy: TapHandler.ReleaseWithinBounds
                onTapped: root.toggled()
            }

            ToolTip.visible: hover.hovered && ToolTip.text !== ""
            ToolTip.text: !root.switchable ? root.reason : root.tooltip
            ToolTip.delay: 500
        }

        NxIconButton {
            visible: root.hasDetails
            Layout.rightMargin: Theme.spaceXs
            iconName: "chevron-right"
            iconSize: Theme.iconSmall
            tooltip: root.detailsTooltip
            onClicked: root.detailsRequested()
        }
    }
}
