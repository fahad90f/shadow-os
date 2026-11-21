// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.shadow.assistant

// A code block (or tool output): a quiet slab with its language, a copy
// action, selectable monospace text and horizontal scrolling instead of
// wrapping, so commands are never broken in the middle.
Rectangle {
    id: slab
    required property StageModel stage
    property string code: ""
    property string lang: ""
    property bool growing: false        // still streaming in
    property bool plainLabel: false
    property real maxBodyHeight: 0      // 0 = no limit
    property bool showHeader: true

    implicitHeight: (showHeader ? head.height + 1 : 0) + bodyBox.height
    radius: Pe.rCard
    color: Pe.codeBg
    border.width: 1
    border.color: Pe.line
    clip: true
    Accessible.role: Accessible.Grouping
    Accessible.name: (lang !== "" ? lang : qsTr("Code")) + ": " + code.slice(0, 200)

    Item {
        id: head
        visible: slab.showHeader
        width: parent.width
        height: slab.showHeader ? 34 : 0
        Row {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: slab.growing ? Pe.cyan : Pe.alpha(Pe.violet, 0.8)
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                text: slab.lang !== "" ? (slab.plainLabel ? slab.lang : slab.lang.toUpperCase()) : qsTr("CODE")
                font.family: Pe.sans
                font.pixelSize: Pe.label
                font.weight: Font.DemiBold
                font.letterSpacing: 1.2
                color: Pe.inkMute
            }
        }
        PeButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            kind: "ghost"
            iconName: "copy"
            text: qsTr("Copy")
            tip: qsTr("Copy this code")
            onClicked: slab.stage.copy(slab.code)
        }
    }
    Rectangle {
        visible: slab.showHeader
        y: head.height
        width: parent.width
        height: 1
        color: Pe.line
    }
    Flickable {
        id: bodyBox
        y: slab.showHeader ? head.height + 1 : 0
        width: parent.width
        height: slab.maxBodyHeight > 0 ? Math.min(codeText.implicitHeight, slab.maxBodyHeight) : codeText.implicitHeight
        contentWidth: Math.max(width, codeText.implicitWidth)
        contentHeight: codeText.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: slab.maxBodyHeight > 0 ? Flickable.AutoFlickDirection : Flickable.HorizontalFlick
        readonly property bool wider: contentWidth > width + 1
        readonly property bool taller: contentHeight > height + 1
        interactive: wider || taller
        // lines that run past the edge: Tab reaches the slab and the arrow keys scroll it
        activeFocusOnTab: wider || taller
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("%1, scrollable").arg(slab.lang !== "" ? slab.lang : qsTr("Code"))
        Keys.onLeftPressed: contentX = Math.max(originX, contentX - 48)
        Keys.onRightPressed: contentX = Math.min(originX + contentWidth - width, contentX + 48)
        Keys.onUpPressed: (event) => {
            event.accepted = taller
            if (taller)
                contentY = Math.max(originY, contentY - 40)
        }
        Keys.onDownPressed: (event) => {
            event.accepted = taller
            if (taller)
                contentY = Math.min(originY + contentHeight - height, contentY + 40)
        }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Home) {
                contentX = originX
                event.accepted = true
            } else if (event.key === Qt.Key_End) {
                contentX = Math.max(originX, originX + contentWidth - width)
                event.accepted = true
            }
        }
        ScrollBar.horizontal: PeScrollBar { hint: bodyBox.wider }
        ScrollBar.vertical: PeScrollBar { hint: bodyBox.taller }
        TextEdit {
            id: codeText
            width: Math.max(bodyBox.width, implicitWidth)
            padding: 14
            text: slab.code
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.NoWrap
            font.family: Pe.mono
            font.pixelSize: Pe.code
            color: Pe.codeInk
            selectionColor: Pe.alpha(Pe.violet, 0.38)
            selectedTextColor: Pe.ink
            activeFocusOnPress: false
            Accessible.ignored: true
        }
    }
    // the line goes on: a fade at the right edge says so
    Rectangle {
        visible: bodyBox.wider && !bodyBox.atXEnd
        anchors.right: parent.right
        anchors.rightMargin: 1
        y: bodyBox.y
        width: 36
        height: bodyBox.height - (bodyBox.wider ? 6 : 0)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Pe.alpha(Pe.codeBg, 0) }
            GradientStop { position: 1.0; color: Pe.codeBg }
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.width: 2
        border.color: Pe.cyan
        visible: bodyBox.activeFocus
    }
}
