// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// The top panel: launcher button, workspaces and the active window on the
// left; clock in the centre; status area and session button on the right.
ShellSurface {
    id: root

    property bool launcherOpen: false
    property bool sessionMenuOpen: false
    property bool clockOpen: false
    // The clock is a button only when something (the calendar module) will
    // answer a click on it.
    property bool clockEnabled: false
    // Where the items of optional shell modules (system tray, status
    // indicators, notifications) are placed.
    readonly property Item moduleArea: moduleRow

    signal launcherRequested()
    signal sessionMenuRequested()
    signal clockRequested()

    layer: "top"
    scope: "shadow-panel"
    anchorTop: true; anchorLeft: true; anchorRight: true
    exclusiveZone: Theme.panelHeight
    width: screen ? screen.width : 1280
    height: Theme.panelHeight
    visible: true

    property date now: new Date()
    Timer {
        interval: shellSettings.clockSeconds ? 1000 : 5000
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = new Date()
    }

    Rectangle {
        anchors.fill: parent
        color: panelBlur.glass ? Theme.glassPanel : Theme.bgSurface
        BlurBehind { id: panelBlur; anchors.fill: parent; surface: root }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.borderSubtle
        }
    }

    // ---- left ---------------------------------------------------------------
    RowLayout {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceXs
        height: parent.height
        spacing: Theme.spaceXs

        PanelButton {
            icon: "shadow"
            iconColor: Theme.accent
            checked: root.launcherOpen
            tooltip: qsTr("Applications")
            onClicked: root.launcherRequested()
        }

        Row {
            visible: workspaces.available && workspaces.count > 1
            spacing: 4
            Layout.leftMargin: Theme.spaceXs
            Layout.alignment: Qt.AlignVCenter
            Repeater {
                model: workspaces.list
                Rectangle {
                    id: pill
                    required property var modelData
                    required property int index
                    readonly property bool current: modelData.id === workspaces.current
                    width: current ? 22 : 8
                    height: 8
                    radius: 4
                    color: current ? Theme.accent : pillHover.hovered ? Theme.textSecondary : Theme.textMuted
                    Behavior on width { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Workspace %1").arg(index + 1)
                    HoverHandler { id: pillHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { margin: 6; onTapped: workspaces.switchTo(pill.modelData.id) }
                }
            }
        }

        Text {
            Layout.leftMargin: Theme.spaceMd
            Layout.alignment: Qt.AlignVCenter
            visible: windows.activeAppName !== ""
            text: windows.activeAppName
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Theme.weightSemibold
        }
        Text {
            Layout.alignment: Qt.AlignVCenter
            Layout.maximumWidth: Math.max(80, root.width / 2 - 360)
            visible: windows.activeTitle !== "" && windows.activeTitle !== windows.activeAppName
            text: windows.activeTitle
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
    }

    // ---- centre ---------------------------------------------------------------
    PanelButton {
        anchors.centerIn: parent
        text: {
            const time = Qt.formatTime(root.now, shellSettings.clock24h
                ? (shellSettings.clockSeconds ? "HH:mm:ss" : "HH:mm")
                : (shellSettings.clockSeconds ? "h:mm:ss AP" : "h:mm AP"))
            return shellSettings.clockDate ? Qt.formatDate(root.now, "ddd d MMM") + "   " + time : time
        }
        tooltip: Qt.formatDate(root.now, Qt.locale().dateFormat(Locale.LongFormat))
        checked: root.clockOpen
        interactive: root.clockEnabled
        onClicked: root.clockRequested()
    }

    // ---- right ---------------------------------------------------------------
    RowLayout {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXs
        height: parent.height
        spacing: 0

        Row {
            id: moduleRow
            Layout.alignment: Qt.AlignVCenter
            height: parent.height
            spacing: 0
        }
        PanelButton {
            visible: session.liveSession
            icon: "download"
            text: qsTr("Install")
            iconColor: Theme.accent
            tooltip: qsTr("Install Shadow OS on this computer")
            onClicked: session.run("shadow-installer")
        }
        PanelButton {
            icon: "power"
            checked: root.sessionMenuOpen
            tooltip: qsTr("Lock, log out or power off")
            onClicked: root.sessionMenuRequested()
        }
    }
}
