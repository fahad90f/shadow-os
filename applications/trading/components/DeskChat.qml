// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The conversation with the desk: the messages, and the box to write in.
//
// The box is never locked. A message is handed to the desk, which accepts it
// at once; only then is the box emptied. Anything typed meanwhile stays, and
// a second message can be sent while answers to earlier ones are still being
// written. A draft has its own token, so pressing Enter twice sends it once.
GlowCard {
    id: root

    padding: 0

    function send() {
        var text = field.text
        if (text.trim() === "")
            return
        var result = desk.post(text, field.token)
        if (result.accepted) {
            // Emptied only now, and only of what was sent.
            if (field.text === text)
                field.clear()
            else if (field.text.indexOf(text) === 0)
                field.text = field.text.substring(text.length)
            field.token = ""
            error.text = ""
            list.follow = true
        } else {
            error.text = result.error
        }
        field.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- the messages ------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                clip: true
                spacing: Theme.spaceSm + 2
                model: desk.messages
                boundsBehavior: Flickable.StopAtBounds
                // Stay at the newest message unless the reader has scrolled up.
                property bool follow: true
                onMovementEnded: follow = atYEnd
                onCountChanged: if (follow) Qt.callLater(positionViewAtEnd)
                onContentHeightChanged: if (follow && !moving) Qt.callLater(positionViewAtEnd)
                Component.onCompleted: positionViewAtEnd()
                ScrollBar.vertical: ScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Conversation with the desk")

                delegate: DeskMessage {
                    width: ListView.view.width
                }
            }

            NxEmptyState {
                anchors.centerIn: parent
                width: Math.min(parent.width - Theme.spaceLg * 2, 540)
                visible: list.count === 0
                icon: "zap"
                title: qsTr("The desk is listening")
                message: qsTr("Talk about any market as you would to an analyst next to you: “BTC is approaching 95,000”, “RSI shows bearish divergence”, “what about entering short?”. Every message is read the moment it arrives and adds to the analysis on the right; ask a question and the answer is written while you keep typing.")
            }

            NxButton {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.spaceSm
                visible: !list.follow && list.count > 0
                kind: "secondary"
                iconName: "arrow-down"
                text: qsTr("Newest")
                onClicked: { list.follow = true; list.positionViewAtEnd() }
            }
        }

        // ---- the box to write in --------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.alpha(Theme.accent, 0.25)
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spaceMd
            spacing: Theme.spaceSm

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Math.max(field.implicitHeight, 44), 132)
                TextArea {
                    id: field
                    // A new draft gets a new token the moment it starts.
                    property string token: ""
                    onTextChanged: if (text !== "" && token === "") token = Date.now() + "-" + Math.random().toString(36).slice(2)
                    placeholderText: qsTr("Message the desk…   Enter sends, Shift+Enter starts a new line")
                    wrapMode: TextArea.Wrap
                    font.pixelSize: Theme.fontSizeBody
                    focus: true
                    Accessible.name: qsTr("Message to the desk")
                    Keys.onPressed: (event) => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && !(event.modifiers & Qt.ShiftModifier) && !inputMethodComposing) {
                            event.accepted = true
                            root.send()
                        }
                    }
                }
            }
            NxButton {
                kind: "primary"
                iconName: "arrow-up"
                text: qsTr("Send")
                enabled: field.text.trim() !== ""
                onClicked: root.send()
                Accessible.name: qsTr("Send the message")
            }
        }
        Label {
            id: error
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMd
            Layout.bottomMargin: text === "" ? 0 : Theme.spaceSm
            visible: text !== ""
            color: Theme.warning
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
        }
    }
}
