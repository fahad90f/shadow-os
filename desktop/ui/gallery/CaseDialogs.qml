// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Test page: a modal Dialog with standard buttons over content that reacts
// to taps.
//   * the accept button of the DialogButtonBox is the highlighted one;
//   * a tap on a control inside the dialog (NxSegmented, a Button), on the
//     dialog's own background or on the dimmed area never reaches the
//     pointer handlers of the content underneath.
// Pointer tests (gallery/tests/pointer-tests.sh) read the CASE-GEOMETRY and
// CASE-EVENT lines this page prints.
NxWindow {
    id: window
    title: "ShadowUI Gallery — dialogs"
    width: 900
    height: 640

    property int underTaps: 0          // TapHandler on the page background
    property int underRowClicks: 0     // clickable NxRows under the dialog
    property int underButtonClicks: 0  // a Button under the dialog
    property int segmentActivations: 0
    property string lastResult: ""

    function report(what) { console.log("CASE-EVENT " + what) }
    function centre(item) {
        const p = item.mapToItem(null, item.width / 2, item.height / 2)
        return Math.round(p.x) + " " + Math.round(p.y)
    }
    function reportGeometry() {
        console.log("CASE-GEOMETRY segment1 " + centre(segments.children[0].children[1]))
        console.log("CASE-GEOMETRY segment2 " + centre(segments.children[0].children[2]))
        console.log("CASE-GEOMETRY inner-button " + centre(innerButton))
        console.log("CASE-GEOMETRY dialog-body " + centre(bodyLabel))
        console.log("CASE-GEOMETRY ok " + centre(dialog.standardButton(Dialog.Ok)))
        console.log("CASE-GEOMETRY reopen " + centre(reopen))
    }

    // ---- content underneath: every part of it reacts to a tap -------------
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase
        TapHandler { onTapped: { window.underTaps++; window.report("under-tap") } }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceMd
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Underneath the dialog — taps here: background " + window.underTaps + ", rows " + window.underRowClicks
                  + ", button " + window.underButtonClicks + ". Dialog: segment activations " + window.segmentActivations
                  + ", result “" + window.lastResult + "”."
        }
        NxButton { id: reopen; text: "Open the dialog again"; onClicked: dialog.open() }
        NxSection {
            Layout.fillWidth: true
            title: "Clickable rows (must not react while the dialog is open)"
            Repeater {
                model: 7
                NxRow {
                    required property int index
                    title: "Row " + (index + 1)
                    subtitle: "TapHandler-based"
                    chevron: true
                    onClicked: { window.underRowClicks++; window.report("under-row " + index) }
                }
            }
        }
        Button {
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: "A Button filling the rest"
            onClicked: { window.underButtonClicks++; window.report("under-button") }
        }
    }

    Dialog {
        id: dialog
        title: "Format the report"
        standardButtons: Dialog.Ok | Dialog.Cancel
        width: 460
        onAccepted: { window.lastResult = "accepted"; window.report("accepted") }
        onRejected: { window.lastResult = "rejected"; window.report("rejected") }
        onOpened: window.reportGeometry()

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.spaceLg
            Label {
                id: bodyLabel
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "Standard buttons: OK is the accept button and is drawn highlighted. "
                      + "The selector below keeps its own state."
            }
            NxSegmented {
                id: segments
                model: ["Text", "JSON", "CSV"]
                onActivated: (index) => { window.segmentActivations++; window.report("segment " + index + " current " + currentIndex) }
            }
            NxButton {
                id: innerButton
                text: "A button inside the dialog"
                onClicked: window.report("inner-button")
            }
        }
    }

    Component.onCompleted: dialog.open()

    CaseChecks {
        name: "dialogs"
        interval: 400
        steps: [
            () => {
                verify(dialog.opened, "the dialog is open")
                const ok = dialog.standardButton(Dialog.Ok), cancel = dialog.standardButton(Dialog.Cancel)
                verify(ok !== null && cancel !== null, "the button box created OK and Cancel")
                verify(ok.highlighted === true, "the accept button (OK) is highlighted")
                verify(cancel.highlighted === false, "the reject button (Cancel) is not highlighted")
                compare(ok.DialogButtonBox.buttonRole, DialogButtonBox.AcceptRole, "OK has the accept role")
            },
            () => { dialog.standardButtons = Dialog.Yes | Dialog.No | Dialog.Apply | Dialog.Close },
            () => {
                verify(dialog.standardButton(Dialog.Yes).highlighted, "Yes is highlighted")
                verify(dialog.standardButton(Dialog.Apply).highlighted, "Apply is highlighted")
                verify(!dialog.standardButton(Dialog.No).highlighted, "No is not highlighted")
                verify(!dialog.standardButton(Dialog.Close).highlighted, "Close is not highlighted")
                dialog.standardButtons = Dialog.Ok | Dialog.Cancel
            },
            () => verify(dialog.standardButton(Dialog.Ok).highlighted, "OK is highlighted again after the buttons changed")
        ]
    }
}
