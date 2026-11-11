// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// Transient in-window feedback. Place one at the bottom of the window:
//   NxToaster { id: toaster; anchors.fill: parent }
//   toaster.show(qsTr("Copied"), "success")
Item {
    id: root

    property int duration: 3800
    // A toast is as wide as its text, up to this width; longer text wraps.
    // The limit also keeps a toast from covering the whole window (sidebar
    // included) however long the message is.
    property real maxToastWidth: 480
    readonly property real toastWidthLimit: Math.max(120, Math.min(maxToastWidth, width - Theme.spaceXxl * 2))
    z: 1000

    function show(text, kind) {
        toasts.append({ text: text, kind: kind || "info" })
    }

    ListModel { id: toasts }

    Column {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: Theme.spaceXl
        spacing: Theme.spaceSm

        Repeater {
            model: toasts
            Rectangle {
                id: toast
                required property string text
                required property string kind
                required property int index

                readonly property color tone: kind === "success" ? Theme.success
                    : kind === "warning" ? Theme.warning
                    : kind === "danger" ? Theme.danger : Theme.accent

                anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
                width: Math.min(root.toastWidthLimit, row.implicitWidth + Theme.spaceLg * 2)
                height: Math.max(40, row.implicitHeight + Theme.spaceMd * 2)
                radius: Theme.radiusMd
                color: Theme.bgOverlay
                border.width: Theme.borderWidth
                border.color: Theme.alpha(tone, 0.5)
                Accessible.role: Accessible.Notification
                Accessible.name: text

                RowLayout {
                    id: row
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, root.toastWidthLimit - Theme.spaceLg * 2)
                    spacing: Theme.spaceSm
                    Rectangle {
                        implicitWidth: 8; implicitHeight: 8; radius: 4; color: toast.tone
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Math.max(0, (Theme.fontSizeBody * 1.35 - 8) / 2)
                    }
                    Text {
                        Layout.fillWidth: true
                        text: toast.text
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                    }
                }

                NumberAnimation on opacity { from: 0; to: 1; duration: Theme.motionNormal }
                Timer {
                    interval: root.duration
                    running: true
                    onTriggered: toasts.remove(toast.index)
                }
                // Exclusive grab: dismissing a toast must not also tap what lies under it.
                TapHandler {
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: toasts.remove(toast.index)
                }
            }
        }
    }
}
