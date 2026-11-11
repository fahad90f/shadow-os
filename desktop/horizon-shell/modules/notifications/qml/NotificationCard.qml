// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import org.shadow.ui

// One notification: as a popup, or as an entry of the notification centre.
//
// Everything shown here was cleaned by the server (NotificationText): the
// summary and labels are plain text, the body is escaped text with the tags
// <b> <i> <u> <a href="N"> <br> only, and pictures come from the shell's own
// image provider — nothing a client sent is loaded or interpreted here.
Rectangle {
    id: root

    property int notificationId: 0
    property string appName: ""
    property string summary: ""
    property string body: ""
    property string bodyPlain: ""
    property string imageSource: ""
    property string iconSource: ""
    property bool critical: false
    property bool unread: false
    // A click does something: the notification has a default action.
    property bool interactive: false
    property var actions: []
    property int progress: -1
    property double timestamp: 0
    // Popup (application name, always a picture, close button always shown)
    // or centre entry (time, close button on hover).
    property bool popup: true
    property bool current: false
    property double now: 0
    property int bodyLines: 4

    signal activated()
    signal actionInvoked(string key)
    signal linkActivated(string link)
    signal dismissed()

    readonly property bool hovered: cardHover.hovered
    readonly property string pictureSource: imageSource !== "" ? imageSource : (popup ? iconSource : "")
    readonly property bool showPicture: popup || imageSource !== ""

    function timeText() {
        if (timestamp <= 0 || now <= 0) return ""
        const seconds = Math.max(0, (now - timestamp) / 1000)
        if (seconds < 60) return qsTr("now")
        if (seconds < 3600) return qsTr("%1 min").arg(Math.floor(seconds / 60))
        const date = new Date(timestamp)
        if (seconds < 86400) return Qt.formatTime(date, shellSettings.clock24h ? "HH:mm" : "h:mm AP")
        return Qt.formatDate(date, "d MMM")
    }

    implicitHeight: content.implicitHeight + Theme.spaceMd * 2
    radius: Theme.radiusMd
    color: popup ? Theme.bgOverlay
        : cardHover.hovered || current ? Theme.bgHover : Theme.bgRaised
    border.width: Theme.borderWidth
    border.color: critical ? Theme.alpha(Theme.danger, 0.75)
        : current ? Theme.accent
        : popup ? Theme.borderStrong : Theme.borderSubtle
    Behavior on color { ColorAnimation { duration: Theme.motionFast } }

    Accessible.role: Accessible.Notification
    Accessible.name: (appName !== "" ? appName + ": " : "") + summary
    Accessible.description: bodyPlain

    HoverHandler { id: cardHover; cursorShape: root.popup || root.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    // Takes the click only if no button or link inside did.
    TapHandler {
        gesturePolicy: TapHandler.ReleaseWithinBounds
        enabled: root.popup || root.interactive
        onTapped: root.activated()
    }

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceMd

        // ---- picture ------------------------------------------------------------
        Item {
            visible: root.showPicture
            Layout.alignment: Qt.AlignTop
            implicitWidth: 40
            implicitHeight: 40

            Image {
                anchors.fill: parent
                visible: root.pictureSource !== ""
                source: root.pictureSource
                sourceSize: Qt.size(Math.round(40 * Screen.devicePixelRatio), Math.round(40 * Screen.devicePixelRatio))
                fillMode: Image.PreserveAspectFit
                asynchronous: false
                cache: false
                smooth: true
            }
            Rectangle {
                anchors.fill: parent
                visible: root.pictureSource === ""
                radius: width / 2
                color: root.critical ? Theme.dangerFill : Theme.accentFill
                NxIcon {
                    anchors.centerIn: parent
                    name: root.critical ? "alert-triangle" : "bell"
                    size: Theme.icon
                    color: root.critical ? Theme.danger : Theme.accent
                }
            }
        }

        // ---- text -----------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.spaceXxs

            Text {
                Layout.fillWidth: true
                visible: root.popup && text !== ""
                text: root.appName
                textFormat: Text.PlainText
                color: root.critical ? Theme.danger : Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
                maximumLineCount: 1
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                visible: root.summary !== "" || !root.popup

                Rectangle {
                    visible: !root.popup && root.unread
                    Layout.alignment: Qt.AlignVCenter
                    implicitWidth: 6
                    implicitHeight: 6
                    radius: 3
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    text: root.summary
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: Theme.weightSemibold
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                    maximumLineCount: 2
                }
                Text {
                    visible: !root.popup && text !== ""
                    Layout.alignment: Qt.AlignTop
                    text: root.timeText()
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeCaption
                }
            }
            Text {
                id: bodyText
                Layout.fillWidth: true
                visible: root.body !== ""
                text: root.body
                // The subset the server emits; see the comment at the top.
                textFormat: Text.StyledText
                color: Theme.textSecondary
                linkColor: Theme.accent
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                maximumLineCount: root.bodyLines
                onLinkActivated: link => root.linkActivated(link)
                HoverHandler {
                    enabled: bodyText.hoveredLink !== ""
                    cursorShape: Qt.PointingHandCursor
                }
            }

            // ---- progress ("value" hint) -----------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                visible: root.progress >= 0
                spacing: Theme.spaceSm
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 4
                    radius: 2
                    color: Theme.alpha(Theme.textMuted, 0.3)
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: qsTr("%1 percent").arg(root.progress)
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(100, root.progress)) / 100
                        height: parent.height
                        radius: parent.radius
                        color: Theme.accent
                        Behavior on width { NumberAnimation { duration: Theme.motionFast } }
                    }
                }
                Text {
                    text: qsTr("%1%").arg(Math.max(0, root.progress))
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeCaption
                    horizontalAlignment: Text.AlignRight
                    Layout.minimumWidth: 32
                }
            }

            // ---- actions --------------------------------------------------------------
            Flow {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceSm
                visible: actionRepeater.count > 0
                spacing: Theme.spaceSm
                Repeater {
                    id: actionRepeater
                    model: root.actions
                    NxButton {
                        required property var modelData
                        text: modelData.label
                        kind: "secondary"
                        implicitHeight: 28
                        font.pixelSize: Theme.fontSizeSmall
                        onClicked: root.actionInvoked(modelData.key)
                    }
                }
            }
        }

        // ---- close ---------------------------------------------------------------------
        NxIconButton {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: -Theme.spaceXs
            Layout.rightMargin: -Theme.spaceXs
            implicitWidth: 26
            implicitHeight: 26
            iconSize: Theme.iconSmall
            iconName: "close"
            tooltip: qsTr("Dismiss")
            opacity: root.popup || cardHover.hovered || root.current || activeFocus ? 1 : 0
            onClicked: root.dismissed()
        }
    }
}
