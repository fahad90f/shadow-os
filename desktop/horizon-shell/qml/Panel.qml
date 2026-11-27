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

    // "never" | "smart" (hidden while an application window is open) | "always"
    readonly property string hideMode: shellSettings.panelHide
    readonly property bool wantsHidden: hideMode === "always" || (hideMode === "smart" && windows.openWindows)
    // Shown anyway while the pointer is on it, and while one of its menus or
    // popups (launcher, calendar, session menu, a module's popup) is open.
    property bool peeking: false
    property bool holdOpen: false
    readonly property bool revealed: !wantsHidden || peeking || holdOpen

    layer: "top"
    scope: "shadow-panel"
    anchorTop: true; anchorLeft: true; anchorRight: true
    // Only a panel that never hides reserves space; a hiding one floats over
    // windows when it is shown.
    exclusiveZone: hideMode === "never" ? Theme.panelHeight : 0
    width: screen ? screen.width : 1280
    // Hidden, a thin strip stays at the top edge: touching it with the
    // pointer brings the panel back.
    height: tall ? Theme.panelHeight : 2
    visible: true

    // ---- showing and hiding ------------------------------------------------------
    // What is drawn follows "revealed" through "shown" (0..1). Coming back,
    // the window has its full height at once and the panel fades in while
    // its contents slide down 8 px (Theme.motionFast). Leaving, everything
    // fades (Theme.motionInstant) and only then the window shrinks to the
    // strip: "tall" stays true for as long as something is still drawn.
    // With "Reduce motion" both are the instant jumps they always were.
    property real shown: revealed ? 1 : 0
    Behavior on shown {
        enabled: !Theme.reduceMotion
        NumberAnimation {
            duration: root.revealed ? Theme.motionFast : Theme.motionInstant
            easing.type: Easing.BezierSpline
            easing.bezierCurve: root.revealed ? Theme.easingDecelerate : Theme.easingAccelerate
        }
    }
    readonly property bool tall: revealed || shown > 0

    // ---- start of the session ----------------------------------------------------
    // The panel is there and takes input from the first frame. In the first
    // 0.4 s its background fades in and its three groups (clock, left,
    // right) light up one after the other, each dropping 6 px into place;
    // a hairline in the accent colour grows along the lower edge. Only
    // opacity and an offset of the contents change. The background, which
    // carries the blur, is never moved.
    StartClock { id: start }
    function groupIn(from) { return start.ph(from, from + Theme.motionNormal) }
    function groupOpacity(from) { return (0.2 + 0.8 * groupIn(from)) * shown }
    function groupOffset(from) { return -6 * (1 - groupIn(from)) - (revealed ? 8 * (1 - shown) : 0) }

    HoverHandler {
        id: panelHover
        onHoveredChanged: {
            if (hovered) { hideTimer.stop(); root.peeking = true }
            else hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: 900
        onTriggered: if (!panelHover.hovered) root.peeking = false
    }

    property date now: new Date()
    Timer {
        interval: shellSettings.clockSeconds ? 1000 : 5000
        running: root.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = new Date()
    }

    // The contents keep the full panel height; while the panel is hidden the
    // window is only the trigger strip and the contents are not shown.
    Item {
    id: content
    width: root.width
    height: Theme.panelHeight
    visible: root.tall

    Rectangle {
        anchors.fill: parent
        color: panelBlur.glass ? Theme.glassPanel : Theme.bgSurface
        opacity: start.lin(0, Theme.motionFast) * root.shown
        BlurBehind { id: panelBlur; anchors.fill: parent; surface: root }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.borderSubtle
        }
    }
    // Start of the session: the hairline grows from the centre along the
    // lower edge, then fades into the border under it. Nothing is drawn in
    // the upper rows of the panel.
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.round(parent.width * start.lin(120, Theme.motionPage))
        height: 1
        color: Theme.accent
        opacity: 1 - start.lin(Theme.motionPage, Theme.motionPage + Theme.motionNormal)
        visible: opacity > 0 && width > 0
    }

    // ---- left ---------------------------------------------------------------
    RowLayout {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceXs
        height: parent.height
        spacing: Theme.spaceXs
        opacity: root.groupOpacity(120)
        transform: Translate { y: root.groupOffset(120) }

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
        opacity: root.groupOpacity(60)
        transform: Translate { y: root.groupOffset(60) }
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
        opacity: root.groupOpacity(180)
        transform: Translate { y: root.groupOffset(180) }

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
    } // content
}
