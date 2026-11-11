// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.installer

// A password (or passphrase) with its confirmation and plain remarks about
// concrete weaknesses. There is no strength score or meter: nothing here
// claims to know how long an attack would take.
//
// The text never leaves this item except through committed(): it is emitted
// with the secret when both fields are acceptable and equal, and with ""
// whenever that stops being true.
ColumnLayout {
    id: root

    property string kind: "password"        // "password" | "passphrase" — which engine rule applies
    property string label: qsTr("Password")
    property string confirmLabel: qsTr("Repeat password")
    property var personalWords: []
    // Whether the back-end currently holds a secret of this kind.
    property bool held: false

    signal committed(string secret)

    readonly property string problem: first.text === "" ? ""
        : kind === "passphrase" ? Installer.passphraseProblem(first.text) : Installer.passwordProblem(first.text)
    readonly property bool mismatch: second.text !== "" && first.text !== second.text
    readonly property var facts: Installer.passwordFacts(first.text, personalWords)
    readonly property bool touched: first.text !== "" || second.text !== ""

    function evaluate() {
        const good = first.text !== "" && problem === "" && first.text === second.text
        committed(good ? first.text : "")
    }
    function clear() { first.clear(); second.clear() }

    spacing: Theme.spaceSm
    Layout.fillWidth: true

    Connections {
        target: Installer
        function onSecretsScrubbed() { root.clear() }
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        uniformCellWidths: true
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceXs

        Text {
            Layout.fillWidth: true
            text: root.label
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
        Text {
            Layout.fillWidth: true
            text: root.confirmLabel
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
        NxPasswordField {
            id: first
            Layout.fillWidth: true
            Layout.preferredWidth: 10
            Accessible.name: root.label
            onTextEdited: root.evaluate()
        }
        NxPasswordField {
            id: second
            Layout.fillWidth: true
            Layout.preferredWidth: 10
            Accessible.name: root.confirmLabel
            revealed: first.revealed
            onTextEdited: root.evaluate()
        }
    }

    Text {
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.WordWrap
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        color: root.problem !== "" || root.mismatch ? Theme.danger : root.held ? Theme.success : Theme.textMuted
        text: root.problem !== "" ? root.problem
            : root.mismatch ? qsTr("The two entries are different.")
            : first.text !== "" && second.text === "" ? qsTr("Enter it a second time to rule out a typing mistake.")
            : root.held && root.touched ? qsTr("Both entries match.")
            : root.held ? qsTr("A value supplied by the test preset is in use; typing replaces it.") : ""
    }

    // Facts and remarks — no score.
    ColumnLayout {
        Layout.fillWidth: true
        visible: first.text !== ""
        spacing: 2
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            text: {
                const f = root.facts
                const kinds = []
                if (f.lower) kinds.push(qsTr("letters"))
                if (f.upper) kinds.push(qsTr("capital letters"))
                if (f.digits) kinds.push(qsTr("digits"))
                if (f.symbols) kinds.push(qsTr("symbols or spaces"))
                return (f.length === 1 ? qsTr("1 character") : qsTr("%1 characters").arg(f.length)) + " · " + kinds.join(", ")
            }
        }
        Repeater {
            model: root.facts.remarks
            RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                NxIcon { name: "alert-triangle"; size: Theme.iconSmall; color: Theme.warning; Layout.alignment: Qt.AlignTop }
                Text {
                    Layout.fillWidth: true
                    text: parent.modelData
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }
}
