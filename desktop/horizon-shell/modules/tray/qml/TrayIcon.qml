// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import org.shadow.ui

// One status notifier item in the panel. Looks and behaves like a
// PanelButton, but shows the icon the application provides.
//
//   left click     Activate (or the menu, if the item is nothing but a menu
//                  or does not implement Activate)
//   middle click   SecondaryActivate
//   right click    the item's menu (or ContextMenu, if it exports none)
//   wheel          Scroll
Item {
    id: root

    property var item: null         // TrayItem
    property var shell: null
    property var panel: null

    readonly property bool menuShown: tray.menuItem === item && item !== null
    readonly property int iconSize: Theme.iconSmall + 2

    // The panel spans its screen from the left edge, so a position in the
    // panel's window is a position on that screen (a layer surface cannot
    // ask where it is).
    function screenX() { return Math.round(root.mapToItem(null, root.width / 2, 0).x) }
    function globalX() { return (panel && panel.screen ? panel.screen.virtualX : 0) + screenX() }
    function globalY() { return (panel && panel.screen ? panel.screen.virtualY : 0) + Theme.panelHeight }
    function rgb(c) { const s = c.toString(); return s.length === 9 ? s.substring(3) : s.substring(1) }

    function openMenu() {
        tray.hideToolTip()
        tray.showMenu(item, screenX())
    }
    function primary() {
        tray.hideToolTip()
        if (item.hasMenu && item.itemIsMenu) openMenu()
        else item.activate(globalX(), globalY(), panel)
    }
    function context() {
        if (item.hasMenu) openMenu()
        else { tray.hideToolTip(); item.contextMenu(globalX(), globalY(), panel) }
    }

    visible: item !== null && item.shown
    width: iconSize + Theme.spaceSm * 2
    height: Theme.panelHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: item ? item.toolTipTitle : ""
    Accessible.description: item ? item.toolTipText : ""
    Keys.onReturnPressed: primary()
    Keys.onSpacePressed: primary()
    Keys.onMenuPressed: context()

    Connections {
        target: root.item
        // Activate is not something this item does: offer its menu instead.
        function onActivateUnsupported() { if (root.item.hasMenu) root.openMenu() }
    }

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        radius: Theme.radiusSm
        color: root.menuShown ? Theme.accentFill
            : leftTap.pressed ? Theme.pressedOverlay
            : hover.hovered || root.activeFocus ? Theme.hoverOverlay : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
    }

    Image {
        id: picture
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        visible: root.item !== null && root.item.hasIcon
        // "fg"/"bg": a symbolic icon drawn for another background is recoloured.
        source: root.item && root.item.hasIcon
            ? root.item.iconSource + "?fg=" + root.rgb(Theme.textPrimary) + "&bg=" + root.rgb(Theme.bgSurface)
            : ""
        sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
        asynchronous: false         // rendered on the GUI thread (icon theme)
        cache: false
        smooth: true

        SequentialAnimation on opacity {
            running: root.item !== null && root.item.needsAttention && root.visible && !Appearance.reduceMotion
            loops: Animation.Infinite
            alwaysRunToEnd: true
            NumberAnimation { to: 0.45; duration: 520 }
            NumberAnimation { to: 1.0; duration: 520 }
        }
    }
    // An item without any usable icon still has to be reachable.
    NxIcon {
        anchors.centerIn: parent
        visible: root.item !== null && !root.item.hasIcon
        name: "app-generic"
        size: root.iconSize
        color: Theme.textSecondary
    }
    Rectangle {
        visible: root.item !== null && root.item.needsAttention
        anchors.right: picture.right
        anchors.rightMargin: -3
        anchors.top: picture.top
        anchors.topMargin: -3
        width: 7
        height: 7
        radius: 3.5
        color: Theme.warning
        border.width: 1
        border.color: Theme.bgSurface
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
        onHoveredChanged: {
            if (hovered) toolTipDelay.restart()
            else { toolTipDelay.stop(); tray.hideToolTip() }
        }
    }
    Timer {
        id: toolTipDelay
        interval: 600
        onTriggered: if (hover.hovered && !root.menuShown && root.item) tray.showToolTip(root.item, root.screenX())
    }
    TapHandler { id: leftTap; acceptedButtons: Qt.LeftButton; onTapped: root.primary() }
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: root.context() }
    TapHandler {
        acceptedButtons: Qt.MiddleButton
        onTapped: { tray.hideToolTip(); root.item.secondaryActivate(root.globalX(), root.globalY(), root.panel) }
    }
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => root.item.scroll(event.angleDelta.x, event.angleDelta.y)
    }
}
